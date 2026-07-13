/*
 * test_transports.cpp — acceptance tests for the kernel-ACL'd command
 * transports (StreamOscTransport over TCP + UDS stream, UdsDgramOscTransport)
 * against real client sockets. No engine: the transports talk to a recorded
 * ingest callback, exactly as Main.cpp wires them to engine.ingest().
 *
 * Unix-only: the client side uses POSIX sockets directly. The shared
 * framing/registry logic is covered on every platform by the Rust suite
 * (rust/supersonic-osc-net); the named-pipe path is exercised end-to-end by
 * the CI transport harness (test/transport-harness/run.ps1).
 */
#ifndef _WIN32

#include <catch2/catch_test_macros.hpp>

#include "src/native/StreamOscTransport.h"
#include "src/native/UdsDgramOscTransport.h"
#include "OscTestUtils.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using osc_test::message;
using osc_test::parseAddress;

// Record every ingested (packet, token) pair.
struct IngestLog {
    std::mutex mu;
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> items;

    void record(const uint8_t* d, uint32_t n, uint32_t token) {
        std::lock_guard<std::mutex> lk(mu);
        items.emplace_back(token, std::vector<uint8_t>(d, d + n));
    }
    size_t count() {
        std::lock_guard<std::mutex> lk(mu);
        return items.size();
    }
    std::pair<uint32_t, std::vector<uint8_t>> at(size_t i) {
        std::lock_guard<std::mutex> lk(mu);
        return items.at(i);
    }
};

bool waitUntil(const std::function<bool()>& f, int ms = 3000) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (f()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

// A per-process short temp dir — sun_path is ~104 bytes on macOS, so keep
// socket paths compact.
std::string tmpSock(const char* name) {
    std::string dir = "/tmp/ss-tp-" + std::to_string(getpid());
    mkdir(dir.c_str(), 0700);
    std::string p = dir + "/" + name;
    unlink(p.c_str());
    return p;
}

// ── tiny blocking clients ─────────────────────────────────────────────────────

void writeFramed(int fd, const std::vector<uint8_t>& pkt) {
    uint32_t be = htonl(static_cast<uint32_t>(pkt.size()));
    REQUIRE(write(fd, &be, 4) == 4);
    REQUIRE(write(fd, pkt.data(), pkt.size()) == static_cast<ssize_t>(pkt.size()));
}

// Read one length-prefixed frame; empty on EOF/timeout.
std::vector<uint8_t> readFramed(int fd) {
    auto readAll = [&](uint8_t* dst, size_t n) {
        size_t got = 0;
        while (got < n) {
            ssize_t r = read(fd, dst + got, n - got);
            if (r <= 0) return false;
            got += static_cast<size_t>(r);
        }
        return true;
    };
    uint8_t hdr[4];
    if (!readAll(hdr, 4)) return {};
    uint32_t len = (uint32_t(hdr[0]) << 24) | (uint32_t(hdr[1]) << 16)
                 | (uint32_t(hdr[2]) << 8) | uint32_t(hdr[3]);
    std::vector<uint8_t> body(len);
    if (!readAll(body.data(), len)) return {};
    return body;
}

void setRecvTimeout(int fd) {
    timeval tv{2, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

int connectTcp(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(fd >= 0);
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(static_cast<uint16_t>(port));
    inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
    REQUIRE(connect(fd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) == 0);
    setRecvTimeout(fd);
    return fd;
}

int connectUnixStream(const std::string& path) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    REQUIRE(fd >= 0);
    sockaddr_un sa{};
    sa.sun_family = AF_UNIX;
    std::strncpy(sa.sun_path, path.c_str(), sizeof(sa.sun_path) - 1);
    REQUIRE(connect(fd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) == 0);
    setRecvTimeout(fd);
    return fd;
}

int bindUnixDgram(const std::string& path) {
    int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    REQUIRE(fd >= 0);
    sockaddr_un sa{};
    sa.sun_family = AF_UNIX;
    std::strncpy(sa.sun_path, path.c_str(), sizeof(sa.sun_path) - 1);
    // Bind with the precise address length: binding with sizeof(sa) makes the
    // kernel report this peer's address NUL-padded to the full sun_path, and
    // the server would intern (and fail to reply to) that padded path.
    auto len = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + path.size() + 1);
    REQUIRE(bind(fd, reinterpret_cast<sockaddr*>(&sa), len) == 0);
    setRecvTimeout(fd);
    return fd;
}

void sendUnixDgram(int fd, const std::string& to, const std::vector<uint8_t>& pkt) {
    sockaddr_un sa{};
    sa.sun_family = AF_UNIX;
    std::strncpy(sa.sun_path, to.c_str(), sizeof(sa.sun_path) - 1);
    REQUIRE(sendto(fd, pkt.data(), pkt.size(), 0,
                   reinterpret_cast<sockaddr*>(&sa), sizeof(sa))
            == static_cast<ssize_t>(pkt.size()));
}

} // namespace

TEST_CASE("StreamOscTransport/TCP: framed round trip, token routing, disconnect prune",
          "[transport][tcp]") {
    IngestLog log;
    StreamOscTransport t;
    t.setIngest([&](const uint8_t* d, uint32_t n, uint32_t tok) { log.record(d, n, tok); });
    t.setMaxConnections(4);
    t.initialiseTcp(0, "127.0.0.1");  // port 0 → ephemeral, read back below
    REQUIRE(t.start());
    REQUIRE(t.boundPort() > 0);

    int fd = connectTcp(t.boundPort());
    auto probe = message("/status");
    writeFramed(fd, probe.data);
    REQUIRE(waitUntil([&] { return log.count() == 1; }));

    auto [token, pkt] = log.at(0);
    CHECK(token >= 1);  // 0 is the in-process caller
    CHECK(parseAddress(pkt.data(), static_cast<uint32_t>(pkt.size())) == "/status");

    // Reply routing: send to the origin token reaches this client, framed.
    auto reply = message("/status.reply", 1);
    REQUIRE(t.send(token, reply.ptr(), reply.size(), false));
    auto body = readFramed(fd);
    REQUIRE(!body.empty());
    CHECK(parseAddress(body.data(), static_cast<uint32_t>(body.size())) == "/status.reply");

    // Subscription lifetime == connection lifetime.
    CHECK(t.subscribeNotify(token));
    CHECK(t.hasNotifySubscribers());
    close(fd);
    REQUIRE(waitUntil([&] { return !t.hasNotifySubscribers(); }));
    CHECK_FALSE(t.send(token, reply.ptr(), reply.size(), false));  // token is dead

    t.stop();
}

TEST_CASE("StreamOscTransport/UDS: round trip over an owner-only socket file",
          "[transport][uds]") {
    IngestLog log;
    StreamOscTransport t;
    t.setIngest([&](const uint8_t* d, uint32_t n, uint32_t tok) { log.record(d, n, tok); });
    std::string path = tmpSock("st.sock");
    t.initialiseUds(path);
    REQUIRE(t.start());

    struct stat st{};
    REQUIRE(stat(path.c_str(), &st) == 0);
    CHECK((st.st_mode & 0777) == 0600);

    int fd = connectUnixStream(path);
    auto probe = message("/g_queryTree", 0, 0);
    writeFramed(fd, probe.data);
    REQUIRE(waitUntil([&] { return log.count() == 1; }));
    auto [token, pkt] = log.at(0);
    CHECK(parseAddress(pkt.data(), static_cast<uint32_t>(pkt.size())) == "/g_queryTree");

    auto reply = message("/g_queryTree.reply");
    REQUIRE(t.send(token, reply.ptr(), reply.size(), false));
    auto body = readFramed(fd);
    REQUIRE(!body.empty());
    CHECK(parseAddress(body.data(), static_cast<uint32_t>(body.size())) == "/g_queryTree.reply");

    close(fd);
    t.stop();
    CHECK(access(path.c_str(), F_OK) != 0);  // stop unlinks the socket path
}

TEST_CASE("UdsDgramOscTransport: bound peers get replies, unbound peers are unaddressable",
          "[transport][uds]") {
    IngestLog log;
    UdsDgramOscTransport t;
    t.setIngest([&](const uint8_t* d, uint32_t n, uint32_t tok) { log.record(d, n, tok); });
    std::string server = tmpSock("dg.sock");
    t.initialise(server);
    REQUIRE(t.start());

    // Bound client: command in, reply routed back by token.
    std::string clientPath = tmpSock("dg-client.sock");
    int fd = bindUnixDgram(clientPath);
    auto probe = message("/status");
    sendUnixDgram(fd, server, probe.data);
    REQUIRE(waitUntil([&] { return log.count() == 1; }));
    auto [token, pkt] = log.at(0);
    CHECK(parseAddress(pkt.data(), static_cast<uint32_t>(pkt.size())) == "/status");

    auto reply = message("/status.reply", 1);
    REQUIRE(t.send(token, reply.ptr(), reply.size(), false));
    uint8_t buf[512];
    ssize_t n = recv(fd, buf, sizeof(buf), 0);
    REQUIRE(n > 0);
    CHECK(parseAddress(buf, static_cast<uint32_t>(n)) == "/status.reply");

    // Same client keeps the same token (stable across packets)…
    sendUnixDgram(fd, server, probe.data);
    REQUIRE(waitUntil([&] { return log.count() == 2; }));
    CHECK(log.at(1).first == token);

    // …and can join a notify audience.
    CHECK(t.subscribeNotify(token));
    auto push = message("/supersonic/devices/changed");
    t.broadcastNotify(push.ptr(), push.size());
    n = recv(fd, buf, sizeof(buf), 0);
    REQUIRE(n > 0);
    CHECK(parseAddress(buf, static_cast<uint32_t>(n)) == "/supersonic/devices/changed");

    // Unbound client: ingested, but replies and subscriptions are rejected.
    int anon = socket(AF_UNIX, SOCK_DGRAM, 0);
    REQUIRE(anon >= 0);
    sendUnixDgram(anon, server, probe.data);
    REQUIRE(waitUntil([&] { return log.count() == 3; }));
    uint32_t anonToken = log.at(2).first;
    CHECK_FALSE(t.send(anonToken, reply.ptr(), reply.size(), false));
    CHECK_FALSE(t.subscribeNotify(anonToken));

    close(fd);
    close(anon);
    unlink(clientPath.c_str());
    t.stop();
    CHECK(access(server.c_str(), F_OK) != 0);  // stop unlinks the socket path
}

#endif // !_WIN32

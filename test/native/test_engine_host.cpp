// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Sam Aaron
/*
 * test_engine_host.cpp — the pieces SuperSonic's main is composed from
 * (host/EngineHost.h): the scsynth-shaped command line, and the command
 * transports with their boot queue. No engine: the transports talk to a
 * recorded ingest, exactly as the main wires them to the front and the
 * engine. The process itself is pinned in test_supersonic_binary.cpp.
 */
#include <catch2/catch_test_macros.hpp>

#include "EngineHost.h"
#include "EngineFixture.h"
#include "OscTestUtils.h"

#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <unistd.h>
#endif

namespace {

using supersonic_host::Options;
using supersonic_host::parseArgs;
using supersonic_host::CommandTransports;

Options parse(std::vector<std::string> args, std::string* err = nullptr, bool* ok = nullptr) {
    std::vector<char*> argv;
    static std::string prog = "host";
    argv.push_back(prog.data());
    for (auto& a : args) argv.push_back(a.data());
    Options o;
    std::string e;
    const bool r = parseArgs(static_cast<int>(argv.size()), argv.data(), o, &e);
    if (err) *err = e;
    if (ok) *ok = r;
    return o;
}

} // namespace

TEST_CASE("host args: the scsynth-shaped flags land in the engine's config", "[host][args]") {
    bool ok = false;
    std::string err;
    const Options o = parse({ "-u", "4000", "-a", "512", "-m", "65536", "-Z", "128", "-z", "64",
                              "-B", "127.0.0.1", "-b", "2048", "-n", "4096", "-i", "0", "-o", "2",
                              "-S", "44100", "-w", "128", "-c", "8192",
                              "--default-bpm", "60", "--app-name", "Probe", "--audio-driver", "CoreAudio",
                              "--max-connections", "16", "--inbox-mb", "64", "--headless",
                              "-D", "0", "-R", "0", "-l", "1" }, &err, &ok);
    INFO(err);
    REQUIRE(ok);
    CHECK(o.cfg.udpPort == 4000);
    CHECK(o.cfg.numAudioBusChannels == 512);
    CHECK(o.cfg.realTimeMemorySize == 65536);
    CHECK(o.cfg.bufferSize == 128);
    CHECK(o.cfg.blockSize == 64);
    CHECK(o.cfg.bindAddress == "127.0.0.1");
    CHECK(o.cfg.numBuffers == 2048);
    CHECK(o.cfg.maxNodes == 4096);
    CHECK(o.cfg.numInputChannels == 0);
    CHECK(o.cfg.numOutputChannels == 2);
    CHECK(o.cfg.sampleRate == 44100);
    CHECK(o.cfg.maxWireBufs == 128);
    CHECK(o.cfg.numControlBusChannels == 8192);
    CHECK(o.cfg.defaultBpm == 60.0);
    CHECK(o.cfg.appName == "Probe");
    CHECK(o.cfg.audioDriver == "CoreAudio");
    CHECK(o.maxConnections == 16);
    CHECK(o.cfg.inboxBytes == 64u * 1024u * 1024u);
    CHECK(o.cfg.headless);
    CHECK(o.headless);
    // scsynth's own flags that mean nothing here are accepted, not warned about.
    CHECK(o.warnings.empty());
    // -u > 0 derives the segment's endpoint.
    CHECK(o.shmEndpoint.find("4000") != std::string::npos);
    CHECK(o.desiredInputChannels == 0);
}

TEST_CASE("host args: defaults, and what the host derives", "[host][args]") {
    const Options o = parse({});
    CHECK(o.cfg.udpPort == 57110);
    CHECK(o.cfg.sampleRate == 48000);
    CHECK(o.cfg.bufferSize == 0);
    CHECK(o.cfg.callbackWatchdog);
    CHECK(o.cfg.inboxBytes == 512u * 1024u * 1024u);   // address space, not memory
    CHECK(o.maxConnections == 4);
    CHECK_FALSE(o.headless);
    CHECK(o.desiredInputChannels == ClockworkEngine::kAutoChannelCount);
    CHECK_FALSE(o.shmEndpoint.empty());

    // -u 0: no segment, no endpoint.
    const Options none = parse({ "-u", "0" });
    CHECK(none.cfg.udpPort == 0);
    CHECK(none.shmEndpoint.empty());

    // Clamps: a thread bomb and a lane past the header's reach are both refused quietly.
    const Options big = parse({ "--max-connections", "99999", "--inbox-mb", "99999" });
    CHECK(big.maxConnections == 1024);
    CHECK(big.cfg.inboxBytes == 3072u * 1024u * 1024u);
}

TEST_CASE("host args: an unknown flag is reported, not silently applied", "[host][args]") {
    const Options o = parse({ "--no-such-flag", "7", "-q", "3", "-u", "4001" });
    REQUIRE(o.warnings.size() == 2);
    CHECK(o.warnings[0].find("--no-such-flag") != std::string::npos);
    CHECK(o.warnings[1].find("-q") != std::string::npos);
    CHECK(o.cfg.udpPort == 4001);   // parsing carried on past them
}

TEST_CASE("host args: -H names the devices, scsynth's way", "[host][args]") {
    const Options one = parse({ "-H", "MacBook Pro Speakers" });
    CHECK(one.cfg.hardwareDevice == "MacBook Pro Speakers");
    CHECK(one.cfg.inputDevice == "MacBook Pro Speakers");
    const Options two = parse({ "-H", "Mic In", "Speakers Out", "-u", "4002" });
    CHECK(two.cfg.inputDevice == "Mic In");
    CHECK(two.cfg.hardwareDevice == "Speakers Out");
    CHECK(two.cfg.udpPort == 4002);
}

TEST_CASE("host args: at most one command transport, and the segment ones need a segment", "[host][args]") {
    bool ok = true;
    std::string err;
    parse({ "--tcp", "4003", "--uds", "/tmp/x.sock" }, &err, &ok);
    CHECK_FALSE(ok);
    CHECK(err.find("at most one") != std::string::npos);
    parse({ "-u", "0", "--shm-commands" }, &err, &ok);
    CHECK_FALSE(ok);
    CHECK(err.find("-u > 0") != std::string::npos);
    parse({ "-u", "0", "--shm-endpoint", "/tmp/y.sock" }, &err, &ok);
    CHECK_FALSE(ok);
    const Options fine = parse({ "-u", "4004", "--tcp", "4005", "--shm-endpoint", "/tmp/z.sock" }, &err, &ok);
    CHECK(ok);
    CHECK(fine.tcpPort == 4005);
    CHECK(fine.shmEndpoint == "/tmp/z.sock");
}

TEST_CASE("host usage: every flag the parser knows is in the help text", "[host][args]") {
    const std::string u = supersonic_host::usage("Probe");
    CHECK(u.find("Probe") != std::string::npos);
    for (const char* flag : { "-u", "-S", "-Z", "-z", "-i", "-o", "-n", "-b", "-a", "-c", "-m", "-w", "-B", "-H",
                              "--default-bpm", "--app-name", "--audio-driver", "--tcp", "--uds", "--uds-dgram",
                              "--pipe", "--shm-commands", "--max-connections", "--inbox-mb", "--shm-endpoint",
                              "--headless", "--list-devices" })
        CHECK(u.find(flag) != std::string::npos);
}

#ifndef _WIN32
namespace {

struct IngestLog {
    std::mutex mu;
    std::vector<std::pair<uint32_t, std::string>> items;   // token, address
    void record(const uint8_t* d, uint32_t n, uint32_t token) {
        std::lock_guard<std::mutex> lk(mu);
        items.emplace_back(token, osc_test::parseAddress(d, n));
    }
    size_t count() { std::lock_guard<std::mutex> lk(mu); return items.size(); }
    std::pair<uint32_t, std::string> at(size_t i) { std::lock_guard<std::mutex> lk(mu); return items.at(i); }
};

bool waitUntil(const std::function<bool()>& f, int ms = 3000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (f()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return f();
}

int freePort(int type) {
    int fd = socket(AF_INET, type, 0);
    REQUIRE(fd >= 0);
    sockaddr_in sa {};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    REQUIRE(bind(fd, reinterpret_cast<sockaddr*>(&sa), sizeof sa) == 0);
    socklen_t len = sizeof sa;
    REQUIRE(getsockname(fd, reinterpret_cast<sockaddr*>(&sa), &len) == 0);
    close(fd);
    return ntohs(sa.sin_port);
}

void sendUdp(int port, const osc_test::Packet& p) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    REQUIRE(fd >= 0);
    sockaddr_in sa {};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(static_cast<uint16_t>(port));
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    REQUIRE(sendto(fd, p.ptr(), p.size(), 0, reinterpret_cast<sockaddr*>(&sa), sizeof sa) == static_cast<ssize_t>(p.size()));
    close(fd);
}

int connectTcp(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(fd >= 0);
    sockaddr_in sa {};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(static_cast<uint16_t>(port));
    inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
    REQUIRE(connect(fd, reinterpret_cast<sockaddr*>(&sa), sizeof sa) == 0);
    timeval tv { 2, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    return fd;
}
void writeFramed(int fd, const osc_test::Packet& p) {
    const uint32_t be = htonl(static_cast<uint32_t>(p.size()));
    REQUIRE(write(fd, &be, 4) == 4);
    REQUIRE(write(fd, p.ptr(), p.size()) == static_cast<ssize_t>(p.size()));
}
std::vector<uint8_t> readFramed(int fd) {
    auto readAll = [&](uint8_t* dst, size_t n) {
        size_t got = 0;
        while (got < n) {
            const ssize_t r = read(fd, dst + got, n - got);
            if (r <= 0) return false;
            got += static_cast<size_t>(r);
        }
        return true;
    };
    uint8_t hdr[4];
    if (!readAll(hdr, 4)) return {};
    const uint32_t len = (uint32_t(hdr[0]) << 24) | (uint32_t(hdr[1]) << 16) | (uint32_t(hdr[2]) << 8) | uint32_t(hdr[3]);
    std::vector<uint8_t> body(len);
    if (!readAll(body.data(), len)) return {};
    return body;
}

} // namespace

TEST_CASE("host transports: UDP receives from the start and holds what arrives until the engine is ready",
          "[host][transports]") {
    const int port = freePort(SOCK_DGRAM);
    Options o = parse({ "-u", std::to_string(port), "-B", "127.0.0.1" });
    IngestLog log;
    CommandTransports t;
    std::string err;
    IOscTransport* transport = t.select(o, nullptr, [&](const uint8_t* d, uint32_t n, uint32_t tok) { log.record(d, n, tok); }, &err);
    INFO(err);
    REQUIRE(transport != nullptr);
    CHECK(t.description().find(std::to_string(port)) != std::string::npos);

    // Before the engine is up: received and held, in order.
    sendUdp(port, osc_test::message("/first"));
    sendUdp(port, osc_test::message("/second"));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK(log.count() == 0);

    // The engine is up: the queue drains in order, and new packets go straight through.
    REQUIRE(t.start());
    REQUIRE(waitUntil([&] { return log.count() == 2; }));
    CHECK(log.at(0).second == "/first");
    CHECK(log.at(1).second == "/second");
    sendUdp(port, osc_test::message("/third"));
    REQUIRE(waitUntil([&] { return log.count() == 3; }));
    CHECK(log.at(2).second == "/third");

    // Stopped: nothing more arrives.
    t.stop();
    sendUdp(port, osc_test::message("/fourth"));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK(log.count() == 3);
    t.stop();   // idempotent
}

TEST_CASE("host transports: TCP is the command port when asked, and a reply goes back to its sender",
          "[host][transports]") {
    const int shmPort = freePort(SOCK_DGRAM), tcpPort = freePort(SOCK_STREAM);
    Options o = parse({ "-u", std::to_string(shmPort), "--tcp", std::to_string(tcpPort), "-B", "127.0.0.1",
                        "--max-connections", "2" });
    IngestLog log;
    CommandTransports t;
    std::string err;
    IOscTransport* transport = t.select(o, nullptr, [&](const uint8_t* d, uint32_t n, uint32_t tok) { log.record(d, n, tok); }, &err);
    REQUIRE(transport != nullptr);
    CHECK(t.description().find("TCP") != std::string::npos);
    REQUIRE(t.start());

    const int fd = connectTcp(tcpPort);
    writeFramed(fd, osc_test::message("/status"));
    REQUIRE(waitUntil([&] { return log.count() == 1; }));
    CHECK(log.at(0).second == "/status");
    const uint32_t token = log.at(0).first;
    CHECK(token != 0);

    const auto reply = osc_test::message("/status.reply", int32_t{1});
    REQUIRE(transport->send(token, reply.ptr(), reply.size(), false));
    const auto got = readFramed(fd);
    REQUIRE_FALSE(got.empty());
    CHECK(osc_test::parseAddress(got.data(), static_cast<uint32_t>(got.size())) == "/status.reply");
    close(fd);
    t.stop();
}
#endif

#ifndef _WIN32
namespace {
// A UDP client that keeps its socket, to hear the reply.
struct UdpClient {
    int fd = -1;
    UdpClient() {
        fd = socket(AF_INET, SOCK_DGRAM, 0);
        REQUIRE(fd >= 0);
        sockaddr_in me {};
        me.sin_family = AF_INET;
        me.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        REQUIRE(bind(fd, reinterpret_cast<sockaddr*>(&me), sizeof me) == 0);
        timeval tv { 3, 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    }
    ~UdpClient() { if (fd >= 0) close(fd); }
    void send(int port, const osc_test::Packet& p) {
        sockaddr_in sa {};
        sa.sin_family = AF_INET;
        sa.sin_port = htons(static_cast<uint16_t>(port));
        sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        REQUIRE(sendto(fd, p.ptr(), p.size(), 0, reinterpret_cast<sockaddr*>(&sa), sizeof sa) == static_cast<ssize_t>(p.size()));
    }
    std::string recvAddress() {
        uint8_t buf[65536];
        const ssize_t n = recv(fd, buf, sizeof buf, 0);
        if (n <= 0) return "";
        return osc_test::parseAddress(buf, static_cast<uint32_t>(n));
    }
};
} // namespace

TEST_CASE("host egress pump: the engine's replies reach the socket through the client API, not the engine's gateway",
          "[host][egress]") {
    // The whole path a running host has, in one process: a UDP command port,
    // an engine that leaves its egress rings to the host, and the pump that
    // takes from them and routes to the transport.
    const int port = freePort(SOCK_DGRAM);
    Options o = parse({ "-u", std::to_string(port), "-B", "127.0.0.1", "--headless" });
    o.cfg.hostDrainsEgress = true;
    o.cfg.manualAudioPump = false;
    o.cfg.udpPort = 0;   // the fixture's engine: no segment needed here
    o.cfg.headless = true;

    EngineFixture fx(o.cfg);
    CommandTransports t;
    std::string err;
    Options wire = o;
    wire.cfg.udpPort = port;
    IOscTransport* transport = t.select(wire, nullptr,
        [&](const uint8_t* d, uint32_t n, uint32_t tok) { fx.engine().ingest(d, n, tok); }, &err);
    REQUIRE(transport != nullptr);
    fx.engine().setTransport(transport);   // the token registry is the transport's
    REQUIRE(t.start());

    std::vector<std::string> debug;
    supersonic_host::EgressPump pump(fx.engine(), *transport, [&](const std::string& s) { debug.push_back(s); });
    pump.start();

    UdpClient c;
    c.send(port, osc_test::message("/status"));
    CHECK(c.recvAddress() == "/status.reply");
    c.send(port, osc_test::message("/b_query", int32_t{0}));
    CHECK(c.recvAddress() == "/b_info");

    // Stopped, the pump takes nothing more; the engine's own transport
    // never saw these (the fixture listens there).
    pump.stop();
    OscReply r;
    CHECK_FALSE(fx.waitForReply("/b_info", r, 200));
    t.stop();
}
#endif

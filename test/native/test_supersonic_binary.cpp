// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Sam Aaron
/*
 * test_supersonic_binary.cpp — the SuperSonic process, as Sonic Pi's daemon
 * starts it: SuperSonic's OWN main (host/SuperSonicMain.cpp), which opens the
 * command socket with clockwork's comms library, embeds the engine, stands
 * its front between the two, and serves the shared-memory segment.
 *
 * Spawned, not linked: what is pinned is the binary a launcher runs — its
 * flags, its socket, its replies, its front, its segment, and that SIGTERM
 * brings it down cleanly. POSIX only (fork/exec); the pieces it is built
 * from are covered on every platform in clockwork's own suite.
 */
#ifndef _WIN32
#include <catch2/catch_test_macros.hpp>

#include "OscTestUtils.h"
#include "clockwork_client.h"
#include "clockwork_audio_file.h"

#include <algorithm>
#include <cmath>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#ifndef SUPERSONIC_BINARY
#define SUPERSONIC_BINARY ""
#endif
#ifndef CLOCKWORK_SAMPLES_DIR
#define CLOCKWORK_SAMPLES_DIR ""
#endif
#ifndef CLOCKWORK_SYNTHDEFS_DIR
#define CLOCKWORK_SYNTHDEFS_DIR ""
#endif

namespace {

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

std::string logPath() {
    return (std::filesystem::temp_directory_path() / ("supersonic-binary-" + std::to_string(::getpid()) + ".log")).string();
}

// The process, with stderr to a file this test can read.
struct Process {
    pid_t pid = -1;
    std::string log;
    ~Process() { kill(SIGKILL); }
    void start(const std::vector<std::string>& args) {
        log = logPath();
        std::vector<std::string> all = { SUPERSONIC_BINARY };
        all.insert(all.end(), args.begin(), args.end());
        std::vector<char*> argv;
        for (auto& a : all) argv.push_back(a.data());
        argv.push_back(nullptr);
        pid = fork();
        REQUIRE(pid >= 0);
        if (pid == 0) {
            const int fd = open(log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
            if (fd >= 0) { dup2(fd, 2); dup2(fd, 1); close(fd); }
            execv(argv[0], argv.data());
            _exit(127);
        }
    }
    // Sends the signal; returns the exit status, or -1 if it did not exit in time.
    int kill(int sig, int timeoutMs = 10000) {
        if (pid <= 0) return -1;
        ::kill(pid, sig);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        int status = 0;
        while (std::chrono::steady_clock::now() < deadline) {
            const pid_t r = waitpid(pid, &status, WNOHANG);
            if (r == pid) { pid = -1; return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status); }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return -1;
    }
    std::string logText() const {
        std::ifstream f(log);
        return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    }
};

// A TCP client, length-prefixed OSC, as the spider talks.
struct Client {
    int fd = -1;
    ~Client() { if (fd >= 0) close(fd); }
    bool connectWithin(int port, int ms) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (std::chrono::steady_clock::now() < deadline) {
            fd = socket(AF_INET, SOCK_STREAM, 0);
            sockaddr_in sa {};
            sa.sin_family = AF_INET;
            sa.sin_port = htons(static_cast<uint16_t>(port));
            inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
            if (connect(fd, reinterpret_cast<sockaddr*>(&sa), sizeof sa) == 0) {
                timeval tv { 5, 0 };
                setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
                return true;
            }
            close(fd); fd = -1;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        return false;
    }
    void send(const osc_test::Packet& p) {
        const uint32_t be = htonl(static_cast<uint32_t>(p.size()));
        REQUIRE(write(fd, &be, 4) == 4);
        REQUIRE(write(fd, p.ptr(), p.size()) == static_cast<ssize_t>(p.size()));
    }
    std::vector<uint8_t> read() {
        auto readAll = [&](uint8_t* dst, size_t n) {
            size_t got = 0;
            while (got < n) {
                const ssize_t r = ::read(fd, dst + got, n - got);
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
    // The next reply at `addr`, skipping others (the engine also broadcasts).
    osc_test::ParsedReply expect(const char* addr, int tries = 50) {
        for (int i = 0; i < tries; ++i) {
            const auto b = read();
            REQUIRE_FALSE(b.empty());
            if (osc_test::parseAddress(b.data(), static_cast<uint32_t>(b.size())) == addr)
                return osc_test::parseReply(b.data(), static_cast<uint32_t>(b.size()));
        }
        FAIL("no reply at " << addr);
        return osc_test::parseReply(nullptr, 0);
    }
};

} // namespace

TEST_CASE("the SuperSonic binary: its own main opens the socket, fronts the engine, serves the segment, and stops on SIGTERM",
          "[binary]") {
    REQUIRE(std::filesystem::exists(SUPERSONIC_BINARY));
    const int shmPort = freePort(SOCK_DGRAM), tcpPort = freePort(SOCK_STREAM);
    Process p;
    p.start({ "--headless", "-u", std::to_string(shmPort), "--tcp", std::to_string(tcpPort),
              "--max-connections", "4", "-B", "127.0.0.1", "--inbox-mb", "32" });

    Client c;
    REQUIRE(c.connectWithin(tcpPort, 20000));

    // Alive and answering, as scsynth would.
    c.send(osc_test::message("/status"));
    const auto status = c.expect("/status.reply");
    CHECK(status.argCount() >= 2);

    // The front is in the process: a file verb is answered in scsynth's words
    // (the engine alone would refuse it).
    const std::string sample = std::string(CLOCKWORK_SAMPLES_DIR) + "/bd_haus.flac";
    if (std::filesystem::exists(sample)) {
        osc_test::Builder b;
        b.begin("/b_allocRead") << int32_t{0} << sample.c_str() << int32_t{0} << int32_t{0};
        c.send(b.end());
        const auto done = c.expect("/done");
        CHECK(done.argString(0) == "/b_allocRead");
        CHECK(done.argInt(1) == 0);
        c.send(osc_test::message("/b_query", int32_t{0}));
        CHECK(c.expect("/b_info").argInt(1) > 0);
    }

    // A session recording through the process: the front holds the guest's
    // tap, the guest feeds it, the file is written here — with a synth
    // playing so the file is not silence.
    {
        const std::string rec = (std::filesystem::temp_directory_path() / ("supersonic-binary-rec-" + std::to_string(::getpid()) + ".wav")).string();
        osc_test::Builder b;
        b.begin("/clockwork/record/start") << rec.c_str() << "wav" << int32_t{16};
        c.send(b.end());
        const auto started = c.expect("/clockwork/record/start.reply");
        CHECK(started.argInt(0) == 1);
        // A synthdef the process can load from the packaged set.
        const std::string def = std::string(CLOCKWORK_SYNTHDEFS_DIR) + "/sonic-pi-beep.scsyndef";
        REQUIRE(std::filesystem::exists(def));
        c.send(osc_test::message("/d_load", def.c_str()));
        CHECK(c.expect("/done").argString(0) == "/d_load");
        osc_test::Builder sn;
        sn.begin("/s_new") << "sonic-pi-beep" << int32_t{5300} << int32_t{0} << int32_t{0} << "note" << 60.0f << "amp" << 0.8f << "sustain" << 2.0f;
        c.send(sn.end());
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        c.send(osc_test::message("/clockwork/record/stop"));
        const auto stopped = c.expect("/clockwork/record/stop.reply");
        CHECK(stopped.argInt(0) == 1);
        c.send(osc_test::message("/n_free", int32_t{5300}));
        ClockworkAudioInfo info {};
        info.struct_bytes = sizeof info;
        float* frames = nullptr;
        REQUIRE(clockwork_audio_decode_file(rec.c_str(), &info, &frames) == CLOCKWORK_OK);
        CHECK(info.frames > 48000 * 0.2);
        float peak = 0.f;
        for (uint64_t i = 0; i < info.frames * info.channels; ++i) peak = std::max(peak, std::fabs(frames[i]));
        CHECK(peak > 0.05f);
        clockwork_audio_free(frames);
        std::filesystem::remove(rec);
    }

    // The segment is served at the endpoint -u derives, and the lane is the
    // size the flag asked for.
    char endpoint[512];
    REQUIRE(clockwork_client_default_endpoint(static_cast<unsigned>(shmPort), endpoint, sizeof endpoint) > 0);
    ClockworkStatus st = CLOCKWORK_OK;
    ClockworkClient* shm = nullptr;
    for (int i = 0; i < 50 && !shm; ++i) {
        shm = clockwork_client_open_shm(endpoint, &st);
        if (!shm) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    REQUIRE(shm != nullptr);
    ClockworkRegion inbox {};
    REQUIRE(clockwork_client_region(shm, CLOCKWORK_REGION_INBOX, &inbox) == CLOCKWORK_OK);
    CHECK(inbox.bytes == 32u * 1024u * 1024u);
    clockwork_client_close(shm);

    // Down cleanly on SIGTERM, and the log says whose main this was.
    CHECK(p.kill(SIGTERM) == 0);
    const std::string log = p.logText();
    CHECK(log.find("front:") != std::string::npos);
    CHECK(log.find("egress: both rings drained here") != std::string::npos);
    CHECK(log.find("shutting down") != std::string::npos);
    std::filesystem::remove(p.log);
}

TEST_CASE("the SuperSonic binary: -v names the product and exits at once", "[binary]") {
    REQUIRE(std::filesystem::exists(SUPERSONIC_BINARY));
    Process p;
    p.start({ "-v" });
    CHECK(p.kill(0, 5000) == 0);   // signal 0: only waits
    const std::string out = p.logText();
    CHECK(out.find("SuperSonic") != std::string::npos);
    std::filesystem::remove(p.log);
}
#endif

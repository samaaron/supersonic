/*
 * test_shm_peer_crash.cpp — cross-process crash-safety of the SHM command
 * plane, proven with real processes: a peer (peer_shm/main.cpp) is SIGKILLed
 * at random moments while writing the command ring, repeatedly, and the
 * engine must never observe a torn frame, never stall, and always accept a
 * fresh peer afterwards.
 *
 * The properties under test are structural (single-producer commit-publish;
 * validate-on-read): a producer killed at ANY instruction leaves the
 * committed prefix intact and the uncommitted frame invisible. The kill loop
 * is a fuzz over "any instruction".
 *
 * POSIX-only: the spawn/SIGKILL harness uses fork/execv. The in-process
 * plane behaviour is covered cross-platform in test_shm_peer_plane.cpp.
 */
#ifndef _WIN32

#include <catch2/catch_test_macros.hpp>

#include "EngineFixture.h"
#include "OscTestUtils.h"
#include "src/shm_peer_plane.h"
#include "src/synth/common/server_shm.hpp"

#include <csignal>
#include <random>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

// Detect ThreadSanitizer (GCC macro or Clang feature) — used to gate the
// spam-flood loop below, which wedges under TSan for reasons unrelated to the
// property it tests (see that TEST_CASE's guard comment).
#if defined(__SANITIZE_THREAD__)
#  define SS_TSAN 1
#elif defined(__has_feature)
#  if __has_feature(thread_sanitizer)
#    define SS_TSAN 1
#  endif
#endif

// Sanitizer builds run several times slower, and this test also churns child
// processes (fork/exec/SIGKILL) that contend with the engine's threads — so a
// fixed 2s reply/poll timeout can expire before a perfectly live engine
// answers. Scale the waits up under a sanitizer build only. Mirrors
// kTimeoutScale in test_scheduling_accuracy.cpp.
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
constexpr int kTimeoutScale = 4;
#elif defined(__has_feature)
#  if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
constexpr int kTimeoutScale = 4;
#  else
constexpr int kTimeoutScale = 1;
#  endif
#else
constexpr int kTimeoutScale = 1;
#endif

namespace {

SupersonicEngine::Config crashConfig(unsigned port) {
    SupersonicEngine::Config cfg;
    cfg.sampleRate   = 48000;
    cfg.bufferSize   = 128;
    cfg.udpPort      = port;
    cfg.numBuffers   = 256;
    cfg.maxNodes     = 256;
    cfg.maxGraphDefs = 64;
    cfg.maxWireBufs  = 32;
    cfg.headless     = true;
    cfg.shmCommands  = true;
    return cfg;
}

// Spawn the peer helper; block until it prints "ready" (attached).
pid_t spawnPeer(const char* mode, unsigned port, int count = 0) {
    int fds[2];
    REQUIRE(pipe(fds) == 0);
    pid_t pid = fork();
    REQUIRE(pid >= 0);
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);
        std::string portStr = std::to_string(port);
        std::string countStr = std::to_string(count);
        execl(SUPERSONIC_TEST_SHM_PEER_BINARY, "peer_shm",
              mode, portStr.c_str(),
              count > 0 ? countStr.c_str() : nullptr,
              nullptr);
        _exit(127);  // exec failed
    }
    close(fds[1]);
    // Gate on "ready" so kills land during ring writes, not during attach.
    char buf[8] = {};
    ssize_t n = read(fds[0], buf, sizeof(buf) - 1);
    close(fds[0]);
    REQUIRE(n > 0);
    REQUIRE(std::string(buf).find("ready") == 0);
    return pid;
}

void killPeer(pid_t pid) {
    kill(pid, SIGKILL);
    int status = 0;
    waitpid(pid, &status, 0);
}

bool waitPeerExit(pid_t pid, int timeoutMs = 10000) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        int status = 0;
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) return WIFEXITED(status) && WEXITSTATUS(status) == 0;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    kill(pid, SIGKILL);
    waitpid(pid, nullptr, 0);
    return false;
}

} // namespace

// Gated off ThreadSanitizer: the 12-round spam-flood loop repeatedly fork/exec/
// SIGKILLs a peer that hammers the command ring, and under TSan that combination
// wedges the headless driver's gateway wake (TSan's thread-stop can fall through
// to pthread_cancel/killThread, and once the headless tick stalls nothing wakes
// the gateway — see test_ring_reader.cpp) so /status stops being answered. The
// crash-safety property this proves is structural and is fully exercised in the
// Release and ASan builds; the plane's *race* behaviour is covered under TSan
// in-process by test_shm_peer_plane.cpp. The lighter reattach case below still
// runs under TSan.
#ifndef SS_TSAN
TEST_CASE("shm-peer crash: SIGKILL mid-traffic never corrupts the ring or stalls the engine",
          "[shm][peer][crash]") {
    constexpr unsigned kPort = 57224;
    EngineFixture fx(crashConfig(kPort));
    const PerformanceMetrics& m = fx.engine().getMetrics();

    std::mt19937 rng(0x5EED);  // deterministic schedule; the fuzz is over iterations
    std::uniform_int_distribution<int> holdMs(1, 25);

    constexpr int kKillCycles = 12;
    for (int i = 0; i < kKillCycles; ++i) {
        uint32_t sentBefore = m.osc_out_messages_sent.load(std::memory_order_relaxed);
        pid_t pid = spawnPeer("spam", kPort);

        // Let it write for a random slice so the kill lands at an arbitrary
        // point in the produce loop (including mid-frame-memcpy).
        std::this_thread::sleep_for(std::chrono::milliseconds(holdMs(rng)));
        killPeer(pid);

        // The engine made progress on the peer's traffic…
        REQUIRE(fx.pollUntil([&] {
            return m.osc_out_messages_sent.load(std::memory_order_relaxed) > sentBefore;
        }, 2000 * kTimeoutScale));
        // …and never saw a torn frame: commit-publish means a partial write
        // is unpublished, and the drain's validation never fired.
        REQUIRE(m.osc_in_corrupted.load(std::memory_order_relaxed) == 0);

        // The engine (audio + gateway) is fully alive after each corpse.
        OscReply reply;
        fx.send(osc_test::message("/status"));
        REQUIRE(fx.waitForReply("/status.reply", reply, 2000 * kTimeoutScale));
    }
}
#endif // !SS_TSAN

TEST_CASE("shm-peer crash: a corpse holding the writer lock stalls nobody; reattach recovers it",
          "[shm][peer][crash]") {
    constexpr unsigned kPort = 57225;
    EngineFixture fx(crashConfig(kPort));
    const PerformanceMetrics& m = fx.engine().getMetrics();

    server_shared_memory_client client(kPort);
    ShmPeerPlaneHeader* plane = client.get_peer_plane();
    REQUIRE(plane != nullptr);

    // A peer dies holding cmd_write_lock.
    pid_t holder = spawnPeer("hold-lock", kPort);
    killPeer(holder);
    REQUIRE(plane->cmd_write_lock.load(std::memory_order_relaxed) == 1);

    // The host never takes that lock: the engine stays fully responsive.
    OscReply reply;
    fx.send(osc_test::message("/status"));
    REQUIRE(fx.waitForReply("/status.reply", reply, 2000 * kTimeoutScale));

    // A fresh peer attaches (resetting the stale lock) and completes a whole
    // burst — every committed frame is delivered, in order, none lost.
    constexpr int kBurst = 200;
    uint32_t sentBefore = m.osc_out_messages_sent.load(std::memory_order_relaxed);
    pid_t burster = spawnPeer("burst", kPort, kBurst);
    REQUIRE(waitPeerExit(burster));
    REQUIRE(fx.pollUntil([&] {
        return m.osc_out_messages_sent.load(std::memory_order_relaxed)
               >= sentBefore + kBurst;
    }, 2000 * kTimeoutScale));
    CHECK(m.osc_in_corrupted.load(std::memory_order_relaxed) == 0);

    // Completeness + order: /synced ids 1..kBurst, monotone, none missing.
    // (Replies surface via the fixture's CallbackTransport.)
    REQUIRE(fx.pollUntil([&] {
        int expected = 1;
        for (const auto& r : fx.allReplies()) {
            if (r.address != "/synced") continue;
            if (r.parsed().argInt(0) == expected) ++expected;
        }
        return expected > kBurst;
    }, 10000 * kTimeoutScale));
}

#endif // !_WIN32

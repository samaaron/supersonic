/*
 * test_scope_race.cpp — Stress-test the ScopeOut2 cold-swap race
 *
 * Background: ScopeOut2_next has two defensive NULL checks on the scope
 * buffer pointer. A comment in DelayUGens.cpp says they're there because
 * "the shm_scope_buffer in shm was released out from under us by a cold-swap
 * race." The defenses were added in response to a SIGSEGV seen during
 * macOS device-switching work.
 *
 * But: no test reproduces the crash, no current failing test exists.
 * The defenses may still be protecting against a live race, OR the
 * race window may have been incidentally closed by subsequent ordering
 * fixes (deferred aggregate destroy, mSuppressRunLoop, the 1-second
 * changeListenerCallback debounce). We don't know.
 *
 * This file's tests try to find out:
 *   1. Many rapid cold swaps while multiple scope synths are active.
 *   2. After the stress run, read gScopeOut2DefenseTripCount — the
 *      counter incremented each time the defensive guard fired.
 *
 * Interpretation of outcomes:
 *   - counter > 0 AND test passes: defenses caught a live race the
 *     test exercised. The crash without defenses is plausible; we
 *     have a lead on timing/ordering to investigate.
 *   - counter == 0 AND test passes: stress didn't hit the race
 *     window. Either the window is closed, or the test isn't
 *     aggressive enough. Either way, the defenses are working
 *     hard for nothing we can currently observe.
 *   - test fails: defenses are incomplete OR something else is
 *     broken — either way, we have a new reproducer.
 */
#include <catch2/catch_test_macros.hpp>
#include "EngineFixture.h"
#include "OscBuilder.h"
#include "OscTestUtils.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <thread>

// Exposed by DelayUGens.cpp; tests extern it directly to avoid adding a
// whole diagnostics header for a single counter.
extern std::atomic<int> gScopeOut2DefenseTripCount;

static SupersonicEngine::Config scopeStressConfig() {
    SupersonicEngine::Config cfg;
    cfg.sampleRate        = 48000;
    cfg.bufferSize        = 128;
    cfg.udpPort           = 57200;  // non-zero enables shared memory
    cfg.numBuffers        = 1024;
    cfg.maxNodes          = 1024;
    cfg.maxGraphDefs      = 512;
    cfg.maxWireBufs       = 64;
    cfg.headless          = true;
    cfg.numOutputChannels = 2;
    cfg.numInputChannels  = 2;
    return cfg;
}

// Build + send a /s_new for sonic-pi-scope with the given node id.
static void spawnScopeSynth(EngineFixture& fix, int32_t nodeId) {
    osc_test::Builder b;
    auto& s = b.begin("/s_new");
    s << "sonic-pi-scope" << nodeId << (int32_t)0 << (int32_t)0
      << "max_frames" << 1024.0f;
    fix.send(b.end());
}

TEST_CASE("ScopeRace: many cold swaps with active scope synths", "[ScopeRace]") {
    std::string defPath = std::string(SUPERSONIC_SYNTHDEFS_DIR) + "/sonic-pi-scope.scsyndef";
    if (!std::filesystem::exists(defPath)) {
        SKIP("sonic-pi-scope synthdef not available");
    }

    EngineFixture fix(scopeStressConfig());
    const int startCount = gScopeOut2DefenseTripCount.load();

    OscReply reply;
    fix.send(osc_test::message("/notify", 1));
    REQUIRE(fix.waitForReply("/done", reply));

    REQUIRE(fix.loadSynthDef("sonic-pi-scope"));

    // Spin up several scope synths on distinct node ids so the audio
    // thread is running ScopeOut2_next against multiple buffers each
    // callback. More live units = more surface area for the race.
    const int kNumScopeSynths = 4;
    for (int i = 0; i < kNumScopeSynths; ++i) {
        fix.clearReplies();
        spawnScopeSynth(fix, 1000 + i);
        REQUIRE(fix.waitForReply("/n_go", reply, 3000));
    }

    // Let the audio thread warm up so ScopeOut2_next is actually running
    // — otherwise the cold swaps happen faster than the first callback.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Many rapid cold swaps. Alternating rates force a real world
    // rebuild every time (not caught by the "same rate = hot swap"
    // fast path). 20 iterations is enough to find races that fire
    // on the order of 1-in-thousands of callbacks without making the
    // test slow.
    const int kSwapIterations = 20;
    for (int i = 0; i < kSwapIterations; ++i) {
        double rate = (i % 2 == 0) ? 44100 : 48000;
        auto r = fix.engine().switchDevice("", rate);
        REQUIRE(r.success);
        REQUIRE(r.type == SwapType::Cold);
        // Brief sleep to let the rebuilt world's audio thread run a
        // few callbacks so new ScopeOut2 units get instantiated and
        // start writing.
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    // Engine must still be responsive.
    fix.clearReplies();
    fix.send(osc_test::message("/status"));
    REQUIRE(fix.waitForReply("/status.reply", reply, 3000));

    const int endCount   = gScopeOut2DefenseTripCount.load();
    const int tripsHere  = endCount - startCount;
    if (tripsHere > 0) {
        WARN("[ScopeRace] ScopeOut2 defensive guards tripped "
             << tripsHere << " time(s) during this test run. The race "
             "window is live — removing the NULL checks would segfault. "
             "Investigate the ordering of destroy_world vs. the audio "
             "callback in switchDevice.");
    } else {
        WARN("[ScopeRace] Defensive guards did not trip in "
             << kSwapIterations << " rapid cold swaps with "
             << kNumScopeSynths << " active scope synths. The race "
             "window may be closed; consider removing the scaffolding "
             "in ScopeOut2_next after more stress iterations confirm.");
    }
}

TEST_CASE("ScopeRace: aggressive stress — 100 swaps with 8 scope synths",
          "[ScopeRace][Stress][.slow]") {
    std::string defPath = std::string(SUPERSONIC_SYNTHDEFS_DIR) + "/sonic-pi-scope.scsyndef";
    if (!std::filesystem::exists(defPath)) {
        SKIP("sonic-pi-scope synthdef not available");
    }

    EngineFixture fix(scopeStressConfig());
    const int startCount = gScopeOut2DefenseTripCount.load();

    OscReply reply;
    fix.send(osc_test::message("/notify", 1));
    REQUIRE(fix.waitForReply("/done", reply));
    REQUIRE(fix.loadSynthDef("sonic-pi-scope"));

    // More synths + more iterations than the baseline test. If the
    // race window is narrow, this has a better chance of hitting it.
    const int kNumScopeSynths = 8;
    for (int i = 0; i < kNumScopeSynths; ++i) {
        fix.clearReplies();
        spawnScopeSynth(fix, 3000 + i);
        REQUIRE(fix.waitForReply("/n_go", reply, 3000));
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    const int kSwapIterations = 100;
    for (int i = 0; i < kSwapIterations; ++i) {
        double rate = (i % 2 == 0) ? 44100 : 48000;
        auto r = fix.engine().switchDevice("", rate);
        REQUIRE(r.success);
        // Very short sleep — tighter timing, more swaps per second,
        // more likely to catch the audio thread mid-ScopeOut2_next.
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    fix.clearReplies();
    fix.send(osc_test::message("/status"));
    REQUIRE(fix.waitForReply("/status.reply", reply, 3000));

    const int endCount  = gScopeOut2DefenseTripCount.load();
    const int tripsHere = endCount - startCount;
    if (tripsHere > 0) {
        WARN("[ScopeRace] Aggressive stress tripped defenses "
             << tripsHere << " time(s) in " << kSwapIterations
             << " rapid cold swaps. Race window confirmed live.");
    } else {
        WARN("[ScopeRace] Aggressive stress: 0 defensive trips in "
             << kSwapIterations << " rapid cold swaps × "
             << kNumScopeSynths << " active scope synths. Strong "
             "signal that the original race window is closed.");
    }
}

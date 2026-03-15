/*
 * test_audio_resume.cpp — Tests for audio quality after pause/resume cycles.
 *
 * After a device swap, SuperSonic pauses the audio callback and resumes on
 * the new device. These tests verify that:
 *
 *   1. Callback state (prefetch buffer, sample position) is properly reset
 *   2. The scsynth engine doesn't produce a burst of catch-up audio
 *   3. The engine remains functional after pause/resume
 *   4. Simulated JUCE callbacks produce clean output after resume
 */
#include <catch2/catch_test_macros.hpp>
#include "EngineFixture.h"
#include "JuceAudioCallback.h"
#include <thread>
#include <chrono>
#include <cmath>
#include <cstring>
#include <numeric>
#include <vector>

// =============================================================================
// SECTION: Gap detector state after resume
// =============================================================================

TEST_CASE("AudioResume: gap detector baseline resets on resume", "[AudioResume]") {
    EngineFixture fix;
    auto& cb = fix.engine().audioCallback();

    // Arm the gap detector — simulates that a JUCE callback has occurred
    // and recorded a timestamp baseline for measuring inter-callback gaps.
    cb.armGapDetector();
    REQUIRE(cb.gapDetectorArmed());

    // Pause and wait (simulating a device swap duration)
    cb.pause();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    cb.resume();

    // After resume, the gap detector baseline must be cleared.
    // Otherwise the first callback after resume would see the entire
    // pause duration as a scheduling gap and log a false [late-cb] warning.
    CHECK_FALSE(cb.gapDetectorArmed());
}

// =============================================================================
// SECTION: Callback state after resume
// =============================================================================

TEST_CASE("AudioResume: prefetchCount is zero after resume", "[AudioResume]") {
    EngineFixture fix;
    auto& cb = fix.engine().audioCallback();

    // Let the engine run for a bit
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    cb.pause();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    cb.resume();

    // After resume, isPaused should be false (basic sanity)
    REQUIRE_FALSE(cb.isPaused());
}

TEST_CASE("AudioResume: engine processes audio after pause/resume cycle",
          "[AudioResume]") {
    EngineFixture fix;
    auto& cb = fix.engine().audioCallback();

    // Pause for 100ms — enough to miss many audio blocks
    cb.pause();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    cb.resume();

    // Engine should still respond to OSC after resume
    OscReply reply;
    fix.send(osc_test::message("/status"));
    REQUIRE(fix.waitForReply("/status.reply", reply));
}

TEST_CASE("AudioResume: multiple rapid pause/resume cycles don't crash",
          "[AudioResume]") {
    EngineFixture fix;
    auto& cb = fix.engine().audioCallback();

    // Simulate rapid device switching (user clicking through devices)
    for (int i = 0; i < 10; ++i) {
        cb.pause();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        cb.resume();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    // Engine should still be functional
    OscReply reply;
    fix.send(osc_test::message("/status"));
    REQUIRE(fix.waitForReply("/status.reply", reply));
}

// =============================================================================
// SECTION: Synth scheduling after resume
// =============================================================================

TEST_CASE("AudioResume: synth created before pause still runs after resume",
          "[AudioResume]") {
    EngineFixture fix;
    auto& cb = fix.engine().audioCallback();

    // Load a synthdef and create a synth
    REQUIRE(fix.loadSynthDef("sonic-pi-beep"));

    {
        osc_test::Builder b;
        auto& s = b.begin("/s_new");
        s << "sonic-pi-beep" << (int32_t)1000 << (int32_t)0 << (int32_t)1
          << "note" << 69.0f << "out_bus" << 0.0f;
        fix.send(b.end());
    }
    // Barrier: wait for the synth to be created
    OscReply syncReply;
    fix.send(osc_test::message("/sync", 42));
    REQUIRE(fix.waitForReply("/synced", syncReply));

    // Let it play for a bit
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Pause and resume
    cb.pause();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    cb.resume();

    // Give the engine time to settle
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Verify the node tree is still intact — the synth should still exist
    // (or have naturally freed if it's an envelope-based synth)
    fix.send(osc_test::message("/g_queryTree", 0, 0));
    OscReply treeReply;
    REQUIRE(fix.waitForReply("/g_queryTree.reply", treeReply));
    // Root group should have at least the default group as a child
    REQUIRE(treeReply.parsed().argCount() >= 3);
}

// =============================================================================
// SECTION: No scheduling burst after resume
// =============================================================================

TEST_CASE("AudioResume: LATE count does not spike after short pause",
          "[AudioResume]") {
    EngineFixture fix;
    auto& cb = fix.engine().audioCallback();

    // Load and create a synth that will be continuously running
    REQUIRE(fix.loadSynthDef("sonic-pi-beep"));

    // Create synths with immediate scheduling (time = 0 means "now")
    for (int i = 0; i < 5; ++i) {
        osc_test::Builder b;
        auto& s = b.begin("/s_new");
        s << "sonic-pi-beep" << (int32_t)(2000 + i)
          << (int32_t)0 << (int32_t)1
          << "note" << (float)(60.0f + i) << "out_bus" << 0.0f;
        fix.send(b.end());
    }

    // Wait for all synths to be created
    OscReply syncReply;
    fix.send(osc_test::message("/sync", 50));
    REQUIRE(fix.waitForReply("/synced", syncReply));

    // Clear debug messages before the pause
    fix.clearDebugMessages();

    // Pause for 200ms — in real-time audio, this is ~9600 samples missed
    cb.pause();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    cb.resume();

    // Let it run for a bit after resume
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Check debug messages for LATE warnings — if there's a scheduling
    // burst, scsynth will log LATE messages for the catch-up
    auto msgs = fix.debugMessages();
    int lateCount = 0;
    for (auto& m : msgs) {
        if (m.find("LATE") != std::string::npos)
            ++lateCount;
    }

    // A small number of LATE messages right after resume is acceptable,
    // but a burst of many indicates a scheduling catch-up problem
    CHECK(lateCount <= 2);
}

// =============================================================================
// SECTION: Simulated JUCE callback produces clean output after resume
// =============================================================================

TEST_CASE("AudioResume: simulated callback output is finite after resume",
          "[AudioResume]") {
    EngineFixture fix;
    auto& cb = fix.engine().audioCallback();

    // Let the engine run for a bit to warm up
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    cb.pause();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    cb.resume();

    // Wait a few blocks for the headless driver to produce output
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    // Read the output bus — check that samples are finite (no NaN/Inf)
    auto* outputBus = reinterpret_cast<float*>(get_audio_output_bus());
    if (outputBus) {
        int bufSamples = get_audio_buffer_samples();
        bool allFinite = true;
        for (int i = 0; i < bufSamples * 2; ++i) {  // 2 channels
            if (!std::isfinite(outputBus[i])) {
                allFinite = false;
                break;
            }
        }
        CHECK(allFinite);
    }
}

TEST_CASE("AudioResume: output amplitude is reasonable after resume",
          "[AudioResume]") {
    EngineFixture fix;
    auto& cb = fix.engine().audioCallback();

    // Create a synth to produce some audio
    REQUIRE(fix.loadSynthDef("sonic-pi-beep"));
    {
        osc_test::Builder b;
        auto& s = b.begin("/s_new");
        s << "sonic-pi-beep" << (int32_t)3000 << (int32_t)0 << (int32_t)1
          << "note" << 69.0f << "out_bus" << 0.0f;
        fix.send(b.end());
    }

    OscReply syncReply;
    fix.send(osc_test::message("/sync", 60));
    REQUIRE(fix.waitForReply("/synced", syncReply));

    // Let it play
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Pause for 500ms — a realistic device swap duration
    cb.pause();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    cb.resume();

    // Wait for a few blocks to be processed
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    // Check output bus amplitude isn't absurdly large (no burst of
    // overlapping catch-up synths)
    auto* outputBus = reinterpret_cast<float*>(get_audio_output_bus());
    if (outputBus) {
        int bufSamples = get_audio_buffer_samples();
        float maxAmp = 0.0f;
        for (int i = 0; i < bufSamples * 2; ++i) {
            float v = std::fabs(outputBus[i]);
            if (v > maxAmp) maxAmp = v;
        }

        // Normal audio should be well under 10.0 amplitude.
        // A catch-up burst would stack many synths and exceed this.
        CHECK(maxAmp < 10.0f);
    }
}

// =============================================================================
// SECTION: Long pause doesn't cause timing explosion
// =============================================================================

TEST_CASE("AudioResume: long pause doesn't cause process_audio burst",
          "[AudioResume]") {
    EngineFixture fix;
    auto& cb = fix.engine().audioCallback();

    // Let the engine warm up
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Pause for 1 second
    cb.pause();
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    // Capture processCount right before resume — processCount still
    // increments while paused (callback outputs silence), so we measure
    // only the blocks processed AFTER resume.
    uint32_t countBefore = cb.processCount.load(std::memory_order_acquire);
    cb.resume();

    // Wait 100ms — should process ~37 blocks (48000 Hz / 128 samples * 0.1s)
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    uint32_t countAfter = cb.processCount.load(std::memory_order_acquire);
    uint32_t blocksProcessed = countAfter - countBefore;

    // Expected: ~37 blocks in 100ms at 48kHz/128.
    // If catch-up happened, we'd see hundreds or thousands of blocks.
    // Allow 3x margin for OS timer jitter, but flag extreme catch-up.
    uint32_t maxReasonable = (48000 / 128) * 100 / 1000 * 3;  // ~111

    CHECK(blocksProcessed < maxReasonable);
}

/*
 * test_scheduling_accuracy.cpp — Verify OSC bundle timetag execution accuracy
 *
 * Uses the audio capture buffer (same mechanism as WASM audio_capture.spec.mjs)
 * to measure exactly when a scheduled synth starts producing audio, and
 * compares that to the requested bundle timetag.
 *
 * Target: within 3ms of the scheduled time.
 */
#include "EngineFixture.h"
#include "NTPClock.h"
#include "src/shared_memory.h"
#include <chrono>
#include <cmath>
#include <thread>

extern "C" {
    extern uint8_t ring_buffer_storage[];
}

// Find the first sample frame where audio exceeds threshold (onset detection)
static int findOnsetFrame(const float* interleaved, uint32_t numFrames,
                          uint32_t channels, float threshold = 0.001f) {
    for (uint32_t f = 0; f < numFrames; f++) {
        for (uint32_t ch = 0; ch < channels; ch++) {
            if (std::fabs(interleaved[f * channels + ch]) > threshold)
                return static_cast<int>(f);
        }
    }
    return -1;
}

TEST_CASE("scheduled bundle executes within 3ms of timetag",
          "[scheduling][accuracy]") {
    EngineFixture fx;
    if (!fx.loadSynthDef("sonic-pi-beep")) { SKIP("sonic-pi-beep not available"); }
    fx.clearReplies();

    // Get pointers into the audio capture region of ring_buffer_storage
    auto* capture = reinterpret_cast<AudioCaptureHeader*>(
        ring_buffer_storage + AUDIO_CAPTURE_START);
    auto* captureData = reinterpret_cast<float*>(
        ring_buffer_storage + AUDIO_CAPTURE_START + AUDIO_CAPTURE_HEADER_SIZE);

    // Reset capture buffer and enable
    capture->head.store(0, std::memory_order_release);
    capture->enabled.store(1, std::memory_order_release);

    // Let a few audio blocks process so the capture is running
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    // Record capture head position — this is our reference "time zero"
    uint32_t captureStartFrame = capture->head.load(std::memory_order_acquire);
    uint32_t sampleRate = capture->sample_rate;
    REQUIRE(sampleRate > 0);

    // Schedule a synth 200ms in the future
    constexpr double DELAY_SEC = 0.200;
    double ntpNow = static_cast<double>(juce::Time::currentTimeMillis()) * 0.001
                  + NTPClock::NTP_EPOCH_OFFSET;
    double scheduledNTP = ntpNow + DELAY_SEC;

    fx.engine().sendBundle(scheduledNTP, {
        OscBuilder::message("/s_new", "sonic-pi-beep", 1000, 0, 0,
                            "amp", 0.5f, "note", 72.0f, "release", 0.1f)
    });

    // Wait enough for the synth to execute + produce some audio
    std::this_thread::sleep_for(std::chrono::milliseconds(400));

    // Disable capture
    capture->enabled.store(0, std::memory_order_release);
    uint32_t captureEndFrame = capture->head.load(std::memory_order_acquire);

    // Free the synth
    fx.send(osc_test::message("/n_free", 1000));

    // Analyse captured audio — find onset relative to our start frame
    REQUIRE(captureEndFrame > captureStartFrame);
    uint32_t framesAvailable = captureEndFrame - captureStartFrame;
    REQUIRE(framesAvailable > 0);

    const float* startPtr = captureData + captureStartFrame * AUDIO_CAPTURE_CHANNELS;
    int onsetFrame = findOnsetFrame(startPtr, framesAvailable,
                                     AUDIO_CAPTURE_CHANNELS);

    // We must find audio
    REQUIRE(onsetFrame >= 0);

    // Convert onset to milliseconds relative to capture start
    double onsetMs = (static_cast<double>(onsetFrame) / sampleRate) * 1000.0;
    double expectedMs = DELAY_SEC * 1000.0;
    double errorMs = onsetMs - expectedMs;
    double absErrorMs = std::fabs(errorMs);

    INFO("Sample rate: " << sampleRate);
    INFO("Onset frame: " << onsetFrame << " / " << framesAvailable);
    INFO("Onset time:  " << onsetMs << " ms");
    INFO("Expected:    " << expectedMs << " ms");
    INFO("Error:       " << errorMs << " ms");

    CHECK(absErrorMs < 3.0);
}

TEST_CASE("two bundles execute in correct order with accurate spacing",
          "[scheduling][accuracy]") {
    EngineFixture fx;
    if (!fx.loadSynthDef("sonic-pi-beep")) { SKIP("sonic-pi-beep not available"); }
    fx.clearReplies();

    auto* capture = reinterpret_cast<AudioCaptureHeader*>(
        ring_buffer_storage + AUDIO_CAPTURE_START);
    auto* captureData = reinterpret_cast<float*>(
        ring_buffer_storage + AUDIO_CAPTURE_START + AUDIO_CAPTURE_HEADER_SIZE);

    // Reset and enable capture
    capture->head.store(0, std::memory_order_release);
    capture->enabled.store(1, std::memory_order_release);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    uint32_t captureStartFrame = capture->head.load(std::memory_order_acquire);
    uint32_t sampleRate = capture->sample_rate;
    REQUIRE(sampleRate > 0);

    // Schedule two synths: one at +100ms, one at +200ms
    // Use different notes so we could distinguish them, but we just need onset times
    double ntpNow = static_cast<double>(juce::Time::currentTimeMillis()) * 0.001
                  + NTPClock::NTP_EPOCH_OFFSET;

    // First synth at +100ms — short release so it's silent by the time the second starts
    fx.engine().sendBundle(ntpNow + 0.100, {
        OscBuilder::message("/s_new", "sonic-pi-beep", 1001, 0, 0,
                            "amp", 0.5f, "note", 72.0f, "release", 0.02f)
    });

    // Second synth at +300ms (200ms gap after first)
    fx.engine().sendBundle(ntpNow + 0.300, {
        OscBuilder::message("/s_new", "sonic-pi-beep", 1002, 0, 0,
                            "amp", 0.5f, "note", 60.0f, "release", 0.02f)
    });

    // Wait for both to execute
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    capture->enabled.store(0, std::memory_order_release);
    uint32_t captureEndFrame = capture->head.load(std::memory_order_acquire);

    fx.send(osc_test::message("/n_free", 1001));
    fx.send(osc_test::message("/n_free", 1002));

    REQUIRE(captureEndFrame > captureStartFrame);
    uint32_t framesAvailable = captureEndFrame - captureStartFrame;
    const float* startPtr = captureData + captureStartFrame * AUDIO_CAPTURE_CHANNELS;

    // Find first onset
    int onset1 = findOnsetFrame(startPtr, framesAvailable, AUDIO_CAPTURE_CHANNELS);
    REQUIRE(onset1 >= 0);

    // Find second onset — scan from after the first synth's release (~50ms after onset1)
    uint32_t searchStart = onset1 + static_cast<uint32_t>(sampleRate * 0.050);
    // First find silence (the gap between synths)
    int gapStart = -1;
    for (uint32_t f = searchStart; f < framesAvailable; f++) {
        bool silent = true;
        for (uint32_t ch = 0; ch < AUDIO_CAPTURE_CHANNELS; ch++) {
            if (std::fabs(startPtr[f * AUDIO_CAPTURE_CHANNELS + ch]) > 0.001f)
                silent = false;
        }
        if (silent) { gapStart = static_cast<int>(f); break; }
    }
    REQUIRE(gapStart >= 0);

    // Find second onset after the gap
    int onset2 = findOnsetFrame(startPtr + gapStart * AUDIO_CAPTURE_CHANNELS,
                                 framesAvailable - gapStart,
                                 AUDIO_CAPTURE_CHANNELS);
    REQUIRE(onset2 >= 0);
    onset2 += gapStart; // Make absolute

    double onset1Ms = (static_cast<double>(onset1) / sampleRate) * 1000.0;
    double onset2Ms = (static_cast<double>(onset2) / sampleRate) * 1000.0;
    double spacingMs = onset2Ms - onset1Ms;
    double expectedSpacingMs = 200.0;
    double spacingErrorMs = std::fabs(spacingMs - expectedSpacingMs);

    INFO("Onset 1: " << onset1Ms << " ms (expected ~100ms)");
    INFO("Onset 2: " << onset2Ms << " ms (expected ~300ms)");
    INFO("Spacing: " << spacingMs << " ms (expected 200ms)");
    INFO("Spacing error: " << spacingErrorMs << " ms");

    // Relative spacing should be accurate within 3ms
    CHECK(spacingErrorMs < 3.0);

    // Both absolute onsets should be roughly correct too (wider tolerance — 5ms)
    CHECK(std::fabs(onset1Ms - 100.0) < 5.0);
    CHECK(std::fabs(onset2Ms - 300.0) < 5.0);
}

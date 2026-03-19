/*
 * test_scheduling_accuracy.cpp — Verify OSC bundle timetag execution accuracy
 *
 * Uses the audio capture buffer (same mechanism as WASM audio_capture.spec.mjs)
 * to measure relative spacing between scheduled synth onsets.  Absolute timing
 * conflates scheduling accuracy with driver output latency, so we only test
 * that *relative* spacing matches the requested interval.
 *
 * Tolerance is expressed in audio blocks — the fundamental scheduling quantum.
 */
#include "EngineFixture.h"
#include "NTPClock.h"
#include "src/shared_memory.h"
#include <chrono>
#include <cmath>
#include <thread>
#include <vector>
#include <numeric>

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

TEST_CASE("relative scheduling accuracy across multiple bundles",
          "[scheduling][accuracy]") {
    EngineFixture fx;
    if (!fx.loadSynthDef("sonic-pi-beep")) { SKIP("sonic-pi-beep not available"); }
    fx.clearReplies();

    constexpr int    NUM_BUNDLES      = 10;
    constexpr double SPACING_SEC      = 0.100;   // 100ms between bundles
    constexpr double FIRST_DELAY_SEC  = 0.200;   // first bundle at +200ms
    constexpr double RELEASE_SEC      = 0.020;    // 20ms release — silent well before next

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

    double ntpNow = static_cast<double>(juce::Time::currentTimeMillis()) * 0.001
                  + NTPClock::NTP_EPOCH_OFFSET;

    // Schedule NUM_BUNDLES synths at regular intervals
    for (int i = 0; i < NUM_BUNDLES; i++) {
        double t = ntpNow + FIRST_DELAY_SEC + i * SPACING_SEC;
        int nodeId = 1001 + i;
        fx.engine().sendBundle(t, {
            OscBuilder::message("/s_new", "sonic-pi-beep", nodeId, 0, 0,
                                "amp", 0.5f, "note", 72.0f,
                                "release", static_cast<float>(RELEASE_SEC))
        });
    }

    // Wait for all synths to execute and release
    auto totalWaitMs = static_cast<int>(
        (FIRST_DELAY_SEC + NUM_BUNDLES * SPACING_SEC) * 1000.0 + 300);
    std::this_thread::sleep_for(std::chrono::milliseconds(totalWaitMs));

    capture->enabled.store(0, std::memory_order_release);
    uint32_t captureEndFrame = capture->head.load(std::memory_order_acquire);

    // Free any lingering synths
    for (int i = 0; i < NUM_BUNDLES; i++)
        fx.send(osc_test::message("/n_free", 1001 + i));

    REQUIRE(captureEndFrame > captureStartFrame);
    uint32_t framesAvailable = captureEndFrame - captureStartFrame;
    const float* startPtr = captureData + captureStartFrame * AUDIO_CAPTURE_CHANNELS;

    // ── Detect all onsets ────────────────────────────────────────────────
    std::vector<int> onsets;
    int searchFrom = 0;

    for (int i = 0; i < NUM_BUNDLES; i++) {
        if (static_cast<uint32_t>(searchFrom) >= framesAvailable) break;

        int onset = findOnsetFrame(
            startPtr + searchFrom * AUDIO_CAPTURE_CHANNELS,
            framesAvailable - searchFrom,
            AUDIO_CAPTURE_CHANNELS);
        if (onset < 0) break;
        onset += searchFrom;
        onsets.push_back(onset);

        // Skip past this synth's audio (~50ms) then find silence
        uint32_t skipTo = onset + static_cast<uint32_t>(sampleRate * 0.050);
        if (skipTo >= framesAvailable) break;

        int gapStart = -1;
        for (uint32_t f = skipTo; f < framesAvailable; f++) {
            bool silent = true;
            for (uint32_t ch = 0; ch < AUDIO_CAPTURE_CHANNELS; ch++) {
                if (std::fabs(startPtr[f * AUDIO_CAPTURE_CHANNELS + ch]) > 0.001f)
                    silent = false;
            }
            if (silent) { gapStart = static_cast<int>(f); break; }
        }
        if (gapStart < 0) break;
        searchFrom = gapStart;
    }

    INFO("Detected " << onsets.size() << " of " << NUM_BUNDLES << " onsets");
    REQUIRE(onsets.size() >= 8);   // tolerate up to 2 missed detections

    // ── Compute spacings ─────────────────────────────────────────────────
    double blockMs = 1000.0 * 128.0 / sampleRate;
    double expectedSpacingMs = SPACING_SEC * 1000.0;

    std::vector<double> errors;
    for (size_t i = 1; i < onsets.size(); i++) {
        double spacingMs = (static_cast<double>(onsets[i] - onsets[i - 1])
                            / sampleRate) * 1000.0;
        double error = spacingMs - expectedSpacingMs;
        errors.push_back(error);
        INFO("Spacing " << i << ": " << spacingMs
             << " ms  (error: " << error << " ms)");
    }

    double sumAbsError = 0.0;
    double maxAbsError = 0.0;
    for (double e : errors) {
        double ae = std::fabs(e);
        sumAbsError += ae;
        maxAbsError = std::max(maxAbsError, ae);
    }
    double meanAbsError = sumAbsError / errors.size();

    INFO("Block period:      " << blockMs << " ms");
    INFO("Mean spacing error: " << meanAbsError << " ms");
    INFO("Max spacing error:  " << maxAbsError << " ms");

    // Tolerance relative to block period — real audio drivers add some
    // jitter beyond the engine's scheduling quantum
    CHECK(meanAbsError < blockMs * 2);   // mean within 2 blocks
    CHECK(maxAbsError  < blockMs * 3);   // worst case within 3 blocks
}

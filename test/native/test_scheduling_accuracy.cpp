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
#include "WallClock.h"
#include "src/shared_memory.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
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
    // Default to headless.  Set SUPERSONIC_TEST_DEVICE to a driver name
    // (e.g. "Windows Audio") to test against real hardware.
    SupersonicEngine::Config cfg;
    cfg.sampleRate   = 48000;
    cfg.bufferSize   = 128;
    cfg.udpPort      = 0;
    cfg.numBuffers   = 1024;
    cfg.maxNodes     = 1024;
    cfg.maxGraphDefs = 512;
    cfg.maxWireBufs  = 64;
    const char* device = std::getenv("SUPERSONIC_TEST_DEVICE");
    if (device && device[0] != '\0') {
        cfg.headless = false;
        cfg.hardwareDevice = device;
    } else {
        cfg.headless = true;
    }
    EngineFixture fx(cfg);
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
                  + NTP_EPOCH_OFFSET;

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
    REQUIRE(onsets.size() >= 7);   // tolerate up to 3 missed detections (CI runners are noisy)

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

// ─────────────────────────────────────────────────────────────────────────
// Distribution of bundle spacing jitter.
//
// 100 bundles at 50ms intervals = 5 seconds of audio — fits in the test
// build's enlarged AUDIO_CAPTURE buffer (cmake bumps SUPERSONIC_AUDIO_
// CAPTURE_SECONDS from 1 to 10 when BUILD_TESTS=ON). 100 samples gives
// a stable p99 alongside mean / stddev / p50 / p90 / max. A spike in
// p99 means live-coders feel occasional flams even when mean is fine.
// ─────────────────────────────────────────────────────────────────────────
TEST_CASE("scheduling jitter distribution (mean/stddev/p50/p90/p99 over 100 bundles)",
          "[scheduling][accuracy]") {
    SupersonicEngine::Config cfg;
    cfg.sampleRate   = 48000;
    cfg.bufferSize   = 128;
    cfg.udpPort      = 0;
    cfg.numBuffers   = 1024;
    cfg.maxNodes     = 2048;
    cfg.maxGraphDefs = 512;
    cfg.maxWireBufs  = 64;
    const char* device = std::getenv("SUPERSONIC_TEST_DEVICE");
    if (device && device[0] != '\0') {
        cfg.headless = false;
        cfg.hardwareDevice = device;
    } else {
        cfg.headless = true;
    }
    EngineFixture fx(cfg);
    if (!fx.loadSynthDef("sonic-pi-beep")) { SKIP("sonic-pi-beep not available"); }
    fx.clearReplies();

    constexpr int    NUM_BUNDLES     = 100;
    constexpr double SPACING_SEC     = 0.050;   // 50ms — short release
    constexpr double FIRST_DELAY_SEC = 0.200;
    constexpr double RELEASE_SEC     = 0.012;   // 12ms — silent ~38ms before next

    auto* capture = reinterpret_cast<AudioCaptureHeader*>(
        ring_buffer_storage + AUDIO_CAPTURE_START);
    auto* captureData = reinterpret_cast<float*>(
        ring_buffer_storage + AUDIO_CAPTURE_START + AUDIO_CAPTURE_HEADER_SIZE);

    capture->head.store(0, std::memory_order_release);
    capture->enabled.store(1, std::memory_order_release);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    uint32_t captureStartFrame = capture->head.load(std::memory_order_acquire);
    uint32_t sampleRate = capture->sample_rate;
    REQUIRE(sampleRate > 0);

    double ntpNow = static_cast<double>(juce::Time::currentTimeMillis()) * 0.001
                  + NTP_EPOCH_OFFSET;

    for (int i = 0; i < NUM_BUNDLES; i++) {
        double t = ntpNow + FIRST_DELAY_SEC + i * SPACING_SEC;
        int nodeId = 5001 + i;
        fx.engine().sendBundle(t, {
            OscBuilder::message("/s_new", "sonic-pi-beep", nodeId, 0, 0,
                                "amp", 0.5f, "note", 72.0f,
                                "release", static_cast<float>(RELEASE_SEC))
        });
    }

    auto totalWaitMs = static_cast<int>(
        (FIRST_DELAY_SEC + NUM_BUNDLES * SPACING_SEC) * 1000.0 + 300);
    std::this_thread::sleep_for(std::chrono::milliseconds(totalWaitMs));

    capture->enabled.store(0, std::memory_order_release);
    uint32_t captureEndFrame = capture->head.load(std::memory_order_acquire);

    for (int i = 0; i < NUM_BUNDLES; i++)
        fx.send(osc_test::message("/n_free", 5001 + i));

    REQUIRE(captureEndFrame > captureStartFrame);
    uint32_t framesAvailable = captureEndFrame - captureStartFrame;
    const float* startPtr = captureData + captureStartFrame * AUDIO_CAPTURE_CHANNELS;

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

        // Skip past the synth's audio (~25ms covers 12ms release + tail)
        // then find silence to anchor the next search.
        uint32_t skipTo = onset + static_cast<uint32_t>(sampleRate * 0.025);
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
    REQUIRE(onsets.size() >= 90);   // need ~100 for stable p99; tolerate up to 10 missed

    double blockMs = 1000.0 * 128.0 / sampleRate;
    double expectedSpacingMs = SPACING_SEC * 1000.0;

    std::vector<double> absErrors;
    absErrors.reserve(onsets.size() - 1);
    for (size_t i = 1; i < onsets.size(); i++) {
        double spacingMs = (static_cast<double>(onsets[i] - onsets[i - 1])
                            / sampleRate) * 1000.0;
        absErrors.push_back(std::fabs(spacingMs - expectedSpacingMs));
    }

    double mean = std::accumulate(absErrors.begin(), absErrors.end(), 0.0)
                  / absErrors.size();
    double sqSum = 0.0;
    for (double e : absErrors) sqSum += (e - mean) * (e - mean);
    double stddev = std::sqrt(sqSum / absErrors.size());

    std::sort(absErrors.begin(), absErrors.end());
    auto percentile = [&](double p) {
        size_t idx = std::min(absErrors.size() - 1,
                              static_cast<size_t>(p / 100.0 * absErrors.size()));
        return absErrors[idx];
    };

    double p50  = percentile(50);
    double p90  = percentile(90);
    double p99  = percentile(99);
    double pmax = absErrors.back();

    INFO("Block period:     " << blockMs << " ms");
    INFO("Expected spacing: " << expectedSpacingMs << " ms");
    INFO("Spacing |error| ms: mean=" << mean << " stddev=" << stddev
         << " p50=" << p50 << " p90=" << p90 << " p99=" << p99
         << " max=" << pmax);

    // Tolerance relative to block period. Headless DSP has near-zero
    // jitter; real drivers add some. Loose enough that one slow CI run
    // doesn't flake but tight enough that a regression of a few blocks
    // is caught.
    CHECK(p50  < blockMs * 1);   // half within one block
    CHECK(p90  < blockMs * 2);   // 90% within two blocks
    CHECK(p99  < blockMs * 3);   // tail within three blocks
    CHECK(pmax < blockMs * 4);   // worst within four blocks
}

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
#include "clock/clock_math.h"   // wallClockNTP; WallClock.h was a shim for this
#include "shared_memory.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <vector>
#include <numeric>

extern "C" {
    extern uint8_t arena_storage[];
}

// The engine the cases measure. Headless, the fixture's pumped engine on a
// freewheel clock from a fixed origin: frame f is heard at kOriginNtp +
// f / rate, the same render in every run. Or the device CLOCKWORK_TEST_DEVICE
// names (e.g. "MacBook Pro Speakers"; a name that matches no device opens the
// default), on the wall clock it keeps.
constexpr double kOriginNtp = 3'900'000'000.0;

static const char* testDevice() {
    const char* device = std::getenv("CLOCKWORK_TEST_DEVICE");
    return device && device[0] != '\0' ? device : nullptr;
}

static ClockworkEngine::Config accuracyConfig(int maxNodes) {
    const char* device = testDevice();
    ClockworkEngine::Config cfg = device ? EngineFixture::driverConfig() : EngineFixture::defaultConfig();
    setGuestOption(cfg, "maxNodes", maxNodes);
    cfg.freewheelClock = true;
    if (device) {
        cfg.headless       = false;
        cfg.hardwareDevice = device;
    } else {
        cfg.freewheelOriginNtp = kOriginNtp;
    }
    return cfg;
}

// Now on the engine's clock: the wall clock a device keeps, or, headless, the
// time the next block is heard at, from the blocks rendered so far.
static double engineNow(EngineFixture& fx, uint32_t sampleRate) {
    if (testDevice()) return wallClockNTP();
    const uint32_t blocks = fx.engine().processor().processCount.load(std::memory_order_acquire);
    return kOriginNtp + double(blocks) * 128.0 / double(sampleRate);
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

// The OUT tap read the way a client reads it: from the writer's cursor at
// the start, pulled while the engine renders, until `neededFrames` are in
// hand. Nothing is reset and nothing is toggled — the tap flows from boot —
// and the ring may be shorter than the window, which is why the pull runs
// alongside the render rather than after it: whenever the ring is drained, a
// block more (pumped here, or waited for from a device). Returns interleaved
// frames.
static std::vector<float> captureTap(EngineFixture& fx, shm_audio_buffer* tap, uint32_t neededFrames) {
    shm_audio_buffer_reader reader(tap);
    reader.seek_to_live();
    const uint32_t channels = tap->channels;
    std::vector<float> out;
    out.reserve(static_cast<size_t>(neededFrames) * channels);
    std::vector<float> chunk(4096 * channels);
    // Every block brings 128 frames, so twice the blocks the window needs is
    // a render that has stopped, not a slow one.
    uint32_t blocks = 0;
    while (out.size() < static_cast<size_t>(neededFrames) * channels) {
        uint64_t gap = 0;
        const uint32_t n = reader.pull(chunk.data(), 4096, &gap);
        REQUIRE(gap == 0);
        if (n == 0) {
            REQUIRE(++blocks <= 2 * (neededFrames / 128 + 1));
            fx.pumpBlock();
            continue;
        }
        out.insert(out.end(), chunk.begin(), chunk.begin() + static_cast<size_t>(n) * channels);
    }
    return out;
}

// On hardware, the engine must really be on it, and say which. An engine that
// cannot open the device it was given runs headless instead, and the case
// would then pass on the headless clock having measured no device at all.
static void requireHardware(EngineFixture& fx) {
    if (!testDevice()) return;
    const auto cur = fx.engine().currentDevice();
    WARN("measured on " << cur.typeName << " / " << cur.name << " at "
                        << cur.activeSampleRate << " Hz");
    REQUIRE(fx.engine().audioSource() == ClockworkEngine::AudioSource::RealCallback);
}

TEST_CASE("relative scheduling accuracy across multiple bundles",
          "[scheduling][accuracy]") {
    EngineFixture fx(accuracyConfig(1024));
    requireHardware(fx);
    if (!fx.loadSynthDef("sonic-pi-beep")) { SKIP("sonic-pi-beep not available"); }
    fx.clearReplies();

    constexpr int    NUM_BUNDLES      = 10;
    constexpr double SPACING_SEC      = 0.100;   // 100ms between bundles
    constexpr double FIRST_DELAY_SEC  = 0.200;   // first bundle at +200ms
    constexpr double RELEASE_SEC      = 0.020;    // 20ms release — silent well before next

    auto* capture = reinterpret_cast<shm_audio_buffer*>(arena_storage + SCOPE_START) + CLOCKWORK_TAP_OUT;
    REQUIRE(capture->enabled.load(std::memory_order_acquire) == 1);
    const uint32_t channels = capture->channels;
    uint32_t sampleRate = capture->sample_rate;
    REQUIRE(sampleRate > 0);

    double ntpNow = engineNow(fx, sampleRate);

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

    const uint32_t neededFrames = static_cast<uint32_t>(
        (FIRST_DELAY_SEC + NUM_BUNDLES * SPACING_SEC + 0.1) * sampleRate);
    const std::vector<float> captured = captureTap(fx, capture, neededFrames);

    // Free any lingering synths
    for (int i = 0; i < NUM_BUNDLES; i++)
        fx.send(osc_test::message("/n_free", 1001 + i));

    const uint32_t framesAvailable = static_cast<uint32_t>(captured.size() / channels);
    const float* startPtr = captured.data();

    // ── Detect all onsets ────────────────────────────────────────────────
    std::vector<int> onsets;
    int searchFrom = 0;

    for (int i = 0; i < NUM_BUNDLES; i++) {
        if (static_cast<uint32_t>(searchFrom) >= framesAvailable) break;

        int onset = findOnsetFrame(
            startPtr + searchFrom * channels,
            framesAvailable - searchFrom,
            channels);
        if (onset < 0) break;
        onset += searchFrom;
        onsets.push_back(onset);

        // Skip past this synth's audio (~50ms) then find silence
        uint32_t skipTo = onset + static_cast<uint32_t>(sampleRate * 0.050);
        if (skipTo >= framesAvailable) break;

        int gapStart = -1;
        for (uint32_t f = skipTo; f < framesAvailable; f++) {
            bool silent = true;
            for (uint32_t ch = 0; ch < channels; ch++) {
                if (std::fabs(startPtr[f * channels + ch]) > 0.001f)
                    silent = false;
            }
            if (silent) { gapStart = static_cast<int>(f); break; }
        }
        if (gapStart < 0) break;
        searchFrom = gapStart;
    }

    INFO("Detected " << onsets.size() << " of " << NUM_BUNDLES << " onsets");
    // Headless, the render is the same in every run, so every onset is in it;
    // a device's may lose up to 3 to the machine.
    REQUIRE(onsets.size() >= (testDevice() ? 7u : size_t{NUM_BUNDLES}));

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

    // The freewheel clock makes dispatch sample-deterministic, so the only
    // error is block-quantization of the 37.5-block spacing (±0.5 block =
    // 1.33 ms here). Verified stable even under full CPU load, so these
    // bounds are tight enough to catch a ≥1-block scheduling regression
    // without flaking on contended CI runners.
    CHECK(meanAbsError < blockMs * 1);    // mean within one block
    CHECK(maxAbsError  < blockMs * 1.5);  // worst case within 1.5 blocks
}

// ─────────────────────────────────────────────────────────────────────────
// Distribution of bundle spacing jitter.
//
// 100 bundles at 50ms intervals = 5 seconds of audio, which fits the
// test build's 10-second ring (CMake sets CLOCKWORK_SCOPE_SECONDS
// to 10 when BUILD_TESTS=ON). 100 samples gives a stable p99 alongside
// mean / stddev / p50 / p90 / max. A spike in p99 means live-coders
// feel occasional flams even when the mean is fine.
// ─────────────────────────────────────────────────────────────────────────
TEST_CASE("scheduling jitter distribution (mean/stddev/p50/p90/p99 over 100 bundles)",
          "[scheduling][accuracy]") {
    EngineFixture fx(accuracyConfig(2048));
    requireHardware(fx);
    if (!fx.loadSynthDef("sonic-pi-beep")) { SKIP("sonic-pi-beep not available"); }
    fx.clearReplies();

    constexpr int    NUM_BUNDLES     = 100;
    constexpr double SPACING_SEC     = 0.050;   // 50ms — short release
    constexpr double FIRST_DELAY_SEC = 0.200;
    constexpr double RELEASE_SEC     = 0.012;   // 12ms — silent ~38ms before next

    auto* capture = reinterpret_cast<shm_audio_buffer*>(arena_storage + SCOPE_START) + CLOCKWORK_TAP_OUT;
    REQUIRE(capture->enabled.load(std::memory_order_acquire) == 1);
    const uint32_t channels = capture->channels;
    uint32_t sampleRate = capture->sample_rate;
    REQUIRE(sampleRate > 0);

    double ntpNow = engineNow(fx, sampleRate);

    for (int i = 0; i < NUM_BUNDLES; i++) {
        double t = ntpNow + FIRST_DELAY_SEC + i * SPACING_SEC;
        int nodeId = 5001 + i;
        fx.engine().sendBundle(t, {
            OscBuilder::message("/s_new", "sonic-pi-beep", nodeId, 0, 0,
                                "amp", 0.5f, "note", 72.0f,
                                "release", static_cast<float>(RELEASE_SEC))
        });
    }

    const uint32_t neededFrames = static_cast<uint32_t>(
        (FIRST_DELAY_SEC + NUM_BUNDLES * SPACING_SEC + 0.1) * sampleRate);
    const std::vector<float> captured = captureTap(fx, capture, neededFrames);

    for (int i = 0; i < NUM_BUNDLES; i++)
        fx.send(osc_test::message("/n_free", 5001 + i));

    const uint32_t framesAvailable = static_cast<uint32_t>(captured.size() / channels);
    const float* startPtr = captured.data();

    std::vector<int> onsets;
    int searchFrom = 0;
    for (int i = 0; i < NUM_BUNDLES; i++) {
        if (static_cast<uint32_t>(searchFrom) >= framesAvailable) break;
        int onset = findOnsetFrame(
            startPtr + searchFrom * channels,
            framesAvailable - searchFrom,
            channels);
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
            for (uint32_t ch = 0; ch < channels; ch++) {
                if (std::fabs(startPtr[f * channels + ch]) > 0.001f)
                    silent = false;
            }
            if (silent) { gapStart = static_cast<int>(f); break; }
        }
        if (gapStart < 0) break;
        searchFrom = gapStart;
    }

    INFO("Detected " << onsets.size() << " of " << NUM_BUNDLES << " onsets");
    // Headless, every onset (see above); a device may lose up to 10 of the ~100
    // a stable p99 needs.
    REQUIRE(onsets.size() >= (testDevice() ? 90u : size_t{NUM_BUNDLES}));

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

    // The freewheel clock makes dispatch sample-deterministic (no wall-clock
    // drift), so jitter is just block-quantization and stays put under CPU
    // contention instead of spiking on a busy CI runner. Tightened well below
    // the old contention-driven bounds while still catching a multi-block
    // regression.
    CHECK(p50  < blockMs * 1);    // half within one block
    CHECK(p90  < blockMs * 1.5);  // 90% within 1.5 blocks
    CHECK(p99  < blockMs * 2);    // tail within two blocks
    CHECK(pmax < blockMs * 3);    // worst within three blocks
}

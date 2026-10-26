/*
 * test_multichannel.cpp — Multichannel input/output and recording tests
 *
 * Verifies that the engine boots and processes audio correctly with
 * various channel configurations (mono, stereo, 4ch, 8ch, 16ch).
 */
#include "EngineFixture.h"

#include <thread>
#include <chrono>
#include <filesystem>
#include <cstring>
#include <algorithm>
#include <cmath>

extern "C" {
    uintptr_t get_audio_output_bus();
    uintptr_t get_audio_input_bus();
}

// ── Helper: boot engine with given channel config ────────────────────────────

// The fixture's pumped config, at these widths.
static ClockworkEngine::Config channels(int outCh, int inCh) {
    ClockworkEngine::Config cfg = EngineFixture::defaultConfig();
    cfg.numOutputChannels = outCh;
    cfg.numInputChannels  = inCh;
    return cfg;
}

static void bootAndVerify(int outCh, int inCh) {
    EngineFixture fx(channels(outCh, inCh));
    REQUIRE(fx.engine().isRunning());
    // Render a few blocks at this width: a channel count the renderer cannot
    // handle crashes here.
    REQUIRE(fx.waitForBlocks(4));
    CHECK(fx.engine().isRunning());
}

// ── 1. Boot with various output channel counts ──────────────────────────────

TEST_CASE("Engine boots with 1 output channel", "[multichannel]") {
    bootAndVerify(1, 0);
}

TEST_CASE("Engine boots with 2 output channels (stereo)", "[multichannel]") {
    bootAndVerify(2, 2);
}

TEST_CASE("Engine boots with 4 output channels", "[multichannel]") {
    bootAndVerify(4, 0);
}

TEST_CASE("Engine boots with 8 output channels", "[multichannel]") {
    bootAndVerify(8, 0);
}

TEST_CASE("Engine boots with 16 output channels", "[multichannel]") {
    bootAndVerify(16, 0);
}

TEST_CASE("Engine boots with 32 output channels", "[multichannel]") {
    bootAndVerify(32, 0);
}

// ── 2. Boot with various input channel counts ───────────────────────────────

TEST_CASE("Engine boots with 1 input channel", "[multichannel]") {
    bootAndVerify(2, 1);
}

TEST_CASE("Engine boots with 4 input channels", "[multichannel]") {
    bootAndVerify(2, 4);
}

TEST_CASE("Engine boots with 8 input channels", "[multichannel]") {
    bootAndVerify(2, 8);
}

TEST_CASE("Engine boots with 16 input channels", "[multichannel]") {
    bootAndVerify(2, 16);
}

// ── 3. Symmetric multichannel (same in/out) ─────────────────────────────────

TEST_CASE("Engine boots with 4in/4out", "[multichannel]") {
    bootAndVerify(4, 4);
}

TEST_CASE("Engine boots with 8in/8out", "[multichannel]") {
    bootAndVerify(8, 8);
}

// ── 4. Output bus has correct layout for multichannel ────────────────────────

TEST_CASE("Output bus has correct channel-major layout", "[multichannel]") {
    int outCh = 4;
    int inCh  = 2;

    ClockworkEngine engine;
    engine.onReply = [](const uint8_t*, uint32_t) {};

    ClockworkEngine::Config cfg = channels(outCh, inCh);
    // Manual pump: this test renders blocks on the test thread, so the engine
    // must not start its own headless driver thread; two renderers would be a
    // data race. Pinned, as SUPERSONIC_TEST_REALTIME=1 would otherwise start one.
    cfg.manualAudioPump   = true;
    engine.init(cfg);

    REQUIRE(engine.isRunning());

    auto* outputBus = reinterpret_cast<float*>(get_audio_output_bus());
    REQUIRE(outputBus != nullptr);

    // Poison the entire channel-major output region with a sentinel. A
    // correctly-striding renderer overwrites every channel's full 128-sample
    // (QUANTUM_SIZE) span; with no synths running it produces exact silence,
    // so any surviving sentinel would expose a clearing/layout gap. Exact
    // equality is sound here: this thread is the only renderer, so nothing
    // races writes to the bus.
    constexpr float kSentinel = 1234.5f;
    for (int i = 0; i < outCh * 128; ++i) outputBus[i] = kSentinel;

    // Render one block on this thread.
    engine.pumpAudioBlock();

    // Every channel's full span must read exact silence.
    for (int ch = 0; ch < outCh; ++ch) {
        float maxAbs = 0.0f;
        for (int s = 0; s < 128; ++s)
            maxAbs = std::max(maxAbs, std::fabs(outputBus[ch * 128 + s]));
        INFO("output channel " << ch << " maxAbs=" << maxAbs);
        CHECK(maxAbs == 0.0f);
    }

    engine.shutdown();
}

// ── 5. Input bus has correct layout for multichannel ─────────────────────────

TEST_CASE("Input bus has correct channel-major layout", "[multichannel]") {
    int outCh = 2;
    int inCh  = 4;

    ClockworkEngine engine;
    engine.onReply = [](const uint8_t*, uint32_t) {};

    ClockworkEngine::Config cfg = channels(outCh, inCh);
    // Manual pump: this test renders blocks on the test thread, so the engine
    // must not start its own headless driver thread; two renderers would be a
    // data race. Pinned, as SUPERSONIC_TEST_REALTIME=1 would otherwise start one.
    cfg.manualAudioPump   = true;
    engine.init(cfg);

    REQUIRE(engine.isRunning());

    auto* inputBus = reinterpret_cast<float*>(get_audio_input_bus());
    REQUIRE(inputBus != nullptr);
    auto* outputBus = reinterpret_cast<float*>(get_audio_output_bus());
    REQUIRE(outputBus != nullptr);

    // Distinct per-channel pattern in the input region (simulating JUCE's
    // input copy), so a channel-major layout error would surface as a wrong
    // or zero value in the readback below.
    auto inVal = [](int ch) { return static_cast<float>(ch + 1) * 0.1f; };
    for (int ch = 0; ch < inCh; ++ch)
        for (int s = 0; s < 128; ++s)
            inputBus[ch * 128 + s] = inVal(ch);

    // Poison the output region: no synth routes input->output, so the block
    // must leave exact silence.
    constexpr float kSentinel = 1234.5f;
    for (int i = 0; i < outCh * 128; ++i) outputBus[i] = kSentinel;

    // Render one block on this thread.
    engine.pumpAudioBlock();

    // Output is exact silence across every channel.
    for (int ch = 0; ch < outCh; ++ch) {
        float maxAbs = 0.0f;
        for (int s = 0; s < 128; ++s)
            maxAbs = std::max(maxAbs, std::fabs(outputBus[ch * 128 + s]));
        INFO("output channel " << ch << " maxAbs=" << maxAbs);
        CHECK(maxAbs == 0.0f);
    }

    // Input region is read but not clobbered: each channel still holds its
    // distinct value across its full contiguous span (channel-major).
    for (int ch = 0; ch < inCh; ++ch) {
        bool intact = true;
        for (int s = 0; s < 128; ++s)
            if (inputBus[ch * 128 + s] != inVal(ch)) { intact = false; break; }
        INFO("input channel " << ch << " expected " << inVal(ch));
        CHECK(intact);
    }

    engine.shutdown();
}

// ── 6. Prefetch buffer is correctly sized ────────────────────────────────────

TEST_CASE("Prefetch buffer resizes for channel count", "[multichannel]") {
    // The device path keeps the rest of a block it rendered when the device
    // asks for fewer frames than a block (ClockworkProcessor's prefetch), a
    // block per output channel. A device of 100 frames against 128-frame
    // blocks takes that path every callback, so a prefetch sized for fewer
    // channels than the device has is written past its end (once hardcoded
    // at 8: 16 outputs crashed). Only the device path has a prefetch; the
    // headless driver renders whole blocks.
    for (const int outCh : {8, 16}) {
        INFO(outCh << " outputs");
        ClockworkEngine engine;
        engine.onReply = [](const uint8_t*, uint32_t) {};
        ClockworkEngine::Config cfg = channels(outCh, 0);
        cfg.manualAudioPump = true;   // pumpDevice's premise, whatever SUPERSONIC_TEST_REALTIME says
        engine.init(cfg);
        REQUIRE(engine.isRunning());

        // Nothing plays, so every callback's whole buffer is silence: each
        // one written in full, from the prefetch and the block after it.
        constexpr int kFrames = 100;
        constexpr float kSentinel = 1234.5f;
        std::vector<std::vector<float>> out(outCh, std::vector<float>(kFrames));
        std::vector<float*> outs;
        for (auto& ch : out) outs.push_back(ch.data());
        int unwritten = 0;
        for (int callback = 0; callback < 32; ++callback) {   // 25 blocks
            for (auto& ch : out) std::fill(ch.begin(), ch.end(), kSentinel);
            engine.pumpDevice(nullptr, 0, outs.data(), outCh, kFrames);
            for (const auto& ch : out)
                unwritten += static_cast<int>(std::count_if(ch.begin(), ch.end(), [](float s) { return s != 0.0f; }));
        }
        CHECK(unwritten == 0);
        engine.shutdown();
    }
}

// ── 7. Recording start/stop API ─────────────────────────────────────────────

// The engine's recording API is gone: the engine opens no files. Session
// recording is the front's (front/Recorder.h; test_front_recording.cpp).

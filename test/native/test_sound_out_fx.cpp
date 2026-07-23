/*
 * test_sound_out_fx.cpp — routing semantics of the shipped
 * sonic-pi-fx_sound_out / sonic-pi-fx_sound_out_stereo synthdefs against a
 * multichannel output bus.
 *
 * The :sound_out FX family has a routing contract that is easy to misread
 * from the client side and worth pinning at the engine boundary:
 *
 *  - `output` is 1-indexed: output=3 writes the mono mix to hardware
 *    channel 3 (output bus index 2) and only that channel — the direct
 *    write must never fold back into channels 1/2.
 *  - The FX input always passes through to `out_bus` regardless of `mix`:
 *    the synthdef's wet signal IS its dry signal, so the def-fx mix
 *    crossfade blends the signal with itself and `mix` is a no-op here.
 *  - `amp` scales only the passthrough. `amp: 0` silences the context
 *    output while the direct hardware write (taken pre-amp) stays at
 *    full level — the documented way to send *only* to the chosen channel
 *    (Sonic Pi tutorial 13.3, "Direct Out").
 */
#include "EngineFixture.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

extern "C" {
    uintptr_t get_audio_output_bus();
}

namespace {

constexpr int   kOutCh   = 4;
constexpr int   kBlock   = 128;
// Private audio bus for the source→FX wire: beyond the 4 output + 2 input
// hardware buses of the quad test world.
constexpr float kInBus   = 16.0f;
constexpr float kSilent  = 1e-6f;
constexpr float kAudible = 0.05f;

SupersonicEngine::Config quadConfig() {
    auto cfg = EngineFixture::defaultConfig();
    // Manual pump: assertions read the output bus directly on the test
    // thread, which must therefore be the sole process_audio() caller.
    cfg.manualAudioPump   = true;
    cfg.numOutputChannels = kOutCh;
    return cfg;
}

float channelMaxAbs(int ch) {
    auto* bus = reinterpret_cast<const float*>(get_audio_output_bus());
    REQUIRE(bus != nullptr);
    float m = 0.0f;
    for (int s = 0; s < kBlock; ++s)
        m = std::max(m, std::fabs(bus[ch * kBlock + s]));
    return m;
}

// Mono sine at the head of the root group, writing to the private bus.
void spawnSource(EngineFixture& fix, int32_t nodeId, float bus, float freq) {
    osc_test::Builder b;
    auto& s = b.begin("/s_new");
    s << "fft_test_sine" << nodeId << (int32_t)0 << (int32_t)0
      << "out" << bus << "freq" << freq << "amp" << 0.5f;
    fix.send(b.end());
}

// sound_out FX at the tail of the root group so it executes after the source.
void spawnFx(EngineFixture& fix, const char* synthdef, int32_t nodeId,
             float output, float amp, float mix) {
    osc_test::Builder b;
    auto& s = b.begin("/s_new");
    s << synthdef << nodeId << (int32_t)1 << (int32_t)0
      << "in_bus" << kInBus << "out_bus" << 0.0f
      << "output" << output << "amp" << amp << "mix" << mix;
    fix.send(b.end());
}

void setControl(EngineFixture& fix, int32_t nodeId, const char* name, float value) {
    osc_test::Builder b;
    auto& s = b.begin("/n_set");
    s << nodeId << name << value;
    fix.send(b.end());
}

// Wait until all preceding commands have been processed, then render a few
// blocks so the output bus reflects the current graph.
void syncAndRender(EngineFixture& fix, int32_t syncId) {
    fix.clearReplies();
    fix.send(osc_test::message("/sync", syncId));
    OscReply reply;
    REQUIRE(fix.waitForReply("/synced", reply));
    fix.pumpBlock(4);
}

} // namespace

TEST_CASE("sound_out: output is 1-indexed and the direct write lands only on its channel",
          "[sound_out_fx]") {
    EngineFixture fix(quadConfig());
    REQUIRE(fix.loadSynthDef("fft_test_sine"));
    REQUIRE(fix.loadSynthDef("sonic-pi-fx_sound_out"));

    spawnSource(fix, 1000, kInBus, 440.0f);
    // amp: 0 mutes the passthrough so only the direct write is audible.
    spawnFx(fix, "sonic-pi-fx_sound_out", 1001, /*output*/ 3.0f, /*amp*/ 0.0f, /*mix*/ 1.0f);
    syncAndRender(fix, 1);

    CHECK(channelMaxAbs(0) < kSilent);
    CHECK(channelMaxAbs(1) < kSilent);
    CHECK(channelMaxAbs(2) > kAudible);   // output: 3 → bus index 2
    CHECK(channelMaxAbs(3) < kSilent);
}

TEST_CASE("sound_out: passthrough to out_bus is unaffected by mix",
          "[sound_out_fx]") {
    EngineFixture fix(quadConfig());
    REQUIRE(fix.loadSynthDef("fft_test_sine"));
    REQUIRE(fix.loadSynthDef("sonic-pi-fx_sound_out"));

    spawnSource(fix, 1000, kInBus, 440.0f);
    spawnFx(fix, "sonic-pi-fx_sound_out", 1001, /*output*/ 3.0f, /*amp*/ 1.0f, /*mix*/ 0.0f);
    syncAndRender(fix, 1);

    // mix: 0 does NOT bypass the FX chain output: the dry feed still
    // reaches channel 1, alongside the direct write on channel 3.
    CHECK(channelMaxAbs(0) > kAudible);
    CHECK(channelMaxAbs(2) > kAudible);

    setControl(fix, 1001, "mix", 1.0f);
    syncAndRender(fix, 2);

    // Identical routing at mix: 1 — wet == dry, so the crossfade is a no-op.
    CHECK(channelMaxAbs(0) > kAudible);
    CHECK(channelMaxAbs(2) > kAudible);
}

TEST_CASE("sound_out: amp 0 mutes the passthrough but not the direct write",
          "[sound_out_fx]") {
    EngineFixture fix(quadConfig());
    REQUIRE(fix.loadSynthDef("fft_test_sine"));
    REQUIRE(fix.loadSynthDef("sonic-pi-fx_sound_out"));

    spawnSource(fix, 1000, kInBus, 440.0f);
    spawnFx(fix, "sonic-pi-fx_sound_out", 1001, /*output*/ 3.0f, /*amp*/ 1.0f, /*mix*/ 1.0f);
    syncAndRender(fix, 1);

    CHECK(channelMaxAbs(0) > kAudible);
    CHECK(channelMaxAbs(2) > kAudible);

    setControl(fix, 1001, "amp", 0.0f);
    syncAndRender(fix, 2);

    CHECK(channelMaxAbs(0) < kSilent);    // passthrough gone
    CHECK(channelMaxAbs(2) > kAudible);   // direct write survives amp: 0
}

TEST_CASE("sound_out_stereo: writes left/right to consecutive 1-indexed channels",
          "[sound_out_fx]") {
    EngineFixture fix(quadConfig());
    REQUIRE(fix.loadSynthDef("fft_test_sine"));
    REQUIRE(fix.loadSynthDef("sonic-pi-fx_sound_out_stereo"));

    // Distinct sines on both halves of the FX's stereo input pair.
    spawnSource(fix, 1000, kInBus, 440.0f);
    spawnSource(fix, 1001, kInBus + 1.0f, 660.0f);
    spawnFx(fix, "sonic-pi-fx_sound_out_stereo", 1002, /*output*/ 3.0f, /*amp*/ 0.0f, /*mix*/ 1.0f);
    syncAndRender(fix, 1);

    CHECK(channelMaxAbs(0) < kSilent);
    CHECK(channelMaxAbs(1) < kSilent);
    CHECK(channelMaxAbs(2) > kAudible);   // left → output: 3 → bus index 2
    CHECK(channelMaxAbs(3) > kAudible);   // right → next channel up
}

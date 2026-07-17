// Integration tests for LinkTempo / LinkPhase / LinkJump UGens.

#ifdef SUPERSONIC_LINK

#include "EngineFixture.h"
#include "FakeLinkPeerProcess.h"
#include "JuceAudioCallback.h"
#include "OscTestUtils.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>
#include <vector>

namespace {

// Config for the probes below. snapshotOutputBus() reads the shared output bus,
// so no audio thread may be writing it while we look: manualAudioPump starts no
// audio source (see SupersonicEngine::startAudioSource), leaving the test thread
// the only caller of process_audio. Under the HeadlessDriver this read raced the
// driver's clear-then-fill.
//
// Deliberately no freewheelClock. Freewheel only makes *NTP* sample-derived,
// whereas beat phase comes from Ableton Link's own clock
// (LinkSession::clockMicros -> link.clock().micros()), which is real time and
// unaffected by either. So pumping decides when we render; only wall time
// advances the phase, which is why the sleeps below must stay.
SupersonicEngine::Config linkProbeConfig() {
    auto cfg = EngineFixture::defaultConfig();
    cfg.manualAudioPump = true;
    return cfg;
}

bool snapshotOutputBus(uint32_t blockSize, std::vector<float>& out) {
    const auto* outBus = reinterpret_cast<const float*>(get_audio_output_bus());
    if (!outBus) return false;
    out.assign(outBus, outBus + blockSize);
    return true;
}

// Render one block on this thread, then read it. Rendering and reading are
// ordered on the same thread, so the snapshot is a whole, untorn block.
bool pumpAndSnapshot(EngineFixture& fx, uint32_t blockSize, std::vector<float>& out) {
    fx.pumpBlock();
    return snapshotOutputBus(blockSize, out);
}

float peakAbs(const std::vector<float>& samples) {
    float p = 0.0f;
    for (auto s : samples) p = std::max(p, std::fabs(s));
    return p;
}

float meanAbs(const std::vector<float>& samples) {
    if (samples.empty()) return 0.0f;
    double acc = 0.0;
    for (auto s : samples) acc += std::fabs(s);
    return static_cast<float>(acc / static_cast<double>(samples.size()));
}

float mean(const std::vector<float>& samples) {
    if (samples.empty()) return 0.0f;
    double acc = 0.0;
    for (auto s : samples) acc += s;
    return static_cast<float>(acc / static_cast<double>(samples.size()));
}

// Used to exclude the -1.0 disabled-Link sentinel (peakAbs alone accepts it).
bool allAbove(const std::vector<float>& samples, float threshold) {
    for (auto s : samples) if (s <= threshold) return false;
    return true;
}

}  // namespace

// Solo-engine: peer arbitration is non-deterministic, so we only
// assert on the local default (120 BPM → 2.0 CPS).
TEST_CASE("LinkUGen: LinkTempo.kr outputs session tempo in CPS",
          "[Link][LinkUGen][integration]") {
    EngineFixture fx(linkProbeConfig());
    fx.send(osc_test::message("/clock/visibility", int32_t{1}));   // LoopbackOnly

    REQUIRE(fx.loadSynthDef("link_tempo_probe"));
    {
        osc_test::Builder b;
        auto& s = b.begin("/s_new");
        s << "link_tempo_probe" << static_cast<int32_t>(2400)
          << static_cast<int32_t>(0) << static_cast<int32_t>(1)
          << "out" << 0.0f;
        fx.send(b.end());
    }
    {
        OscReply r;
        fx.send(osc_test::message("/sync", 42));
        REQUIRE(fx.waitForReply("/synced", r));
    }

    constexpr uint32_t kBlockSize = 128;
    constexpr float    kExpectedCps = 120.0f / 60.0f;  // 2.0
    std::vector<float> bus;
    float observed = 0.0f;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        REQUIRE(pumpAndSnapshot(fx, kBlockSize, bus));
        observed = mean(bus);
        if (allAbove(bus, 0.0f) && std::fabs(observed - kExpectedCps) < 0.05f) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    INFO("observed CPS=" << observed << " (expected " << kExpectedCps << ")");
    CHECK(allAbove(bus, 0.0f));
    CHECK(std::fabs(observed - kExpectedCps) < 0.05f);

    fx.send(osc_test::message("/n_free", 2400));
}

TEST_CASE("LinkUGen: LinkPhase.kr outputs phase in [0, quantum)",
          "[Link][LinkUGen][integration]") {
    EngineFixture fx(linkProbeConfig());
    fx.send(osc_test::message("/clock/visibility", int32_t{1}));   // LoopbackOnly

    REQUIRE(fx.loadSynthDef("link_phase_probe"));
    {
        osc_test::Builder b;
        auto& s = b.begin("/s_new");
        s << "link_phase_probe" << static_cast<int32_t>(2401)
          << static_cast<int32_t>(0) << static_cast<int32_t>(1)
          << "out" << 0.0f << "quantum" << 4.0f;
        fx.send(b.end());
    }
    {
        OscReply r;
        fx.send(osc_test::message("/sync", 42));
        REQUIRE(fx.waitForReply("/synced", r));
    }

    constexpr uint32_t kBlockSize = 128;
    std::vector<float> bus;
    bool sawValidPhase = false;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        REQUIRE(pumpAndSnapshot(fx, kBlockSize, bus));
        const float p = peakAbs(bus);
        if (allAbove(bus, -0.5f) && p > 0.001f && p < 4.0f) {
            sawValidPhase = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    INFO("peak=" << peakAbs(bus) << " mean=" << mean(bus));
    CHECK(sawValidPhase);

    fx.send(osc_test::message("/n_free", 2401));
}

// LinkJump is level-gated (matches upstream), so a long-lived jump
// synth re-forces the beat every k-block — phase stays glued near
// the target. At 120 BPM / quantum=4, natural advance would walk
// phase out of a 0.2-wide band in ~100 ms, so several consecutive
// in-band samples is evidence the UGen is re-pinning.
TEST_CASE("LinkUGen: LinkJump.kr forces beat-at-time on trigger",
          "[Link][LinkUGen][integration]") {
    EngineFixture fx(linkProbeConfig());
    fx.send(osc_test::message("/clock/visibility", int32_t{1}));   // LoopbackOnly

    REQUIRE(fx.loadSynthDef("link_phase_probe"));
    REQUIRE(fx.loadSynthDef("link_jump_trigger"));

    {
        osc_test::Builder b;
        auto& s = b.begin("/s_new");
        s << "link_phase_probe" << static_cast<int32_t>(2402)
          << static_cast<int32_t>(0) << static_cast<int32_t>(1)
          << "out" << 0.0f << "quantum" << 4.0f;
        fx.send(b.end());
    }
    {
        OscReply r;
        fx.send(osc_test::message("/sync", 42));
        REQUIRE(fx.waitForReply("/synced", r));
    }

    constexpr uint32_t kBlockSize = 128;
    std::vector<float> bus;
    const auto sessionDeadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
    bool sessionUp = false;
    while (std::chrono::steady_clock::now() < sessionDeadline) {
        REQUIRE(pumpAndSnapshot(fx, kBlockSize, bus));
        const float p = peakAbs(bus);
        if (allAbove(bus, -0.5f) && p > 0.001f && p < 4.0f) {
            sessionUp = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    REQUIRE(sessionUp);

    // Mid-quantum target avoids ambiguity with the natural phase=0 wrap.
    constexpr float kJumpTarget = 2.0f;
    {
        osc_test::Builder b;
        auto& s = b.begin("/s_new");
        s << "link_jump_trigger" << static_cast<int32_t>(2403)
          << static_cast<int32_t>(0) << static_cast<int32_t>(1)
          << "trig" << 1.0f
          << "beat" << kJumpTarget
          << "quantum" << 4.0f
          << "hard" << 1.0f;
        fx.send(b.end());
    }
    {
        OscReply r;
        fx.send(osc_test::message("/sync", 43));
        REQUIRE(fx.waitForReply("/synced", r));
    }

    // Natural phase advance at 120 BPM / quantum=4 spends only ~10% of its
    // cycle in [1.8, 2.2]; if the jump is re-pinning every block, the vast
    // majority of samples land in-band, so a >=50% in-band rate cleanly
    // separates the two regimes with headroom for K2A transients across the
    // jump discontinuity and for CI scheduling jitter. The assertion stays on
    // that window fraction rather than a single snapshot: each block is now read
    // whole (this thread renders it), but an isolated block can still sit on the
    // jump discontinuity, and the regimes differ in rate, not in any one sample.
    int totalSamples = 0;
    int inBandSamples = 0;
    float lastMean = 0.0f;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        REQUIRE(pumpAndSnapshot(fx, kBlockSize, bus));
        lastMean = mean(bus);
        ++totalSamples;
        if (std::fabs(lastMean - kJumpTarget) < 0.2f) ++inBandSamples;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    INFO("inBand=" << inBandSamples << "/" << totalSamples
         << " lastMean=" << lastMean);
    CHECK(inBandSamples * 2 >= totalSamples);

    fx.send(osc_test::message("/n_free", 2402));
    fx.send(osc_test::message("/n_free", 2403));
}

#endif  // SUPERSONIC_LINK

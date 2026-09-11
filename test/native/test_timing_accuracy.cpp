// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_timing_accuracy.cpp — is the scheduler's timing sample-regular?
 *
 * Measured in the rendered audio, not on the wire: a wire round-trip times the
 * transport, not the render. The engine runs in freewheel + manual-pump, so
 * engine time is purely sample-derived and one pumped block is exactly one
 * block of time — deterministic, no OS jitter.
 *
 * Two properties are asserted exactly, and both cancel the small wall-clock
 * offset between boot and the first send (so they are not flaky):
 *   1. Determinism — the same schedule produces the same onset block every run.
 *   2. Uniform spacing — a train of blips scheduled one fixed interval apart
 *      renders one fixed interval apart, with zero accumulation over the train.
 * A drift or a jitter of even one block in the scheduler fails these.
 */
#include <catch2/catch_test_macros.hpp>
#include "EngineFixture.h"
#include "OscBuilder.h"
#include "OscTestUtils.h"
#include "JuceAudioCallback.h"   // get_audio_output_bus / get_audio_buffer_samples

#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <vector>

#ifndef CLOCKWORK_SYNTHDEFS_DIR
#define CLOCKWORK_SYNTHDEFS_DIR ""
#endif

namespace {

constexpr int kSampleRate = 48000;
constexpr int kBlock      = 128;

ClockworkEngine::Config timingConfig() {
    auto cfg = EngineFixture::defaultConfig();
    cfg.sampleRate      = kSampleRate;
    cfg.bufferSize      = kBlock;
    cfg.manualAudioPump = true;    // this thread renders; no audio-thread race
    cfg.freewheelClock  = true;    // engine time is sample-derived and exact
    return cfg;
}

// NTP Q32.32 from 1900. base offset from wall clock; deltas are exact fixed-point.
constexpr uint64_t kNtpEpochOffsetSecs = 2208988800ULL;
uint64_t ntpNow() {
    auto now = std::chrono::system_clock::now().time_since_epoch();
    uint64_t secs = std::chrono::duration_cast<std::chrono::seconds>(now).count() + kNtpEpochOffsetSecs;
    auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count()
               - std::chrono::duration_cast<std::chrono::seconds>(now).count() * 1'000'000'000LL;
    uint64_t frac = (uint64_t)((double)nanos / 1e9 * 4294967296.0);
    return (secs << 32) | frac;
}
// Exactly `samples` of engine time, in NTP fixed-point units.
uint64_t ntpPlusSamples(uint64_t base, int64_t samples) {
    const int64_t delta = (int64_t)((double)samples / kSampleRate * 4294967296.0);
    return (uint64_t)((int64_t)base + delta);
}

osc_test::Packet blip(int32_t id, uint64_t /*unused*/) {
    // Hard onset (attack 0), short body, auto-frees on env end. Loud and
    // wide-open so per-block peak is a clean active/silent signal.
    osc_test::Builder b;
    auto& s = b.begin("/s_new");
    s << "sonic-pi-saw" << id << (int32_t)0 << (int32_t)1
      << "note" << 50.0f << "amp" << 1.0f
      << "attack" << 0.0f << "decay" << 0.0f << "sustain" << 0.02f << "release" << 0.005f;
    return b.end();
}

// #bundle + timetag + one message.
std::vector<uint8_t> bundle(uint64_t timeTag, const osc_test::Packet& m) {
    std::vector<uint8_t> out(8 + 8 + 4 + m.size());
    uint8_t* p = out.data();
    std::memcpy(p, "#bundle\0", 8); p += 8;
    for (int i = 7; i >= 0; --i) *p++ = (uint8_t)((timeTag >> (i * 8)) & 0xFF);
    uint32_t sz = m.size();
    *p++ = (sz >> 24) & 0xFF; *p++ = (sz >> 16) & 0xFF; *p++ = (sz >> 8) & 0xFF; *p++ = sz & 0xFF;
    std::memcpy(p, m.ptr(), sz);
    return out;
}

float blockPeak() {
    auto* bus = reinterpret_cast<const float*>(get_audio_output_bus());
    if (!bus) return 0.0f;
    const int n = get_audio_buffer_samples() * 2;
    float peak = 0.0f;
    for (int i = 0; i < n; ++i) { float v = std::fabs(bus[i]); if (v > peak) peak = v; }
    return peak;
}

// Pump `n` blocks one at a time, returning the per-block output peak.
std::vector<float> pumpPeaks(EngineFixture& fx, int n) {
    std::vector<float> peaks;
    peaks.reserve(n);
    for (int i = 0; i < n; ++i) { fx.pumpBlock(1); peaks.push_back(blockPeak()); }
    return peaks;
}

// Blocks where the peak rises from below `thr` to at/above it: the onsets.
std::vector<int> onsetBlocks(const std::vector<float>& peaks, float thr = 0.1f) {
    std::vector<int> onsets;
    bool active = false;
    for (int i = 0; i < (int)peaks.size(); ++i) {
        if (!active && peaks[i] >= thr) { onsets.push_back(i); active = true; }
        else if (active && peaks[i] < thr) { active = false; }
    }
    return onsets;
}

bool haveSaw() {
    return std::filesystem::exists(std::string(CLOCKWORK_SYNTHDEFS_DIR) + "/sonic-pi-saw.scsyndef");
}

// Schedule N blips STEP samples apart, starting LEAD samples ahead, and return
// the block index of each rendered onset.
std::vector<int> runTrain(EngineFixture& fx, int n, int stepSamples, int leadSamples) {
    const uint64_t base = ntpNow();
    for (int i = 0; i < n; ++i) {
        const uint64_t when = ntpPlusSamples(base, (int64_t)leadSamples + (int64_t)i * stepSamples);
        auto pkt = bundle(when, blip(3000 + i, when));
        fx.send(pkt.data(), (uint32_t)pkt.size());
    }
    const int totalSamples = leadSamples + n * stepSamples + 4 * stepSamples;
    const int blocks = totalSamples / kBlock + 8;
    return onsetBlocks(pumpPeaks(fx, blocks));
}

} // namespace

TEST_CASE("timing: a scheduled train renders at uniform sample-block spacing", "[timing]") {
    if (!haveSaw()) SKIP("sonic-pi-saw synthdef not available");
    EngineFixture fx(timingConfig());
    REQUIRE(fx.loadSynthDef("sonic-pi-saw"));

    const int N = 12;
    const int STEP = 8 * kBlock;    // 8 blocks = 1024 samples apart
    const int LEAD = 4 * kBlock;
    const auto onsets = runTrain(fx, N, STEP, LEAD);

    INFO("onset blocks: " << [&]{ std::string s; for (int o : onsets) s += std::to_string(o) + " "; return s; }());
    REQUIRE(onsets.size() == (size_t)N);

    // Every gap equals the first gap: no drift, no jitter, no accumulation.
    const int gap0 = onsets[1] - onsets[0];
    CHECK(gap0 == STEP / kBlock);   // 8 blocks
    for (size_t i = 2; i < onsets.size(); ++i)
        CHECK(onsets[i] - onsets[i - 1] == gap0);

    // And no accumulated error across the whole train.
    const int totalSpan = onsets.back() - onsets.front();
    CHECK(totalSpan == gap0 * (N - 1));
}

TEST_CASE("timing: the same schedule is bit-identical across runs (zero jitter)", "[timing]") {
    if (!haveSaw()) SKIP("sonic-pi-saw synthdef not available");

    auto measure = [] {
        EngineFixture fx(timingConfig());
        REQUIRE(fx.loadSynthDef("sonic-pi-saw"));
        // Spacing is what we compare; it is independent of the boot wall-clock,
        // so two runs must agree exactly.
        auto onsets = runTrain(fx, 6, 6 * kBlock, 4 * kBlock);
        std::vector<int> gaps;
        for (size_t i = 1; i < onsets.size(); ++i) gaps.push_back(onsets[i] - onsets[i - 1]);
        return gaps;
    };

    const auto a = measure();
    const auto b = measure();
    REQUIRE(a.size() == 5);
    CHECK(a == b);                        // identical run to run
    for (int g : a) CHECK(g == 6);        // exactly 6 blocks each, every run
}

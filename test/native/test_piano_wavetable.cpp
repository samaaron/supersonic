// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * /supersonic/piano/wavetable — MdaPiano's sample table, handed over as a
 * buffer. The table arrives like any sample (here through the inbox lane,
 * committed as an asset — SampleLane.h) and
 * the verb points the plugin at it. Without a table the piano is silent;
 * with one it sounds; a buffer too short to be a table is refused.
 */
#include "EngineFixture.h"
#include "SampleLane.h"
#include "clockwork_audio_file.h"
#include "piano_wavetable.h"
#include "JuceAudioCallback.h"  // get_audio_output_bus / get_audio_buffer_samples
#include <catch2/catch_approx.hpp>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include "TestPid.h"
#include <vector>

namespace {

const char* kVerb = "/supersonic/piano/wavetable";

// A table-sized WAV: a sine, so that a played piano has something to sound.
std::string writeTable(size_t frames) {
    const std::string path = (std::filesystem::temp_directory_path() /
        ("piano-table-" + std::to_string(testPid()) + ".wav")).string();
    ClockworkAudioWriterConfig cfg{};
    cfg.struct_bytes = sizeof(cfg);
    cfg.format = CLOCKWORK_AUDIO_FORMAT_WAV;
    cfg.encoding = CLOCKWORK_AUDIO_ENCODING_PCM_S16;
    cfg.channels = 1;
    cfg.sample_rate = 44100;
    ClockworkStatus st = CLOCKWORK_OK;
    ClockworkAudioWriter* w = clockwork_audio_writer_open(path.c_str(), &cfg, &st);
    REQUIRE(w != nullptr);
    std::vector<float> block(4096);
    size_t written = 0;
    while (written < frames) {
        const size_t n = std::min(block.size(), frames - written);
        for (size_t i = 0; i < n; ++i)
            block[i] = 0.5f * std::sin((float)((written + i) % 100) / 100.0f * 6.2831853f);
        REQUIRE(clockwork_audio_writer_write(w, block.data(), n) == CLOCKWORK_OK);
        written += n;
    }
    REQUIRE(clockwork_audio_writer_close(w, nullptr, nullptr) == CLOCKWORK_OK);
    return path;
}

bool allocRead(EngineFixture& fx, int32_t bufnum, const std::string& path) {
    return sample_lane::load(fx, bufnum, path);
}

bool setTable(EngineFixture& fx, int32_t bufnum) {
    fx.clearReplies();
    fx.send(osc_test::message(kVerb, bufnum));
    OscReply r;
    return fx.waitForReply("/done", r);
}

float peakOverBlocks(EngineFixture& fx, uint32_t blocks) {
    float peak = 0.0f;
    for (uint32_t b = 0; b < blocks; ++b) {
        fx.pumpBlock();
        auto* bus = reinterpret_cast<const float*>(get_audio_output_bus());
        if (!bus) continue;
        const int n = get_audio_buffer_samples() * 2;
        for (int i = 0; i < n; ++i) peak = std::max(peak, std::fabs(bus[i]));
    }
    return peak;
}

ClockworkEngine::Config probeConfig() {
    auto cfg = EngineFixture::defaultConfig();
    cfg.manualAudioPump = true;
    return cfg;
}

} // namespace

TEST_CASE("/supersonic/piano/wavetable: a buffer becomes the plugin's table", "[piano]") {
    const size_t frames = supersonic_piano_wavetable_min_frames() + 2;
    const std::string path = writeTable(frames);
    EngineFixture fx;
    REQUIRE(allocRead(fx, 30, path));
    REQUIRE(setTable(fx, 30));
    CHECK(supersonic_piano_wavetable_frames() == frames);

    // The plugin keeps its own copy: the buffer can go.
    fx.clearReplies();
    fx.send(osc_test::message("/b_free", 30));
    OscReply freed;
    REQUIRE(fx.waitForReply("/done", freed));
    CHECK(supersonic_piano_wavetable_frames() == frames);

    REQUIRE(setTable(fx, -1));
    CHECK(supersonic_piano_wavetable_frames() == 0);
    std::remove(path.c_str());
}

TEST_CASE("/supersonic/piano/wavetable: a short buffer is refused, the table untouched", "[piano]") {
    EngineFixture fx;
    fx.send(osc_test::message("/b_alloc", 31, (int32_t)1000, (int32_t)1));
    OscReply done;
    REQUIRE(fx.waitForReply("/done", done));
    const size_t before = supersonic_piano_wavetable_frames();
    fx.clearReplies();
    fx.send(osc_test::message(kVerb, 31));
    OscReply fail;
    REQUIRE(fx.waitForReply("/fail", fail));
    CHECK(supersonic_piano_wavetable_frames() == before);

    fx.clearReplies();
    fx.send(osc_test::message(kVerb, 999));   // never allocated
    REQUIRE(fx.waitForReply("/fail", fail));
    fx.send(osc_test::message("/b_free", 31));
}

TEST_CASE("the piano is silent without a table and sounds with one", "[piano]") {
    EngineFixture fx(probeConfig());
    REQUIRE(fx.loadSynthDef("sonic-pi-piano"));
    REQUIRE(setTable(fx, -1));

    auto play = [&](int32_t node) {
        osc_test::Builder b;
        auto& s = b.begin("/s_new");
        s << "sonic-pi-piano" << node << int32_t{0} << int32_t{0} << "note" << 60.0f << "amp" << 1.0f;
        fx.send(b.end());
    };

    play(4001);
    const float silent = peakOverBlocks(fx, 60);
    fx.send(osc_test::message("/n_free", 4001));
    fx.pumpBlock(4);

    const size_t frames = supersonic_piano_wavetable_min_frames() + 2;
    const std::string path = writeTable(frames);
    REQUIRE(allocRead(fx, 32, path));
    REQUIRE(setTable(fx, 32));

    play(4002);
    const float sounding = peakOverBlocks(fx, 60);
    fx.send(osc_test::message("/n_free", 4002));

    CHECK(silent == 0.0f);
    CHECK(sounding > 0.01f);

    REQUIRE(setTable(fx, -1));
    std::remove(path.c_str());
}

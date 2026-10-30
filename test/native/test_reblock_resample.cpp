/*
 * test_reblock_resample.cpp — SynthDef Reblock() and Resample() (upstream
 * PR #7402, #7707): each graph runs at the block size and rate it asks for,
 * and a reblocked, resampled graph still sounds.
 *
 * Probe synthdefs write BlockSize.ir and SampleRate.ir to two control buses,
 * read back with /c_get: the rate observation is what tells an active Reblock
 * from one silently ignored, which would still produce audio.
 *
 * The test renders every block itself (the fixture's manual pump) on a
 * freewheel clock, so each case renders the same blocks on every run however
 * fast the machine is. Fixtures: test/synthdefs/reblock_resample (compiled by
 * test/synthdefs/compile_reblock_resample_synthdefs.scd).
 */
#include "EngineFixture.h"
#include "ClockworkProcessor.h"   // get_audio_output_bus / get_audio_buffer_samples

#include <catch2/catch_approx.hpp>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

constexpr double kOriginNtp = 3'900'000'000.0;

ClockworkEngine::Config config() {
    auto cfg = EngineFixture::defaultConfig();
    cfg.manualAudioPump    = true;
    cfg.freewheelClock     = true;
    cfg.freewheelOriginNtp = kOriginNtp;
    return cfg;
}

void loadFixture(EngineFixture& fx, const std::string& name) {
    const auto path = std::filesystem::path(CLOCKWORK_TEST_SYNTHDEFS_DIR) / "reblock_resample"
                    / (name + ".scsyndef");
    std::ifstream f(path, std::ios::binary);
    INFO(path.string());
    REQUIRE(f);
    const std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    REQUIRE_FALSE(data.empty());
    REQUIRE(fx.sendAndExpectDone(osc_test::messageWithBlob("/d_recv", data.data(), data.size())));
}

struct Rates { float block = -1; float rate = -1; };

// The rates a probe synth reports on `bus` and `bus + 1`, after 16 blocks:
// the first Out.kr runs in the block after /s_new, and 16 covers the largest
// reblock here (512 frames: four server blocks).
Rates probe(EngineFixture& fx, const std::string& name, int32_t bus,
            const std::vector<std::pair<std::string, float>>& controls = {}) {
    loadFixture(fx, name);
    constexpr int32_t kNode = 7000;
    osc_test::Builder s;
    auto& m = s.begin("/s_new");
    m << name.c_str() << kNode << int32_t(0) << int32_t(0) << "bus" << bus;
    for (const auto& [control, value] : controls) m << control.c_str() << value;
    fx.send(s.end());
    fx.pumpBlock(16);

    fx.clearReplies();
    osc_test::Builder g;
    g.begin("/c_get") << bus << bus + 1;
    fx.send(g.end());
    OscReply reply;
    REQUIRE(fx.waitForReply("/c_set", reply));
    const auto r = reply.parsed();
    REQUIRE(r.argCount() == 4);
    REQUIRE(r.argInt(0) == bus);
    REQUIRE(r.argInt(2) == bus + 1);

    osc_test::Builder free;
    free.begin("/n_free") << kNode;
    fx.send(free.end());
    fx.pumpBlock();
    return { r.argFloat(1), r.argFloat(3) };
}

struct Sound { double rms = 0; double peak = 0; double freq = 0; };

// The left channel of `name` playing: 50 ms skipped while it starts, then
// 200 ms read block by block off the output bus. Each block is read before
// the next renders, so nothing else writes the bus during the read.
Sound play(EngineFixture& fx, const std::string& name,
           const std::vector<std::pair<std::string, float>>& controls = {}) {
    loadFixture(fx, name);
    constexpr int32_t kNode = 5500;
    osc_test::Builder s;
    auto& m = s.begin("/s_new");
    m << name.c_str() << kNode << int32_t(0) << int32_t(0);
    for (const auto& [control, value] : controls) m << control.c_str() << value;
    fx.send(s.end());

    const int frames = get_audio_buffer_samples();
    REQUIRE(frames > 0);
    fx.pumpBlock(2400 / frames + 1);
    std::vector<float> left;
    while (left.size() < 9600) {
        fx.pumpBlock();
        const auto* bus = reinterpret_cast<const float*>(get_audio_output_bus());
        REQUIRE(bus != nullptr);
        left.insert(left.end(), bus, bus + frames);   // [L block][R block]
    }

    Sound out;
    int crossings = 0;
    for (size_t i = 0; i < left.size(); ++i) {
        out.rms += double(left[i]) * left[i];
        out.peak = std::max(out.peak, double(std::fabs(left[i])));
        if (i && ((left[i - 1] >= 0) != (left[i] >= 0))) ++crossings;
    }
    out.rms = std::sqrt(out.rms / left.size());
    out.freq = crossings / (2.0 * (left.size() / 48000.0));

    osc_test::Builder free;
    free.begin("/n_free") << kNode;
    fx.send(free.end());
    fx.pumpBlock();
    return out;
}

} // namespace

TEST_CASE("Reblock/Resample: each probe graph runs at the block size and rate it asks for",
          "[reblock][resample]") {
    struct Case { const char* name; float block; float rate; };
    const Case cases[] = {
        { "rr_probe_baseline",         128,  48000 },
        { "rr_probe_reblock32",         32,  48000 },
        { "rr_probe_reblock64",         64,  48000 },
        { "rr_probe_resample2",        128,  96000 },
        { "rr_probe_resample4",        128, 192000 },
        { "rr_probe_reblock_resample",  32,  96000 },
    };
    for (const auto& c : cases) {
        DYNAMIC_SECTION(c.name) {
            EngineFixture fx(config());
            const Rates got = probe(fx, c.name, 100);
            CHECK(got.block == c.block);
            CHECK(got.rate == c.rate);
        }
    }
}

TEST_CASE("Reblock/Resample: block size and factor given as controls",
          "[reblock][resample]") {
    struct Case {
        const char* name;
        std::vector<std::pair<std::string, float>> controls;
        float block; float rate;
    };
    // Upstream #7707: the block size may exceed the server's as long as the
    // effective size (blockSize / factor) does not, and may be smaller than
    // the factor; too large an effective size is reduced to server block
    // size * factor.
    const Case cases[] = {
        { "rr_probe_reblock_ctrl",           {},                                      16,  48000 },
        { "rr_probe_reblock_ctrl",           { { "blockSize", 64 } },                 64,  48000 },
        { "rr_probe_resample_ctrl",          {},                                     128,  96000 },
        { "rr_probe_resample_ctrl",          { { "factor", 4 } },                    128, 192000 },
        { "rr_probe_reblock_resample_ctrl",  { { "blockSize", 256 }, { "factor", 2 } }, 256,  96000 },
        { "rr_probe_reblock_resample_ctrl",  { { "blockSize", 512 }, { "factor", 4 } }, 512, 192000 },
        { "rr_probe_reblock_resample_ctrl",  { { "blockSize", 512 }, { "factor", 2 } }, 256,  96000 },
        { "rr_probe_reblock_resample_ctrl",  { { "blockSize",   1 }, { "factor", 2 } },   1,  96000 },
    };
    for (const auto& c : cases) {
        std::string label = c.name;
        for (const auto& [control, value] : c.controls) label += " " + control + "=" + std::to_string(int(value));
        DYNAMIC_SECTION(label) {
            EngineFixture fx(config());
            const Rates got = probe(fx, c.name, 110, c.controls);
            CHECK(got.block == c.block);
            CHECK(got.rate == c.rate);
        }
    }
}

TEST_CASE("Reblock/Resample: a reblocked, resampled graph still sounds a 440 Hz sine",
          "[reblock][resample][audio]") {
    struct Case { const char* name; std::vector<std::pair<std::string, float>> controls; };
    const Case cases[] = {
        { "rr_audio_reblock_resample",      {} },
        { "rr_audio_reblock_resample_ctrl", {} },
        { "rr_audio_reblock_resample_ctrl", { { "blockSize", 512 }, { "factor", 4 } } },
    };
    for (const auto& c : cases) {
        std::string label = c.name;
        for (const auto& [control, value] : c.controls) label += " " + control + "=" + std::to_string(int(value));
        DYNAMIC_SECTION(label) {
            EngineFixture fx(config());
            const Sound got = play(fx, c.name, c.controls);
            INFO("rms " << got.rms << ", peak " << got.peak << ", " << got.freq << " Hz");
            // A 0.2-amplitude sine: rms 0.2/sqrt(2). The zero-crossing count
            // over 200 ms resolves frequency to 2.5 Hz.
            CHECK(got.rms == Catch::Approx(0.2 / std::sqrt(2.0)).margin(0.002));
            CHECK(got.peak == Catch::Approx(0.2).margin(0.002));
            CHECK(std::fabs(got.freq - 440.0) <= 2.5);
        }
    }
}

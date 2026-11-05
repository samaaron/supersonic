// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Sam Aaron
/*
 * test_plural_engines.cpp — scsynth beside another scsynth renders exactly what
 * it renders alone.
 *
 * scsynth says DSP_INSTANCES_ANY: any number of it in one process, a promise
 * that it keeps nothing process-wide (clockwork's docs/BOUNDARY.md, "How many
 * in a process"). Two checks hold it to that. scripts/check-guest-globals.sh
 * reads the built objects for a mutable static; this case runs two engines at
 * once, each told something different at different blocks, block for block,
 * and compares each one's output bit for bit with the output it gives when it
 * runs alone. State the two shared — a seed sequence, a registry, a table
 * written at load, a counter — shows in one or both.
 */
#include "EngineFixture.h"
#include "ClockworkProcessor.h"   // clockwork_audio_out, clockwork_block_size
#include "dsp_api.h"
#include "OscTestUtils.h"
#include "clockwork_audio_file.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

// A message sent before the block given.
struct Cue {
    uint32_t block;
    osc_test::Packet message;
};
using Script = std::vector<Cue>;
constexpr uint32_t kScriptBlocks = 300;

osc_test::Packet play(int32_t node, float note) {
    osc_test::Builder b;
    auto& s = b.begin("/s_new");
    s << "sonic-pi-beep" << node << int32_t{0} << int32_t{0} << "note" << note << "amp" << 0.5f;
    return b.end();
}

osc_test::Packet set(int32_t node, const char* control, float value) {
    osc_test::Builder b;
    b.begin("/n_set") << node << control << value;
    return b.end();
}

// One engine running a script, block by block, its output kept: every output
// channel of every block, as the engine laid it out.
struct Run {
    EngineFixture fx;
    const Script& script;
    std::vector<float> out;
    explicit Run(const Script& s) : script(s) { REQUIRE(fx.loadSynthDef("sonic-pi-beep")); }
    void block(uint32_t b) {
        for (const Cue& c : script)
            if (c.block == b) fx.send(c.message);
        fx.pumpBlock();
        ClockworkCore* core = fx.engine().core();
        const float* bus = clockwork_audio_out(core);
        REQUIRE(bus != nullptr);
        const size_t n = static_cast<size_t>(clockwork_block_size(core)) * 2;
        out.insert(out.end(), bus, bus + n);
    }
};

std::vector<float> alone(const Script& s) {
    Run run(s);
    for (uint32_t b = 0; b < kScriptBlocks; ++b) run.block(b);
    return run.out;
}

// The first frame where `got` is not `want`, or -1 when it is the same.
long firstDifference(const std::vector<float>& got, const std::vector<float>& want) {
    if (got.size() != want.size()) return static_cast<long>(std::min(got.size(), want.size()));
    const auto at = std::mismatch(got.begin(), got.end(), want.begin());
    return at.first == got.end() ? -1 : static_cast<long>(at.first - got.begin());
}

float peak(const std::vector<float>& v) {
    float p = 0.0f;
    for (float x : v) p = std::max(p, std::fabs(x));
    return p;
}

} // namespace

TEST_CASE("plural engines: scsynth declares any number of instances in a process", "[plural]") {
    CHECK(dsp_describe()->instances == DSP_INSTANCES_ANY);
}

TEST_CASE("plural engines: scsynth beside another scsynth renders exactly what it renders alone", "[plural]") {
    const std::string def = std::string(CLOCKWORK_SYNTHDEFS_DIR) + "/sonic-pi-beep.scsyndef";
    if (!std::filesystem::exists(def)) SKIP("sonic-pi-beep synthdef not available");

    const Script a = { { 0, play(1000, 60.0f) },
                       { 120, set(1000, "note", 67.0f) },
                       { 200, play(1001, 48.0f) } };
    const Script b = { { 0, play(1000, 72.0f) },
                       { 60, osc_test::message("/n_free", 1000) },
                       { 150, play(1002, 55.0f) },
                       { 240, set(1002, "amp", 0.1f) } };

    const std::vector<float> soloA = alone(a), soloB = alone(b);
    // Two scripts that sound, and sound different, so the case can tell them apart.
    REQUIRE(peak(soloA) > 0.0f);
    REQUIRE(peak(soloB) > 0.0f);
    REQUIRE(firstDifference(soloA, soloB) >= 0);

    Run ra(a), rb(b);
    for (uint32_t blk = 0; blk < kScriptBlocks; ++blk) {
        ra.block(blk);
        rb.block(blk);
    }
    const long diffA = firstDifference(ra.out, soloA);
    const long diffB = firstDifference(rb.out, soloB);
    INFO("engine A first differs from its solo run at sample " << diffA);
    INFO("engine B first differs from its solo run at sample " << diffB);
    CHECK(diffA == -1);
    CHECK(diffB == -1);
}

// ── Three engines, one program, each its own conversation ────────────────────
//
// One program holds three engines and talks to each over its own client. Each
// engine is told a different note of a chord; one is then retuned and another
// freed, and only the engine spoken to changes. Every reply comes back on the
// connection that asked. The mix of the three is written to a WAV beside the
// system's temporary files (supersonic-three-engines.wav), so the chord can be
// heard as well as measured.
namespace {

double hz(double note) { return 440.0 * std::pow(2.0, (note - 69.0) / 12.0); }

// The power of one frequency in the left channel of `stereo` over the frames
// [from, to): a Goertzel pass, which is enough to tell three notes apart.
double powerAt(const std::vector<float>& stereo, size_t from, size_t to, double freq, double rate) {
    const double k = 2.0 * std::cos(6.283185307179586 * freq / rate);
    double s0 = 0.0, s1 = 0.0, s2 = 0.0;
    for (size_t i = from; i < to; ++i) {
        s0 = stereo[2 * i] + k * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    return s1 * s1 + s2 * s2 - k * s1 * s2;
}

float peakOver(const std::vector<float>& stereo, size_t from, size_t to) {
    float p = 0.0f;
    for (size_t i = 2 * from; i < 2 * to; ++i) p = std::max(p, std::fabs(stereo[i]));
    return p;
}

osc_test::Packet hold(int32_t node, float note) {
    osc_test::Builder b;
    auto& s = b.begin("/s_new");
    s << "sonic-pi-beep" << node << int32_t{0} << int32_t{0}
      << "note" << note << "amp" << 0.5f << "sustain" << 3.0f << "release" << 0.2f;
    return b.end();
}

struct Engine {
    EngineFixture fx;
    std::vector<float> out;
    Engine() {
        REQUIRE(fx.loadSynthDef("sonic-pi-beep"));
        fx.send(osc_test::message("/notify", 1));   // node notifications, to this client
        REQUIRE(fx.waitForDone("/notify"));
        fx.clearReplies();
    }
    void block() {
        fx.pumpBlock();
        ClockworkCore* core = fx.engine().core();
        const float* bus = clockwork_audio_out(core);
        REQUIRE(bus != nullptr);
        const size_t n = static_cast<size_t>(clockwork_block_size(core)) * 2;
        out.insert(out.end(), bus, bus + n);
    }
    // How many replies to `address` carry `node` as their first argument.
    int replies(const char* address, int32_t node) const {
        int n = 0;
        for (const OscReply& r : fx.allReplies())
            if (r.address == address && r.parsed().argInt(0) == node) ++n;
        return n;
    }
    int synths() const {
        for (const OscReply& r : fx.allReplies())
            if (r.address == "/status.reply") return r.parsed().argInt(2);
        return -1;
    }
};

} // namespace

TEST_CASE("plural engines: one program plays a chord on three engines, each told and answered on its own", "[plural]") {
    const std::string def = std::string(CLOCKWORK_SYNTHDEFS_DIR) + "/sonic-pi-beep.scsyndef";
    if (!std::filesystem::exists(def)) SKIP("sonic-pi-beep synthdef not available");

    const double rate = EngineFixture::defaultConfig().sampleRate;
    const float notes[3] = { 60.0f, 64.0f, 67.0f };   // C4 E4 G4
    const float retuned = 72.0f;                        // C5, engine 2 only
    Engine e[3];
    const uint32_t perSecond = e[0].fx.blocksOfAudio(1000);
    const size_t frames = static_cast<size_t>(clockwork_block_size(e[0].fx.engine().core()));

    // Second 0: a note each. Second 1: engine 2 retuned. Second 2: engine 3 freed.
    for (uint32_t blk = 0; blk < 3 * perSecond; ++blk) {
        if (blk == 0)
            for (int i = 0; i < 3; ++i) e[i].fx.send(hold(1000, notes[i]));
        if (blk == perSecond) e[1].fx.send(set(1000, "note", retuned));
        if (blk == 2 * perSecond) e[2].fx.send(osc_test::message("/n_free", 1000));
        for (Engine& x : e) x.block();
    }
    for (Engine& x : e) x.fx.send(osc_test::message("/status"));
    for (uint32_t blk = 0; blk < 4; ++blk)
        for (Engine& x : e) x.block();

    // Each engine answered its own client, once: a node 1000 went on all three,
    // ended on the third alone, and each reports its own count of synths.
    for (int i = 0; i < 3; ++i) {
        INFO("engine " << i + 1 << " replies:\n" << e[i].fx.repliesDump());
        CHECK(e[i].replies("/n_go", 1000) == 1);
        CHECK(e[i].replies("/n_end", 1000) == (i == 2 ? 1 : 0));
        CHECK(e[i].synths() == (i == 2 ? 0 : 1));
    }

    // What each engine played, a window in each second, well inside the note.
    const auto window = [&](uint32_t second) {
        return std::pair<size_t, size_t>{ (second * perSecond + perSecond / 4) * frames,
                                          (second * perSecond + perSecond - perSecond / 20) * frames };
    };
    const auto dominant = [&](const Engine& x, uint32_t second, float note) {
        const auto [from, to] = window(second);
        const double own = powerAt(x.out, from, to, hz(note), rate);
        double other = 0.0;
        for (float n : { notes[0], notes[1], notes[2], retuned })
            if (n != note) other = std::max(other, powerAt(x.out, from, to, hz(n), rate));
        INFO("second " << second << ": power at " << note << " is " << own << ", the loudest other note " << other);
        return own > 20.0 * other && own > 0.0;
    };
    const auto silent = [&](const Engine& x, uint32_t second) {
        const auto [from, to] = window(second);
        return peakOver(x.out, from, to) < 1e-4f;
    };

    CHECK(dominant(e[0], 0, notes[0]));
    CHECK(dominant(e[1], 0, notes[1]));
    CHECK(dominant(e[2], 0, notes[2]));

    CHECK(dominant(e[0], 1, notes[0]));
    CHECK(dominant(e[1], 1, retuned));
    CHECK(dominant(e[2], 1, notes[2]));

    CHECK(dominant(e[0], 2, notes[0]));
    CHECK(dominant(e[1], 2, retuned));
    CHECK(silent(e[2], 2));

    // The chord, mixed, as a file.
    std::vector<float> mix(e[0].out.size(), 0.0f);
    for (const Engine& x : e) {
        REQUIRE(x.out.size() == mix.size());
        for (size_t i = 0; i < mix.size(); ++i) mix[i] += x.out[i] / 3.0f;
    }
    const std::filesystem::path wav = std::filesystem::temp_directory_path() / "supersonic-three-engines.wav";
    ClockworkAudioWriterConfig wc{};
    wc.struct_bytes = sizeof wc;
    wc.format = CLOCKWORK_AUDIO_FORMAT_WAV;
    wc.encoding = CLOCKWORK_AUDIO_ENCODING_FLOAT32;
    wc.channels = 2;
    wc.sample_rate = static_cast<uint32_t>(rate);
    ClockworkStatus st = CLOCKWORK_OK;
    ClockworkAudioWriter* w = clockwork_audio_writer_open(wav.string().c_str(), &wc, &st);
    REQUIRE(w != nullptr);
    REQUIRE(clockwork_audio_writer_write(w, mix.data(), mix.size() / 2) == CLOCKWORK_OK);
    REQUIRE(clockwork_audio_writer_close(w, nullptr, nullptr) == CLOCKWORK_OK);
    CHECK(std::filesystem::file_size(wav) > mix.size() * sizeof(float));
}

// ── Three engines rendered at once, on three threads ─────────────────────────
//
// A plugin host renders its instances on several threads at the same time.
// Three engines, each on a thread of its own, render their scripts with no
// step between them, and each gives bit for bit what it gives alone. State
// the engines shared would show as a difference, and a race on it to
// ThreadSanitizer, which the sanitizer builds run this case under.
TEST_CASE("plural engines: three engines rendering at once on three threads each render what they render alone",
          "[plural][threads]") {
    const std::string def = std::string(CLOCKWORK_SYNTHDEFS_DIR) + "/sonic-pi-beep.scsyndef";
    if (!std::filesystem::exists(def)) SKIP("sonic-pi-beep synthdef not available");

    const Script scripts[3] = {
        { { 0, play(1000, 60.0f) }, { 120, set(1000, "note", 67.0f) }, { 200, play(1001, 48.0f) } },
        { { 0, play(1000, 72.0f) }, { 60, osc_test::message("/n_free", 1000) }, { 150, play(1002, 55.0f) } },
        { { 10, play(1000, 64.0f) }, { 90, set(1000, "amp", 0.2f) }, { 240, play(1003, 76.0f) } },
    };
    std::vector<float> solo[3];
    for (int i = 0; i < 3; ++i) {
        solo[i] = alone(scripts[i]);
        REQUIRE(peak(solo[i]) > 0.0f);
    }

    // Booted and loaded here, on the test thread (the fixture asserts as it
    // boots); one engine each, whatever SUPERSONIC_TEST_ENGINES says.
    std::unique_ptr<EngineFixture> engines[3];
    for (auto& e : engines) {
        e = std::make_unique<EngineFixture>(1);
        REQUIRE(e->loadSynthDef("sonic-pi-beep"));
    }
    // Each thread sends and renders its own engine's script and keeps its
    // output; nothing asserts off the test thread.
    std::vector<float> out[3];
    std::thread threads[3];
    for (int i = 0; i < 3; ++i) {
        threads[i] = std::thread([&, i] {
            EngineFixture& fx = *engines[i];
            ClockworkCore* core = fx.engine().core();
            const size_t n = static_cast<size_t>(clockwork_block_size(core)) * 2;
            for (uint32_t b = 0; b < kScriptBlocks; ++b) {
                for (const Cue& c : scripts[i])
                    if (c.block == b) fx.send(c.message);
                fx.pumpBlock();
                const float* bus = clockwork_audio_out(core);
                out[i].insert(out[i].end(), bus, bus + n);
            }
        });
    }
    for (auto& t : threads) t.join();

    for (int i = 0; i < 3; ++i) {
        const long diff = firstDifference(out[i], solo[i]);
        INFO("engine " << i + 1 << " first differs from its solo run at sample " << diff);
        CHECK(diff == -1);
    }
}

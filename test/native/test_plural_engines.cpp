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

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>
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

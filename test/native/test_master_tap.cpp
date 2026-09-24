// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Sam Aaron
/*
 * test_master_tap.cpp — the master tap is clockwork's.
 *
 * The OUT tap of the arena's audio taps carries what left for the device
 * every block, written by clockwork from boot. This guest writes nothing
 * into it, holds nothing over it, and has no verb about it.
 */
#include "EngineFixture.h"
#include "OscTestUtils.h"
#include "clockwork_arena.h"
#include "clockwork_client.h"
#include "shm_audio_buffer.hpp"
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cmath>
#include <thread>
#include <vector>

namespace {
shm_audio_buffer* outTap(EngineFixture& fx) {
    ClockworkRegion taps {};
    REQUIRE(clockwork_client_region(fx.engine().egressClient(), CLOCKWORK_REGION_AUDIO_TAPS, &taps) == CLOCKWORK_OK);
    return static_cast<shm_audio_buffer*>(taps.base) + CLOCKWORK_TAP_OUT;
}
// Everything here is counted against the engine's own progress, never a sleep: a CI runner that stalls reads, by
// the wall clock, as a tap that stopped. The macOS arm64 runner did on 2026-09-24 — 1,408 frames in a "100 ms"
// sleep, its clock a second adrift — and failed a release. What the tap must do is carry every block the engine
// renders, and that holds however long the blocks took.
constexpr uint64_t kBlockFrames = 128;   // one rendered block (processCount ticks once per block, headless)

// The engine's rendered blocks and the tap's frames, read as one: the block count unchanged across the read, so
// the two stand at most the one block being rendered as they are read apart.
struct Progress { uint32_t blocks; uint64_t frames; };
Progress progress(EngineFixture& fx, shm_audio_buffer* slot) {
    auto& cb = fx.engine().audioCallback();
    for (;;) {
        const uint32_t before = cb.processCount.load(std::memory_order_acquire);
        const uint64_t frames = slot->write_position.load();
        if (cb.processCount.load(std::memory_order_acquire) == before) return {before, frames};
    }
}

// What the tap took while the engine rendered at least `blocks` more blocks, and how many it rendered.
Progress over(EngineFixture& fx, shm_audio_buffer* slot, uint32_t blocks) {
    const Progress a = progress(fx, slot);
    REQUIRE(fx.waitForBlocks(blocks, 10000));
    const Progress b = progress(fx, slot);
    return {b.blocks - a.blocks, b.frames - a.frames};
}

// Every block rendered is a block in the tap: no more, no fewer, give or take the one in flight at each end.
void carriesEveryBlock(const Progress& p) {
    INFO("rendered " << p.blocks << " blocks; the tap took " << p.frames << " frames");
    CHECK(p.frames + kBlockFrames >= p.blocks * kBlockFrames);
    CHECK(p.frames <= (p.blocks + 1) * kBlockFrames);
}
}

TEST_CASE("master tap: clockwork's OUT tap flows from boot and carries the guest's mix",
          "[tap][guest]") {
    EngineFixture fx;
    shm_audio_buffer* slot = outTap(fx);

    // Live from boot, at the engine's rate: every block it renders.
    CHECK(slot->enabled.load() == 1);
    CHECK(slot->sample_rate == 48000);
    CHECK(slot->channels >= 2);
    carriesEveryBlock(over(fx, slot, 112));   // 0.3 s of blocks

    // What plays is what the slot carries.
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));
    shm_audio_buffer_reader reader(slot);
    reader.seek_to_live();
    {
        osc_test::Builder b;
        b.begin("/s_new") << "sonic-pi-beep" << int32_t{5100} << int32_t{0} << int32_t{0}
                          << "note" << 69.0f << "amp" << 1.0f << "sustain" << 2.0f;
        fx.send(b.end());
    }
    REQUIRE(fx.waitForBlocks(75, 10000));   // 0.2 s of the note rendered, however long that took
    std::vector<float> buf(4096 * SHM_AUDIO_CHANNELS);
    float peak = 0.f;
    for (int i = 0; i < 8; ++i) {
        const uint32_t n = reader.pull(buf.data(), 4096, nullptr);
        for (uint32_t k = 0; k < n * slot->channels; ++k) peak = std::max(peak, std::fabs(buf[k]));
        if (!n) break;
    }
    CHECK(peak > 0.05f);
    fx.send(osc_test::message("/n_free", int32_t{5100}));

    // And it never stops.
    CHECK(slot->enabled.load() == 1);
    carriesEveryBlock(over(fx, slot, 38));    // 0.1 s of blocks
}

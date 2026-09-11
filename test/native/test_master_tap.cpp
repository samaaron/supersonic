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
uint64_t framesIn(shm_audio_buffer* slot, int ms) {
    const uint64_t a = slot->write_position.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    return slot->write_position.load() - a;
}
}

TEST_CASE("master tap: clockwork's OUT tap flows from boot and carries the guest's mix",
          "[tap][guest]") {
    EngineFixture fx;
    shm_audio_buffer* slot = outTap(fx);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Live from boot, at the engine's rate.
    CHECK(slot->enabled.load() == 1);
    CHECK(slot->sample_rate == 48000);
    CHECK(slot->channels >= 2);
    {
        const uint64_t written = framesIn(slot, 300);
        CHECK(written > 48000 * 0.15);
        CHECK(written < 48000 * 0.7);
    }

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
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
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
    CHECK(framesIn(slot, 100) > 48000 * 0.05);
}

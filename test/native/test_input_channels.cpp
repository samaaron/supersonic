/*
 * test_input_channels.cpp — Input channel enable/disable tests
 *
 * Validates that:
 *   - enableInputChannels triggers cold swap when channel count changes
 *   - enableInputChannels(0) disables inputs
 *   - enableInputChannels(-1) re-enables with boot value
 *   - Same channel count = no-op (hot swap)
 *   - Engine remains responsive after input channel changes
 *   - State transitions match cold swap pattern
 */
#include <catch2/catch_test_macros.hpp>
#include "EngineFixture.h"
#include "OscBuilder.h"
#include "OscTestUtils.h"

// ── Disable inputs (2 → 0) triggers cold swap ──────────────────────────────

TEST_CASE("InputChannels: disable inputs triggers cold swap", "[InputChannels]") {
    EngineFixture fix;

    std::vector<std::string> events;
    fix.engine().onSwapEvent = [&](const std::string& event, const SwapResult&) {
        events.push_back(event);
    };

    auto result = fix.engine().enableInputChannels(0);
    REQUIRE(result.success);
    REQUIRE(result.type == SwapType::Cold);

    REQUIRE(events.size() == 2);
    REQUIRE(events[0] == "swap:start");
    REQUIRE(events[1] == "swap:complete");

    // Engine should be responsive
    OscReply reply;
    fix.send(osc_test::message("/status"));
    REQUIRE(fix.waitForReply("/status.reply", reply));
}

// ── Enable inputs (0 → 2) triggers cold swap ───────────────────────────────

TEST_CASE("InputChannels: enable inputs triggers cold swap", "[InputChannels]") {
    // Boot with 0 inputs
    SupersonicEngine::Config cfg;
    cfg.sampleRate       = 48000;
    cfg.bufferSize       = 128;
    cfg.udpPort          = 0;
    cfg.numBuffers       = 1024;
    cfg.maxNodes         = 1024;
    cfg.maxGraphDefs     = 512;
    cfg.maxWireBufs      = 64;
    cfg.headless         = true;
    cfg.numInputChannels = 0;
    EngineFixture fix(cfg);

    std::vector<std::string> events;
    fix.engine().onSwapEvent = [&](const std::string& event, const SwapResult&) {
        events.push_back(event);
    };

    auto result = fix.engine().enableInputChannels(2);
    REQUIRE(result.success);
    REQUIRE(result.type == SwapType::Cold);

    REQUIRE(events.size() == 2);
    REQUIRE(events[0] == "swap:start");
    REQUIRE(events[1] == "swap:complete");

    OscReply reply;
    fix.send(osc_test::message("/status"));
    REQUIRE(fix.waitForReply("/status.reply", reply));
}

// ── Same channel count = no-op ──────────────────────────────────────────────

TEST_CASE("InputChannels: same count is no-op hot swap", "[InputChannels]") {
    EngineFixture fix;

    // Default config has numInputChannels = 2
    auto result = fix.engine().enableInputChannels(2);
    REQUIRE(result.success);
    REQUIRE(result.type == SwapType::Hot);
    REQUIRE(result.sampleRate == 48000);
    REQUIRE(result.bufferSize == 128);
}

// ── Re-enable with boot value (-1) ──────────────────────────────────────────

TEST_CASE("InputChannels: -1 re-enables with boot channel count", "[InputChannels]") {
    EngineFixture fix;

    // Disable inputs first
    auto r1 = fix.engine().enableInputChannels(0);
    REQUIRE(r1.success);
    REQUIRE(r1.type == SwapType::Cold);

    // Re-enable with -1 (should use boot value of 2)
    fix.clearReplies();
    auto r2 = fix.engine().enableInputChannels(-1);
    REQUIRE(r2.success);
    REQUIRE(r2.type == SwapType::Cold);

    // Engine should be responsive
    OscReply reply;
    fix.send(osc_test::message("/status"));
    REQUIRE(fix.waitForReply("/status.reply", reply));

    // Boot value should still be 2
    REQUIRE(fix.engine().configuredInputChannels() == 2);
}

// ── State transitions ───────────────────────────────────────────────────────

TEST_CASE("InputChannels: state transitions Running -> Restarting -> Running",
          "[InputChannels]") {
    EngineFixture fix;

    std::vector<EngineState> states;
    fix.engine().onSwapEvent = [&](const std::string&, const SwapResult&) {
        states.push_back(fix.engine().engineState());
    };

    REQUIRE(fix.engine().engineState() == EngineState::Running);

    auto result = fix.engine().enableInputChannels(0);
    REQUIRE(result.success);

    // At swap:start, state should have been Restarting
    REQUIRE(states.size() == 2);
    REQUIRE(states[0] == EngineState::Restarting);
    // At swap:complete, state is already Running
    REQUIRE(states[1] == EngineState::Running);

    REQUIRE(fix.engine().engineState() == EngineState::Running);
}

// ── Round-trip ──────────────────────────────────────────────────────────────

TEST_CASE("InputChannels: round-trip 2 -> 0 -> 2", "[InputChannels]") {
    EngineFixture fix;

    // Disable
    auto r1 = fix.engine().enableInputChannels(0);
    REQUIRE(r1.success);
    REQUIRE(r1.type == SwapType::Cold);

    OscReply reply;
    fix.send(osc_test::message("/status"));
    REQUIRE(fix.waitForReply("/status.reply", reply));
    fix.clearReplies();

    // Re-enable
    auto r2 = fix.engine().enableInputChannels(2);
    REQUIRE(r2.success);
    REQUIRE(r2.type == SwapType::Cold);

    fix.send(osc_test::message("/status"));
    REQUIRE(fix.waitForReply("/status.reply", reply));
}

// ── Engine processes OSC after input channel cold swap ───────────────────────

TEST_CASE("InputChannels: engine processes OSC after input change", "[InputChannels]") {
    EngineFixture fix;
    REQUIRE(fix.loadSynthDef("sonic-pi-beep"));

    // Cold swap: disable inputs
    auto result = fix.engine().enableInputChannels(0);
    REQUIRE(result.success);

    // Reload synthdef (world was rebuilt)
    fix.clearReplies();
    REQUIRE(fix.loadSynthDef("sonic-pi-beep"));

    // Recreate default group
    fix.send(osc_test::message("/g_new", 1, 0, 0));
    auto syncPkt = osc_test::message("/sync", 99);
    fix.engine().sendOSC(syncPkt.ptr(), syncPkt.size());
    OscReply reply;
    REQUIRE(fix.waitForReply("/synced", reply));
    fix.clearReplies();

    // Create and free a synth
    {
        osc_test::Builder b;
        auto& s = b.begin("/s_new");
        s << "sonic-pi-beep" << (int32_t)1000 << (int32_t)0 << (int32_t)1
          << "note" << 60.0f << "out_bus" << 0.0f;
        fix.send(b.end());
    }
    fix.send(osc_test::message("/n_free", 1000));

    fix.send(osc_test::message("/status"));
    REQUIRE(fix.waitForReply("/status.reply", reply));
}

// ── No-op does not fire swap events ─────────────────────────────────────────

TEST_CASE("InputChannels: no-op does not fire swap events", "[InputChannels]") {
    EngineFixture fix;

    bool eventFired = false;
    fix.engine().onSwapEvent = [&](const std::string&, const SwapResult&) {
        eventFired = true;
    };

    // Same count = no-op, should not trigger swap machinery
    auto result = fix.engine().enableInputChannels(2);
    REQUIRE(result.success);
    REQUIRE_FALSE(eventFired);
}

// ── Callback's cached World widths track cold-swap rebuilds ────────────────
// The IO callback clamps its render/input copies to the World bus widths it
// cached — if those don't follow a cold-swap rebuild, writes to buses beyond
// the boot-time width are silently dropped at the hardware boundary (e.g.
// boot on a 2-out device, swap to a 4-out device: Out.ar(2, sig) renders in
// the World but never reaches channel 3) and re-enabled inputs feed buses
// the World doesn't have. resume() re-syncs the widths from the live World;
// the input path is the one a headless fixture can rebuild at a new width.

TEST_CASE("InputChannels: callback World widths track cold-swap rebuilds",
          "[InputChannels]") {
    EngineFixture fix;
    auto& cb = fix.engine().audioCallback();

    // Boot: fixture world is 2-in / 2-out
    CHECK(cb.worldInputBusChannels() == 2);
    CHECK(cb.worldOutputBusChannels() == 2);

    // Narrow: 2 -> 0 inputs
    auto r0 = fix.engine().enableInputChannels(0);
    REQUIRE(r0.success);
    REQUIRE(r0.type == SwapType::Cold);
    CHECK(cb.worldInputBusChannels() == 0);
    CHECK(cb.worldOutputBusChannels() == 2);

    // Widen past the boot width: 0 -> 4 inputs
    auto r4 = fix.engine().enableInputChannels(4);
    REQUIRE(r4.success);
    REQUIRE(r4.type == SwapType::Cold);
    CHECK(cb.worldInputBusChannels() == 4);
    CHECK(cb.worldOutputBusChannels() == 2);
}

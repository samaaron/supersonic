/*
 * test_driver_switch.cpp — Driver switching with cold swap on rate mismatch
 *
 * Validates that:
 *   - switchDriver with same rate = hot swap
 *   - switchDriver with different rate = cold swap (world rebuilt)
 *   - Exactly one pair of swap events per switchDriver call (option 3 semantics)
 *   - Engine remains responsive after driver-switch cold swap
 *   - SwapResult reports correct type and sample rate
 */
#include <catch2/catch_test_macros.hpp>
#include "EngineFixture.h"
#include "OscBuilder.h"
#include "OscTestUtils.h"

// ── Same rate = hot swap ────────────────────────────────────────────────────

TEST_CASE("DriverSwitch: same rate produces hot swap", "[DriverSwitch]") {
    EngineFixture fix;

    std::vector<std::string> events;
    fix.engine().onSwapEvent = [&](const std::string& event, const SwapResult&) {
        events.push_back(event);
    };

    // Hook returns current rate (48000) — no rate mismatch
    fix.engine().testDriverSwitchRate = []() -> double { return 48000; };

    auto result = fix.engine().switchDriver("TestDriver");
    REQUIRE(result.success);
    REQUIRE(result.type == SwapType::Hot);
    REQUIRE(result.sampleRate == 48000);

    // Single logical swap — exactly one start/complete pair
    REQUIRE(events.size() == 2);
    REQUIRE(events[0] == "swap:start");
    REQUIRE(events[1] == "swap:complete");

    // Engine still responds
    OscReply reply;
    fix.send(osc_test::message("/status"));
    REQUIRE(fix.waitForReply("/status.reply", reply));
}

// ── Different rate = cold swap ──────────────────────────────────────────────

TEST_CASE("DriverSwitch: rate mismatch triggers cold swap", "[DriverSwitch]") {
    EngineFixture fix;

    std::vector<std::string> events;
    fix.engine().onSwapEvent = [&](const std::string& event, const SwapResult&) {
        events.push_back(event);
    };

    // Hook returns different rate — should trigger cold swap
    fix.engine().testDriverSwitchRate = []() -> double { return 44100; };

    auto result = fix.engine().switchDriver("TestDriver");
    REQUIRE(result.success);
    REQUIRE(result.type == SwapType::Cold);
    REQUIRE(result.sampleRate == 44100);

    // Single logical swap — exactly one start/complete pair, not two
    REQUIRE(events.size() == 2);
    REQUIRE(events[0] == "swap:start");
    REQUIRE(events[1] == "swap:complete");
}

// ── Engine responsive after cold swap ───────────────────────────────────────

TEST_CASE("DriverSwitch: engine responsive after rate-change cold swap", "[DriverSwitch]") {
    EngineFixture fix;

    fix.engine().testDriverSwitchRate = []() -> double { return 44100; };

    auto result = fix.engine().switchDriver("TestDriver");
    REQUIRE(result.success);

    // Engine should respond to OSC at new rate
    OscReply reply;
    fix.send(osc_test::message("/status"));
    REQUIRE(fix.waitForReply("/status.reply", reply));
}

// ── Multiple driver switches with alternating rates ─────────────────────────

TEST_CASE("DriverSwitch: multiple rate changes don't corrupt engine", "[DriverSwitch]") {
    EngineFixture fix;

    double rates[] = { 44100, 48000, 44100, 48000 };
    for (double rate : rates) {
        fix.engine().testDriverSwitchRate = [rate]() -> double { return rate; };

        auto result = fix.engine().switchDriver("TestDriver");
        REQUIRE(result.success);
        REQUIRE(result.sampleRate == rate);

        OscReply reply;
        fix.clearReplies();
        fix.send(osc_test::message("/status"));
        REQUIRE(fix.waitForReply("/status.reply", reply));
    }
}

// ── State transitions on cold swap ──────────────────────────────────────────

TEST_CASE("DriverSwitch: cold swap transitions through Restarting state", "[DriverSwitch]") {
    EngineFixture fix;

    std::vector<EngineState> states;
    fix.engine().onSwapEvent = [&](const std::string&, const SwapResult&) {
        states.push_back(fix.engine().engineState());
    };

    REQUIRE(fix.engine().engineState() == EngineState::Running);

    fix.engine().testDriverSwitchRate = []() -> double { return 44100; };
    auto result = fix.engine().switchDriver("TestDriver");
    REQUIRE(result.success);

    REQUIRE(states.size() == 2);
    REQUIRE(states[0] == EngineState::Restarting);
    REQUIRE(states[1] == EngineState::Running);
}

// ── Hot swap does not transition to Restarting ──────────────────────────────

TEST_CASE("DriverSwitch: hot swap stays Running throughout", "[DriverSwitch]") {
    EngineFixture fix;

    std::vector<EngineState> states;
    fix.engine().onSwapEvent = [&](const std::string&, const SwapResult&) {
        states.push_back(fix.engine().engineState());
    };

    fix.engine().testDriverSwitchRate = []() -> double { return 48000; };
    auto result = fix.engine().switchDriver("TestDriver");
    REQUIRE(result.success);

    for (auto s : states) {
        REQUIRE(s == EngineState::Running);
    }
}

// ── No hook in headless = error ─────────────────────────────────────────────

TEST_CASE("DriverSwitch: headless without hook returns error", "[DriverSwitch]") {
    EngineFixture fix;

    // No testDriverSwitchRate set — headless has no real driver to switch
    auto result = fix.engine().switchDriver("TestDriver");
    REQUIRE_FALSE(result.success);
    REQUIRE(result.error == "no audio device in headless mode");
}

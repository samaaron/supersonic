/*
 * test_cold_swap_recovery.cpp — Tests for rebuild_world failure recovery.
 *
 * When rebuild_world() throws during a cold swap, the engine should fall
 * back to safe defaults (original rate, buffer 128) and remain functional.
 */
#include <catch2/catch_test_macros.hpp>
#include "EngineFixture.h"
#include "OscBuilder.h"

TEST_CASE("ColdSwapRecovery: rebuild failure recovers at safe defaults",
          "[ColdSwapRecovery]") {
    EngineFixture fix;

    fix.engine().testRebuildFailure = []() -> std::string {
        return "simulated rebuild crash";
    };

    // Cold swap triggers rebuild_world which throws — engine should recover
    auto result = fix.engine().switchDevice("", 44100);
    REQUIRE(result.success);
    CHECK(result.error.find("recovered") != std::string::npos);
    // Should have fallen back to original rate (48000)
    CHECK(static_cast<int>(result.sampleRate) == 48000);
    CHECK(result.bufferSize == 128);
    CHECK(fix.engine().engineState() == EngineState::Running);
}

TEST_CASE("ColdSwapRecovery: engine responds to OSC after rebuild recovery",
          "[ColdSwapRecovery]") {
    EngineFixture fix;

    fix.engine().testRebuildFailure = []() -> std::string {
        return "simulated rebuild crash";
    };

    auto result = fix.engine().switchDevice("", 44100);
    REQUIRE(result.success);

    // Engine should be alive and responsive
    fix.clearReplies();
    OscReply reply;
    fix.send(osc_test::message("/status"));
    REQUIRE(fix.waitForReply("/status.reply", reply));
    CHECK(reply.parsed().argCount() >= 5);
}

TEST_CASE("ColdSwapRecovery: swap:recovered event fires",
          "[ColdSwapRecovery]") {
    EngineFixture fix;
    std::vector<std::string> events;

    fix.engine().onSwapEvent = [&](const std::string& event, const SwapResult&) {
        events.push_back(event);
    };

    fix.engine().testRebuildFailure = []() -> std::string {
        return "simulated rebuild crash";
    };

    auto result = fix.engine().switchDevice("", 44100);
    REQUIRE(result.success);

    REQUIRE(events.size() == 2);
    CHECK(events[0] == "swap:start");
    CHECK(events[1] == "swap:recovered");
}

TEST_CASE("ColdSwapRecovery: normal swap works after recovery",
          "[ColdSwapRecovery]") {
    EngineFixture fix;

    // First: trigger recovery
    fix.engine().testRebuildFailure = []() -> std::string {
        return "simulated rebuild crash";
    };

    auto r1 = fix.engine().switchDevice("", 44100);
    REQUIRE(r1.success);

    // Clear hook
    fix.engine().testRebuildFailure = nullptr;

    // Second: normal cold swap should work
    auto r2 = fix.engine().switchDevice("", 44100);
    REQUIRE(r2.success);
    REQUIRE(r2.type == SwapType::Cold);
    CHECK(r2.error.empty());
    CHECK(static_cast<int>(r2.sampleRate) == 44100);

    OscReply reply;
    fix.send(osc_test::message("/status"));
    REQUIRE(fix.waitForReply("/status.reply", reply));
}

TEST_CASE("ColdSwapRecovery: state transitions through recovery",
          "[ColdSwapRecovery]") {
    EngineFixture fix;
    std::vector<std::pair<std::string, EngineState>> trace;

    fix.engine().onSwapEvent = [&](const std::string& event, const SwapResult&) {
        trace.push_back({event, fix.engine().engineState()});
    };

    fix.engine().testRebuildFailure = []() -> std::string {
        return "simulated rebuild crash";
    };

    auto result = fix.engine().switchDevice("", 44100);
    REQUIRE(result.success);

    REQUIRE(trace.size() == 2);
    // At swap:start, state should be Restarting
    CHECK(trace[0].first == "swap:start");
    CHECK(trace[0].second == EngineState::Restarting);
    // At swap:recovered, state should be Running (set before event fires)
    CHECK(trace[1].first == "swap:recovered");
    CHECK(trace[1].second == EngineState::Running);
}

/*
 * test_input_fallback.cpp — Output-only fallback when an input open fails.
 *
 * Real-world motivation: on Windows, picking a microphone whose privacy
 * permission is denied causes JUCE/WASAPI to return "Couldn't open the
 * input device!" for the whole device-setup call. Rolling the swap back
 * means a rate change brings down the engine's output too, even though
 * the output would have opened fine on its own. Each rollback is a cold
 * swap, which fires /supersonic/setup, which triggers a Spider reinit,
 * and a burst of these (e.g. a user fiddling with prefs) cascades into
 * a reinit-during-reinit race.
 *
 * The fix: when the device-setup error mentions "input device" and an
 * input was requested, retry with the input cleared. Output keeps
 * working and the result tells the client which input was unavailable
 * and why, so the GUI can surface a single actionable error instead of
 * a storm of cold-swap rollbacks.
 */
#include <catch2/catch_test_macros.hpp>
#include "EngineFixture.h"

namespace {

// Drives testSwapFailure to simulate "input fails, output-only succeeds":
// returns the JUCE-style input-failure string when input is requested,
// and empty (success) when it isn't. Counts attempts so tests can assert
// that the retry actually happened.
struct InputFailureSimulator {
    int totalAttempts = 0;
    int attemptsWithInput = 0;
    int attemptsWithoutInput = 0;
    std::string inputErrorMessage = "Couldn't open the input device!";

    std::function<std::string(bool)> hook() {
        return [this](bool inputRequested) -> std::string {
            ++totalAttempts;
            if (inputRequested) {
                ++attemptsWithInput;
                return inputErrorMessage;
            }
            ++attemptsWithoutInput;
            return "";
        };
    }
};

} // namespace

TEST_CASE("InputFallback: input-open failure retries with output only",
          "[InputFallback]") {
    EngineFixture fix;
    InputFailureSimulator sim;
    fix.engine().testSwapFailure = sim.hook();

    auto result = fix.engine().switchDevice(
        "OutputDevice", 44100, 0, /*forceCold=*/false, "InputDevice");

    // The whole swap should have succeeded (output came up).
    REQUIRE(result.success);
    REQUIRE(result.error.empty());

    // The engine retried with input cleared.
    REQUIRE(sim.attemptsWithInput  == 1);
    REQUIRE(sim.attemptsWithoutInput == 1);
    REQUIRE(sim.totalAttempts == 2);
}

TEST_CASE("InputFallback: result flags input as unavailable with reason",
          "[InputFallback]") {
    EngineFixture fix;
    InputFailureSimulator sim;
    fix.engine().testSwapFailure = sim.hook();

    auto result = fix.engine().switchDevice(
        "OutputDevice", 44100, 0, /*forceCold=*/false, "InputDevice");

    REQUIRE(result.success);
    CHECK(result.inputUnavailable);
    CHECK(result.inputUnavailableReason == "Couldn't open the input device!");
}

TEST_CASE("InputFallback: failure unrelated to input is not retried",
          "[InputFallback]") {
    EngineFixture fix;
    int totalAttempts = 0;
    fix.engine().testSwapFailure = [&](bool) -> std::string {
        ++totalAttempts;
        return "device sample rate not supported";
    };

    auto result = fix.engine().switchDevice(
        "OutputDevice", 44100, 0, /*forceCold=*/false, "InputDevice");

    // Non-input failure → swap fails, rollback (no retry).
    REQUIRE_FALSE(result.success);
    REQUIRE(totalAttempts == 1);
    CHECK_FALSE(result.inputUnavailable);
}

TEST_CASE("InputFallback: failure with no input requested is not retried",
          "[InputFallback]") {
    EngineFixture fix;
    int totalAttempts = 0;
    fix.engine().testSwapFailure = [&](bool inputRequested) -> std::string {
        ++totalAttempts;
        // Pretend even output-only fails. No input was asked for, so no
        // retry should happen — clearing input from a setup that already
        // had none would be a pointless second attempt.
        REQUIRE_FALSE(inputRequested);
        return "Couldn't open the input device!";
    };

    auto result = fix.engine().switchDevice("OutputDevice", 44100);

    REQUIRE_FALSE(result.success);
    REQUIRE(totalAttempts == 1);
}

TEST_CASE("InputFallback: swap:complete event still fires on output-only fallback",
          "[InputFallback]") {
    EngineFixture fix;
    InputFailureSimulator sim;
    fix.engine().testSwapFailure = sim.hook();

    std::vector<std::string> events;
    fix.engine().onSwapEvent = [&](const std::string& event, const SwapResult&) {
        events.push_back(event);
    };

    auto result = fix.engine().switchDevice(
        "OutputDevice", 44100, 0, /*forceCold=*/false, "InputDevice");

    REQUIRE(result.success);
    // Should look like a regular successful swap, not a failed one.
    REQUIRE(events.size() == 2);
    CHECK(events[0] == "swap:start");
    CHECK(events[1] == "swap:complete");
}

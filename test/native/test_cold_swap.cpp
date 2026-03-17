/*
 * test_cold_swap.cpp — Cold swap (destroy world + rebuild) tests
 *
 * Exercises switchDevice() in headless mode, covering:
 *   - Successful cold swap (rate change)
 *   - Failed cold swap (rollback)
 *   - State machine transitions
 *   - Event emission
 *   - Hot swap (same rate)
 *   - Concurrent rejection
 *   - Round-trip cold swaps
 */
#include <catch2/catch_test_macros.hpp>
#include "EngineFixture.h"
#include "OscBuilder.h"
#include <thread>
#include <chrono>

// ── Successful cold swap ─────────────────────────────────────────────────────

TEST_CASE("ColdSwap: successful rate change 48000 -> 44100", "[ColdSwap]") {
    EngineFixture fix;

    // Verify engine is alive at 48000
    OscReply reply;
    fix.send(osc_test::message("/status"));
    REQUIRE(fix.waitForReply("/status.reply", reply));

    // Track swap events
    std::vector<std::string> events;
    fix.engine().onSwapEvent = [&](const std::string& event, const SwapResult&) {
        events.push_back(event);
    };

    // Cold swap to 44100
    auto result = fix.engine().switchDevice("", 44100);
    REQUIRE(result.success);
    REQUIRE(result.type == SwapType::Cold);
    REQUIRE(static_cast<int>(result.sampleRate) == 44100);

    // Engine state should be Running after swap
    REQUIRE(fix.engine().engineState() == EngineState::Running);

    // Engine should respond to OSC at new rate
    fix.clearReplies();
    fix.send(osc_test::message("/status"));
    REQUIRE(fix.waitForReply("/status.reply", reply));

    // Verify events
    REQUIRE(events.size() == 2);
    REQUIRE(events[0] == "swap:start");
    REQUIRE(events[1] == "swap:complete");
}

// ── Failed cold swap (rollback) ──────────────────────────────────────────────

TEST_CASE("ColdSwap: failed swap rolls back to original rate", "[ColdSwap]") {
    EngineFixture fix;

    // Verify engine is alive
    OscReply reply;
    fix.send(osc_test::message("/status"));
    REQUIRE(fix.waitForReply("/status.reply", reply));

    // Track swap events
    std::vector<std::string> events;
    fix.engine().onSwapEvent = [&](const std::string& event, const SwapResult&) {
        events.push_back(event);
    };

    // Inject failure
    fix.engine().testSwapFailure = []() -> std::string {
        return "simulated device error";
    };

    // Attempt cold swap — should fail and rollback
    auto result = fix.engine().switchDevice("", 44100);
    REQUIRE_FALSE(result.success);
    REQUIRE(result.error == "simulated device error");

    // Engine state should be Running (rolled back)
    REQUIRE(fix.engine().engineState() == EngineState::Running);

    // Engine should still respond to OSC (alive at original rate)
    fix.clearReplies();
    fix.send(osc_test::message("/status"));
    REQUIRE(fix.waitForReply("/status.reply", reply));

    // Verify events: start then failed
    REQUIRE(events.size() == 2);
    REQUIRE(events[0] == "swap:start");
    REQUIRE(events[1] == "swap:failed");
}

// ── State transitions ────────────────────────────────────────────────────────

TEST_CASE("ColdSwap: state transitions Running -> Restarting -> Running on success",
          "[ColdSwap]") {
    EngineFixture fix;

    std::vector<EngineState> states;
    fix.engine().onSwapEvent = [&](const std::string&, const SwapResult&) {
        states.push_back(fix.engine().engineState());
    };

    REQUIRE(fix.engine().engineState() == EngineState::Running);

    auto result = fix.engine().switchDevice("", 44100);
    REQUIRE(result.success);

    // At swap:start, state should have been Restarting
    REQUIRE(states.size() == 2);
    REQUIRE(states[0] == EngineState::Restarting);
    // At swap:complete, state is already Running (set before the event fires)
    REQUIRE(states[1] == EngineState::Running);

    // Final state should be Running
    REQUIRE(fix.engine().engineState() == EngineState::Running);
}

TEST_CASE("ColdSwap: state transitions Running -> Restarting -> Running on failure",
          "[ColdSwap]") {
    EngineFixture fix;

    std::vector<EngineState> states;
    fix.engine().onSwapEvent = [&](const std::string&, const SwapResult&) {
        states.push_back(fix.engine().engineState());
    };

    fix.engine().testSwapFailure = []() -> std::string {
        return "test failure";
    };

    REQUIRE(fix.engine().engineState() == EngineState::Running);

    auto result = fix.engine().switchDevice("", 44100);
    REQUIRE_FALSE(result.success);

    // At swap:start, state should be Restarting
    REQUIRE(states.size() == 2);
    REQUIRE(states[0] == EngineState::Restarting);
    // At swap:failed, state is already Running (rollback set before the event fires)
    REQUIRE(states[1] == EngineState::Running);

    // Final state should be Running (rolled back)
    REQUIRE(fix.engine().engineState() == EngineState::Running);
}

// ── Hot swap (same rate) ─────────────────────────────────────────────────────

TEST_CASE("ColdSwap: same rate triggers hot swap", "[ColdSwap]") {
    EngineFixture fix;

    std::vector<std::string> events;
    fix.engine().onSwapEvent = [&](const std::string& event, const SwapResult&) {
        events.push_back(event);
    };

    // Switch to same rate (48000) — should be hot swap
    auto result = fix.engine().switchDevice("", 48000);
    REQUIRE(result.success);
    REQUIRE(result.type == SwapType::Hot);

    // Engine should still work
    OscReply reply;
    fix.send(osc_test::message("/status"));
    REQUIRE(fix.waitForReply("/status.reply", reply));

    REQUIRE(events.size() == 2);
    REQUIRE(events[0] == "swap:start");
    REQUIRE(events[1] == "swap:complete");
}

TEST_CASE("ColdSwap: no rate specified triggers hot swap", "[ColdSwap]") {
    EngineFixture fix;

    // No rate specified (0) = keep current rate = hot swap
    auto result = fix.engine().switchDevice("");
    REQUIRE(result.success);
    REQUIRE(result.type == SwapType::Hot);

    OscReply reply;
    fix.send(osc_test::message("/status"));
    REQUIRE(fix.waitForReply("/status.reply", reply));
}

// ── Concurrent rejection ─────────────────────────────────────────────────────

TEST_CASE("ColdSwap: concurrent swap is rejected", "[ColdSwap]") {
    EngineFixture fix;

    // Use a slow failure hook to hold the swap mutex
    fix.engine().testSwapFailure = []() -> std::string {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        return "slow failure";
    };

    // Launch first swap in a background thread
    SwapResult firstResult;
    std::thread t1([&]() {
        firstResult = fix.engine().switchDevice("", 44100);
    });

    // Give the first swap time to acquire the mutex
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Second swap should be rejected immediately
    auto secondResult = fix.engine().switchDevice("", 22050);
    REQUIRE_FALSE(secondResult.success);
    REQUIRE(secondResult.error == "swap already in progress");

    t1.join();

    // First swap should have completed (with failure from the hook)
    REQUIRE_FALSE(firstResult.success);
    REQUIRE(firstResult.error == "slow failure");

    // Engine should still be running after all that
    OscReply reply;
    fix.clearReplies();
    fix.send(osc_test::message("/status"));
    REQUIRE(fix.waitForReply("/status.reply", reply));
}

// ── State cache is called during cold swap ───────────────────────────────────

TEST_CASE("ColdSwap: captureAll called before destroy on cold swap", "[ColdSwap]") {
    EngineFixture fix;

    // Load a synthdef so the cache has something
    REQUIRE(fix.loadSynthDef("sonic-pi-beep"));
    REQUIRE(fix.engine().stateCache().synthDefs().count("sonic-pi-beep") == 1);

    int captureCount = 0;
    fix.engine().stateCache().registerModule({
        "test-capture-counter",
        [&]() { captureCount++; },
        [&]() { /* restore */ }
    });

    auto result = fix.engine().switchDevice("", 44100);
    REQUIRE(result.success);
    REQUIRE(result.type == SwapType::Cold);

    // captureAll should have been called exactly once
    REQUIRE(captureCount == 1);
}

TEST_CASE("ColdSwap: captureAll called on failed cold swap too", "[ColdSwap]") {
    EngineFixture fix;

    int captureCount = 0;
    fix.engine().stateCache().registerModule({
        "test-capture-counter",
        [&]() { captureCount++; },
        [&]() { /* restore */ }
    });

    fix.engine().testSwapFailure = []() -> std::string {
        return "test failure";
    };

    auto result = fix.engine().switchDevice("", 44100);
    REQUIRE_FALSE(result.success);

    // captureAll should still have been called (before the failure)
    REQUIRE(captureCount == 1);
}

// ── Round-trip cold swaps ────────────────────────────────────────────────────

TEST_CASE("ColdSwap: round-trip 48000 -> 44100 -> 48000", "[ColdSwap]") {
    EngineFixture fix;

    // Swap to 44100
    auto r1 = fix.engine().switchDevice("", 44100);
    REQUIRE(r1.success);
    REQUIRE(r1.type == SwapType::Cold);
    REQUIRE(static_cast<int>(r1.sampleRate) == 44100);

    // Verify engine works
    OscReply reply;
    fix.send(osc_test::message("/status"));
    REQUIRE(fix.waitForReply("/status.reply", reply));
    fix.clearReplies();

    // Swap back to 48000
    auto r2 = fix.engine().switchDevice("", 48000);
    REQUIRE(r2.success);
    REQUIRE(r2.type == SwapType::Cold);
    REQUIRE(static_cast<int>(r2.sampleRate) == 48000);

    // Verify engine still works
    fix.send(osc_test::message("/status"));
    REQUIRE(fix.waitForReply("/status.reply", reply));
}

TEST_CASE("ColdSwap: engine processes OSC after cold swap", "[ColdSwap]") {
    EngineFixture fix;

    // Load synthdef before swap
    REQUIRE(fix.loadSynthDef("sonic-pi-beep"));

    // Cold swap
    auto result = fix.engine().switchDevice("", 44100);
    REQUIRE(result.success);

    // Reload synthdef at new rate (cold swap destroys world)
    fix.clearReplies();
    REQUIRE(fix.loadSynthDef("sonic-pi-beep"));

    // Create default group again (world was rebuilt)
    fix.send(osc_test::message("/g_new", 1, 0, 0));
    OscReply reply;
    auto syncPkt = osc_test::message("/sync", 99);
    fix.engine().sendOsc(syncPkt.ptr(), syncPkt.size());
    REQUIRE(fix.waitForReply("/synced", reply));
    fix.clearReplies();

    // Create and free a synth at the new rate
    {
        osc_test::Builder b;
        auto& s = b.begin("/s_new");
        s << "sonic-pi-beep" << (int32_t)1000 << (int32_t)0 << (int32_t)1
          << "note" << 60.0f << "out_bus" << 0.0f;
        fix.send(b.end());
    }
    fix.send(osc_test::message("/n_free", 1000));

    // Verify engine is still processing
    fix.send(osc_test::message("/status"));
    REQUIRE(fix.waitForReply("/status.reply", reply));
}

// ── Hot swap does NOT trigger state transitions ──────────────────────────────

TEST_CASE("ColdSwap: hot swap does not transition to Restarting", "[ColdSwap]") {
    EngineFixture fix;

    std::vector<EngineState> states;
    fix.engine().onSwapEvent = [&](const std::string&, const SwapResult&) {
        states.push_back(fix.engine().engineState());
    };

    auto result = fix.engine().switchDevice("", 48000);  // same rate = hot
    REQUIRE(result.success);
    REQUIRE(result.type == SwapType::Hot);

    // State should have been Running throughout (hot swap doesn't go to Restarting)
    for (auto s : states) {
        REQUIRE(s == EngineState::Running);
    }
}

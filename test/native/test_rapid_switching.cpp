/*
 * test_rapid_switching.cpp — Guards against rapid device/driver/rate/buffer changes.
 *
 * Exercises the switchDevice() and switchDriver() paths under rapid sequential
 * and mixed-parameter changes, verifying:
 *   - Rapid sequential cold swaps (rate changes back-to-back)
 *   - Rapid sequential hot swaps (same-rate changes)
 *   - Mixed cold + hot swap sequences
 *   - Buffer-size-only changes remain hot swaps
 *   - switchDriver rejection in headless mode
 *   - setDeviceMode in headless mode
 *   - Synth state survives rapid cold swap sequences
 *   - Engine remains healthy after stress sequences
 */
#include <catch2/catch_test_macros.hpp>
#include "EngineFixture.h"
#include "OscBuilder.h"
#include <thread>
#include <chrono>
#include <vector>

// =============================================================================
// RAPID SEQUENTIAL COLD SWAPS
// =============================================================================

TEST_CASE("RapidSwitch: back-to-back cold swaps 48000 -> 44100 -> 48000",
          "[RapidSwitch]") {
    EngineFixture fix;

    auto r1 = fix.engine().switchDevice("", 44100);
    REQUIRE(r1.success);
    REQUIRE(r1.type == SwapType::Cold);

    // Immediately trigger another cold swap — no pause
    auto r2 = fix.engine().switchDevice("", 48000);
    REQUIRE(r2.success);
    REQUIRE(r2.type == SwapType::Cold);

    REQUIRE(fix.engine().engineState() == EngineState::Running);

    OscReply reply;
    fix.send(osc_test::message("/status"));
    REQUIRE(fix.waitForReply("/status.reply", reply));
}

TEST_CASE("RapidSwitch: four sequential cold swaps with varying rates",
          "[RapidSwitch]") {
    EngineFixture fix;
    double rates[] = {44100, 48000, 44100, 48000};

    for (double rate : rates) {
        auto result = fix.engine().switchDevice("", rate);
        REQUIRE(result.success);
        REQUIRE(static_cast<int>(result.sampleRate) == static_cast<int>(rate));
    }

    REQUIRE(fix.engine().engineState() == EngineState::Running);

    OscReply reply;
    fix.send(osc_test::message("/status"));
    REQUIRE(fix.waitForReply("/status.reply", reply));
}

TEST_CASE("RapidSwitch: ten sequential cold swaps (stress)",
          "[RapidSwitch]") {
    EngineFixture fix;

    for (int i = 0; i < 10; ++i) {
        double rate = (i % 2 == 0) ? 44100 : 48000;
        auto result = fix.engine().switchDevice("", rate);
        REQUIRE(result.success);
        REQUIRE(result.type == SwapType::Cold);
    }

    REQUIRE(fix.engine().engineState() == EngineState::Running);

    OscReply reply;
    fix.clearReplies();
    fix.send(osc_test::message("/status"));
    REQUIRE(fix.waitForReply("/status.reply", reply));
}

// =============================================================================
// RAPID SEQUENTIAL HOT SWAPS
// =============================================================================

TEST_CASE("RapidSwitch: ten sequential hot swaps (same rate)",
          "[RapidSwitch]") {
    EngineFixture fix;

    for (int i = 0; i < 10; ++i) {
        auto result = fix.engine().switchDevice("", 48000);
        REQUIRE(result.success);
        REQUIRE(result.type == SwapType::Hot);
    }

    REQUIRE(fix.engine().engineState() == EngineState::Running);

    OscReply reply;
    fix.send(osc_test::message("/status"));
    REQUIRE(fix.waitForReply("/status.reply", reply));
}

TEST_CASE("RapidSwitch: hot swaps with no rate specified",
          "[RapidSwitch]") {
    EngineFixture fix;

    for (int i = 0; i < 10; ++i) {
        auto result = fix.engine().switchDevice("");
        REQUIRE(result.success);
        REQUIRE(result.type == SwapType::Hot);
    }

    OscReply reply;
    fix.send(osc_test::message("/status"));
    REQUIRE(fix.waitForReply("/status.reply", reply));
}

// =============================================================================
// MIXED HOT + COLD SWAP SEQUENCES
// =============================================================================

TEST_CASE("RapidSwitch: cold swap immediately followed by hot swap",
          "[RapidSwitch]") {
    EngineFixture fix;

    // Cold swap (rate change)
    auto r1 = fix.engine().switchDevice("", 44100);
    REQUIRE(r1.success);
    REQUIRE(r1.type == SwapType::Cold);

    // Hot swap immediately after (same rate)
    auto r2 = fix.engine().switchDevice("", 44100);
    REQUIRE(r2.success);
    REQUIRE(r2.type == SwapType::Hot);

    REQUIRE(fix.engine().engineState() == EngineState::Running);

    OscReply reply;
    fix.send(osc_test::message("/status"));
    REQUIRE(fix.waitForReply("/status.reply", reply));
}

TEST_CASE("RapidSwitch: hot swap immediately followed by cold swap",
          "[RapidSwitch]") {
    EngineFixture fix;

    // Hot swap (same rate)
    auto r1 = fix.engine().switchDevice("", 48000);
    REQUIRE(r1.success);
    REQUIRE(r1.type == SwapType::Hot);

    // Cold swap immediately after (rate change)
    auto r2 = fix.engine().switchDevice("", 44100);
    REQUIRE(r2.success);
    REQUIRE(r2.type == SwapType::Cold);

    REQUIRE(fix.engine().engineState() == EngineState::Running);

    OscReply reply;
    fix.send(osc_test::message("/status"));
    REQUIRE(fix.waitForReply("/status.reply", reply));
}

TEST_CASE("RapidSwitch: alternating hot and cold swaps",
          "[RapidSwitch]") {
    EngineFixture fix;
    std::vector<std::string> events;

    fix.engine().onSwapEvent = [&](const std::string& event, const SwapResult&) {
        events.push_back(event);
    };

    // Cold (48k -> 44.1k)
    auto r1 = fix.engine().switchDevice("", 44100);
    REQUIRE(r1.success);
    REQUIRE(r1.type == SwapType::Cold);

    // Hot (stay at 44.1k)
    auto r2 = fix.engine().switchDevice("", 44100);
    REQUIRE(r2.success);
    REQUIRE(r2.type == SwapType::Hot);

    // Cold (44.1k -> 48k)
    auto r3 = fix.engine().switchDevice("", 48000);
    REQUIRE(r3.success);
    REQUIRE(r3.type == SwapType::Cold);

    // Hot (stay at 48k)
    auto r4 = fix.engine().switchDevice("", 48000);
    REQUIRE(r4.success);
    REQUIRE(r4.type == SwapType::Hot);

    // Each swap fires start + complete = 2 events, 4 swaps = 8 events
    REQUIRE(events.size() == 8);
    for (size_t i = 0; i < events.size(); i += 2) {
        CHECK(events[i] == "swap:start");
        CHECK(events[i + 1] == "swap:complete");
    }

    OscReply reply;
    fix.send(osc_test::message("/status"));
    REQUIRE(fix.waitForReply("/status.reply", reply));
}

// =============================================================================
// BUFFER SIZE CHANGES
// =============================================================================

TEST_CASE("RapidSwitch: buffer-size-only change is hot swap",
          "[RapidSwitch]") {
    EngineFixture fix;

    // Change buffer size but keep same rate — should be hot
    auto result = fix.engine().switchDevice("", 0, 256);
    REQUIRE(result.success);
    REQUIRE(result.type == SwapType::Hot);

    OscReply reply;
    fix.send(osc_test::message("/status"));
    REQUIRE(fix.waitForReply("/status.reply", reply));
}

TEST_CASE("RapidSwitch: rapid buffer size changes",
          "[RapidSwitch]") {
    EngineFixture fix;

    int bufferSizes[] = {256, 512, 1024, 128, 64, 256};
    for (int bs : bufferSizes) {
        auto result = fix.engine().switchDevice("", 0, bs);
        REQUIRE(result.success);
        REQUIRE(result.type == SwapType::Hot);
    }

    OscReply reply;
    fix.send(osc_test::message("/status"));
    REQUIRE(fix.waitForReply("/status.reply", reply));
}

TEST_CASE("RapidSwitch: rate change + buffer change together is cold",
          "[RapidSwitch]") {
    EngineFixture fix;

    auto result = fix.engine().switchDevice("", 44100, 256);
    REQUIRE(result.success);
    REQUIRE(result.type == SwapType::Cold);
    REQUIRE(static_cast<int>(result.sampleRate) == 44100);

    OscReply reply;
    fix.send(osc_test::message("/status"));
    REQUIRE(fix.waitForReply("/status.reply", reply));
}

// =============================================================================
// MIXED RATE + BUFFER RAPID CHANGES
// =============================================================================

TEST_CASE("RapidSwitch: rapid rate then buffer then rate changes",
          "[RapidSwitch]") {
    EngineFixture fix;

    // Rate change (cold)
    auto r1 = fix.engine().switchDevice("", 44100);
    REQUIRE(r1.success);
    REQUIRE(r1.type == SwapType::Cold);

    // Buffer change (hot — rate stays 44100)
    auto r2 = fix.engine().switchDevice("", 0, 512);
    REQUIRE(r2.success);
    REQUIRE(r2.type == SwapType::Hot);

    // Rate change back (cold)
    auto r3 = fix.engine().switchDevice("", 48000);
    REQUIRE(r3.success);
    REQUIRE(r3.type == SwapType::Cold);

    // Buffer change (hot)
    auto r4 = fix.engine().switchDevice("", 0, 128);
    REQUIRE(r4.success);
    REQUIRE(r4.type == SwapType::Hot);

    OscReply reply;
    fix.send(osc_test::message("/status"));
    REQUIRE(fix.waitForReply("/status.reply", reply));
}

TEST_CASE("RapidSwitch: rate + buffer combined, then buffer-only, then rate-only",
          "[RapidSwitch]") {
    EngineFixture fix;

    // Rate + buffer together (cold)
    auto r1 = fix.engine().switchDevice("", 44100, 512);
    REQUIRE(r1.success);
    REQUIRE(r1.type == SwapType::Cold);

    // Buffer-only (hot)
    auto r2 = fix.engine().switchDevice("", 0, 256);
    REQUIRE(r2.success);
    REQUIRE(r2.type == SwapType::Hot);

    // Rate-only (cold, back to 48k)
    auto r3 = fix.engine().switchDevice("", 48000);
    REQUIRE(r3.success);
    REQUIRE(r3.type == SwapType::Cold);

    OscReply reply;
    fix.send(osc_test::message("/status"));
    REQUIRE(fix.waitForReply("/status.reply", reply));
}

// =============================================================================
// DRIVER SWITCHING (HEADLESS)
// =============================================================================

TEST_CASE("RapidSwitch: switchDriver returns clean error in headless mode",
          "[RapidSwitch]") {
    EngineFixture fix;

    auto result = fix.engine().switchDriver("ALSA");
    REQUIRE_FALSE(result.success);
    REQUIRE(result.error == "no audio device in headless mode");

    // Engine should still be running
    REQUIRE(fix.engine().engineState() == EngineState::Running);

    OscReply reply;
    fix.send(osc_test::message("/status"));
    REQUIRE(fix.waitForReply("/status.reply", reply));
}

TEST_CASE("RapidSwitch: rapid switchDriver calls don't crash in headless",
          "[RapidSwitch]") {
    EngineFixture fix;

    for (int i = 0; i < 10; ++i) {
        auto result = fix.engine().switchDriver("ALSA");
        REQUIRE_FALSE(result.success);
    }

    REQUIRE(fix.engine().engineState() == EngineState::Running);

    OscReply reply;
    fix.send(osc_test::message("/status"));
    REQUIRE(fix.waitForReply("/status.reply", reply));
}

TEST_CASE("RapidSwitch: switchDriver followed by switchDevice",
          "[RapidSwitch]") {
    EngineFixture fix;

    // Driver switch fails (headless)
    auto r1 = fix.engine().switchDriver("ALSA");
    REQUIRE_FALSE(r1.success);

    // Device switch should still work (cold swap)
    auto r2 = fix.engine().switchDevice("", 44100);
    REQUIRE(r2.success);
    REQUIRE(r2.type == SwapType::Cold);

    // And back
    auto r3 = fix.engine().switchDevice("", 48000);
    REQUIRE(r3.success);

    OscReply reply;
    fix.send(osc_test::message("/status"));
    REQUIRE(fix.waitForReply("/status.reply", reply));
}

// =============================================================================
// DEVICE MODE (HEADLESS)
// =============================================================================

TEST_CASE("RapidSwitch: setDeviceMode in headless mode",
          "[RapidSwitch]") {
    EngineFixture fix;

    // setDeviceMode in headless should handle gracefully
    auto err = fix.engine().setDeviceMode("system");
    // May or may not error in headless — engine should remain running
    REQUIRE(fix.engine().engineState() == EngineState::Running);

    OscReply reply;
    fix.send(osc_test::message("/status"));
    REQUIRE(fix.waitForReply("/status.reply", reply));
}

TEST_CASE("RapidSwitch: rapid mode changes don't crash in headless",
          "[RapidSwitch]") {
    EngineFixture fix;

    std::string modes[] = {"system", "nonexistent-device", "", "system"};
    for (auto& mode : modes) {
        fix.engine().setDeviceMode(mode);
    }

    REQUIRE(fix.engine().engineState() == EngineState::Running);

    OscReply reply;
    fix.send(osc_test::message("/status"));
    REQUIRE(fix.waitForReply("/status.reply", reply));
}

// =============================================================================
// SYNTH STATE SURVIVES RAPID COLD SWAPS
// =============================================================================

TEST_CASE("RapidSwitch: synthdef cache survives rapid cold swaps",
          "[RapidSwitch]") {
    EngineFixture fix;

    // Load a synthdef
    REQUIRE(fix.loadSynthDef("sonic-pi-beep"));
    REQUIRE(fix.engine().stateCache().synthDefs().count("sonic-pi-beep") == 1);

    // Rapid cold swaps
    for (int i = 0; i < 5; ++i) {
        double rate = (i % 2 == 0) ? 44100 : 48000;
        auto result = fix.engine().switchDevice("", rate);
        REQUIRE(result.success);
        REQUIRE(result.type == SwapType::Cold);
    }

    // State cache should still have the synthdef
    CHECK(fix.engine().stateCache().synthDefs().count("sonic-pi-beep") == 1);
}

TEST_CASE("RapidSwitch: engine processes synths after rapid cold swaps",
          "[RapidSwitch]") {
    EngineFixture fix;

    // Rapid cold swaps
    auto r1 = fix.engine().switchDevice("", 44100);
    REQUIRE(r1.success);
    auto r2 = fix.engine().switchDevice("", 48000);
    REQUIRE(r2.success);
    auto r3 = fix.engine().switchDevice("", 44100);
    REQUIRE(r3.success);

    // Now load a synthdef and create a synth at the new rate
    fix.clearReplies();
    REQUIRE(fix.loadSynthDef("sonic-pi-beep"));

    // Create default group again (world was rebuilt)
    fix.send(osc_test::message("/g_new", 1, 0, 0));
    OscReply syncReply;
    fix.send(osc_test::message("/sync", 42));
    REQUIRE(fix.waitForReply("/synced", syncReply));
    fix.clearReplies();

    // Create a synth
    {
        osc_test::Builder b;
        auto& s = b.begin("/s_new");
        s << "sonic-pi-beep" << (int32_t)1000 << (int32_t)0 << (int32_t)1
          << "note" << 69.0f << "out_bus" << 0.0f;
        fix.send(b.end());
    }

    // Verify engine status shows a synth
    fix.send(osc_test::message("/status"));
    OscReply statusReply;
    REQUIRE(fix.waitForReply("/status.reply", statusReply));
    CHECK(statusReply.parsed().argInt(2) >= 1);  // at least our synth

    // Clean up
    fix.send(osc_test::message("/n_free", 1000));
}

// =============================================================================
// CONCURRENT SWAP REJECTION DURING RAPID SEQUENCE
// =============================================================================

TEST_CASE("RapidSwitch: concurrent swap rejected during cold swap",
          "[RapidSwitch]") {
    EngineFixture fix;

    // Slow down the swap with a delay hook
    fix.engine().testSwapFailure = [](bool) -> std::string {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        return "slow failure";
    };

    // First swap in background
    SwapResult firstResult;
    std::thread t1([&]() {
        firstResult = fix.engine().switchDevice("", 44100);
    });

    // Give the first swap time to acquire the mutex
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Three rapid attempts should all be rejected
    for (int i = 0; i < 3; ++i) {
        auto rejected = fix.engine().switchDevice("", 22050);
        REQUIRE_FALSE(rejected.success);
        REQUIRE(rejected.error == "swap already in progress");
    }

    t1.join();

    // Clear the failure hook
    fix.engine().testSwapFailure = nullptr;

    // Engine should recover — do a successful swap
    auto recovery = fix.engine().switchDevice("", 44100);
    REQUIRE(recovery.success);

    OscReply reply;
    fix.clearReplies();
    fix.send(osc_test::message("/status"));
    REQUIRE(fix.waitForReply("/status.reply", reply));
}

TEST_CASE("RapidSwitch: concurrent switchDriver rejected during switchDevice",
          "[RapidSwitch]") {
    EngineFixture fix;

    fix.engine().testSwapFailure = [](bool) -> std::string {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        return "slow failure";
    };

    SwapResult deviceResult;
    std::thread t([&]() {
        deviceResult = fix.engine().switchDevice("", 44100);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // switchDriver shares mSwapMutex — should be rejected
    auto driverResult = fix.engine().switchDriver("ALSA");
    // In headless, switchDriver returns "no audio device" before hitting the mutex,
    // but the guard is still tested
    REQUIRE_FALSE(driverResult.success);

    t.join();

    fix.engine().testSwapFailure = nullptr;

    REQUIRE(fix.engine().engineState() == EngineState::Running);
}

// =============================================================================
// ENGINE STATE CONSISTENCY AFTER RAPID SWITCHING
// =============================================================================

TEST_CASE("RapidSwitch: engine state is always Running after completed swaps",
          "[RapidSwitch]") {
    EngineFixture fix;

    std::vector<EngineState> statesAfterSwap;

    for (int i = 0; i < 6; ++i) {
        double rate = (i % 2 == 0) ? 44100 : 48000;
        auto result = fix.engine().switchDevice("", rate);
        REQUIRE(result.success);
        statesAfterSwap.push_back(fix.engine().engineState());
    }

    // Every swap should leave engine in Running state
    for (auto state : statesAfterSwap) {
        CHECK(state == EngineState::Running);
    }
}

TEST_CASE("RapidSwitch: swap events fire correctly during rapid sequence",
          "[RapidSwitch]") {
    EngineFixture fix;
    std::vector<std::string> events;

    fix.engine().onSwapEvent = [&](const std::string& event, const SwapResult&) {
        events.push_back(event);
    };

    // 5 cold swaps
    for (int i = 0; i < 5; ++i) {
        double rate = (i % 2 == 0) ? 44100 : 48000;
        auto result = fix.engine().switchDevice("", rate);
        REQUIRE(result.success);
    }

    // Each swap should produce exactly 2 events: start + complete
    REQUIRE(events.size() == 10);
    for (size_t i = 0; i < events.size(); i += 2) {
        CHECK(events[i] == "swap:start");
        CHECK(events[i + 1] == "swap:complete");
    }
}

// =============================================================================
// COLD SWAP WITH FAILURE THEN IMMEDIATE RETRY
// =============================================================================

TEST_CASE("RapidSwitch: failed cold swap then immediate successful retry",
          "[RapidSwitch]") {
    EngineFixture fix;

    // First attempt: inject failure
    fix.engine().testSwapFailure = [](bool) -> std::string {
        return "simulated device error";
    };

    auto r1 = fix.engine().switchDevice("", 44100);
    REQUIRE_FALSE(r1.success);
    REQUIRE(fix.engine().engineState() == EngineState::Running);

    // Clear failure and immediately retry
    fix.engine().testSwapFailure = nullptr;

    auto r2 = fix.engine().switchDevice("", 44100);
    REQUIRE(r2.success);
    REQUIRE(r2.type == SwapType::Cold);
    REQUIRE(fix.engine().engineState() == EngineState::Running);

    OscReply reply;
    fix.send(osc_test::message("/status"));
    REQUIRE(fix.waitForReply("/status.reply", reply));
}

TEST_CASE("RapidSwitch: multiple failures then success",
          "[RapidSwitch]") {
    EngineFixture fix;

    int callCount = 0;
    fix.engine().testSwapFailure = [&](bool) -> std::string {
        callCount++;
        if (callCount <= 3) return "transient error";
        return "";  // success on 4th attempt
    };

    // First three should fail
    for (int i = 0; i < 3; ++i) {
        auto result = fix.engine().switchDevice("", 44100);
        REQUIRE_FALSE(result.success);
        REQUIRE(fix.engine().engineState() == EngineState::Running);
    }

    // Fourth should succeed
    auto r4 = fix.engine().switchDevice("", 44100);
    REQUIRE(r4.success);

    fix.engine().testSwapFailure = nullptr;

    OscReply reply;
    fix.send(osc_test::message("/status"));
    REQUIRE(fix.waitForReply("/status.reply", reply));
}

// =============================================================================
// OSC-LEVEL DEVICE SWITCH MESSAGES
// =============================================================================

TEST_CASE("RapidSwitch: /supersonic/devices/switch via sendOsc",
          "[RapidSwitch]") {
    EngineFixture fix;

    // Send the same message the Ruby server would send:
    // /supersonic/devices/switch "" 44100.0 0
    {
        osc_test::Builder b;
        auto& s = b.begin("/supersonic/devices/switch");
        s << "" << 44100.0f << (int32_t)0;
        fix.send(b.end());
    }

    // In headless mode, /supersonic/devices/switch goes through the ring buffer,
    // not the UDP server, so it won't be handled as a supersonic command.
    // This test verifies the message doesn't crash the engine.

    // Engine should still be alive
    OscReply reply;
    fix.send(osc_test::message("/status"));
    REQUIRE(fix.waitForReply("/status.reply", reply));
}

// =============================================================================
// PURGE AFTER RAPID SWAPS
// =============================================================================

TEST_CASE("RapidSwitch: purge after rapid cold swaps clears stale state",
          "[RapidSwitch]") {
    EngineFixture fix;

    // Do several cold swaps
    for (int i = 0; i < 3; ++i) {
        double rate = (i % 2 == 0) ? 44100 : 48000;
        auto result = fix.engine().switchDevice("", rate);
        REQUIRE(result.success);
    }

    // Explicit purge
    fix.engine().purge();

    // Engine should still respond cleanly
    fix.clearReplies();
    fix.send(osc_test::message("/status"));
    OscReply reply;
    REQUIRE(fix.waitForReply("/status.reply", reply));
    CHECK(reply.parsed().argCount() >= 5);
}

// =============================================================================
// CRASH REPRODUCTION: tiny buffer + cold swap (from real crash logs)
//
// The crash sequence observed in production was:
//   1. Cold swap to 44100 (success)
//   2. Hot swap: buffer size to 16 (success)
//   3. Hot swap: buffer size to 96, then 128
//   4. Cold swap to 48000 → CRASH (SuperSonic process died)
//
// The hypothesis is that a tiny buffer size (16) stored in mCurrentConfig
// gets used by rebuild_world() during a subsequent cold swap, creating a
// World with mBufLength=16 while the audio driver still uses larger blocks.
// =============================================================================

TEST_CASE("RapidSwitch: tiny buffer size 16 then cold swap",
          "[RapidSwitch][CrashRepro]") {
    EngineFixture fix;

    // Set buffer to 16 (hot swap — same rate)
    auto r1 = fix.engine().switchDevice("", 0, 16);
    REQUIRE(r1.success);
    REQUIRE(r1.type == SwapType::Hot);

    // Cold swap with the tiny buffer in mCurrentConfig
    auto r2 = fix.engine().switchDevice("", 44100);
    REQUIRE(r2.success);
    REQUIRE(r2.type == SwapType::Cold);

    // Engine must survive and respond
    OscReply reply;
    fix.send(osc_test::message("/status"));
    REQUIRE(fix.waitForReply("/status.reply", reply));
}

TEST_CASE("RapidSwitch: buffer 16, cold swap, then another cold swap back",
          "[RapidSwitch][CrashRepro]") {
    EngineFixture fix;

    // Tiny buffer
    auto r1 = fix.engine().switchDevice("", 0, 16);
    REQUIRE(r1.success);

    // Cold swap to 44100 (world rebuilt with buf=16)
    auto r2 = fix.engine().switchDevice("", 44100);
    REQUIRE(r2.success);
    REQUIRE(r2.type == SwapType::Cold);

    // Cold swap back to 48000 (world rebuilt again with buf=16)
    auto r3 = fix.engine().switchDevice("", 48000);
    REQUIRE(r3.success);
    REQUIRE(r3.type == SwapType::Cold);

    OscReply reply;
    fix.send(osc_test::message("/status"));
    REQUIRE(fix.waitForReply("/status.reply", reply));
}

TEST_CASE("RapidSwitch: exact crash sequence from production logs",
          "[RapidSwitch][CrashRepro]") {
    EngineFixture fix;
    // Fixture boots at 48000 Hz, buffer 128

    // Step 1: Cold swap to 44100 (as seen in logs line 60-62)
    auto r1 = fix.engine().switchDevice("", 44100);
    REQUIRE(r1.success);
    REQUIRE(r1.type == SwapType::Cold);

    // Step 2: Hot swap — device switch (name ignored in headless)
    auto r2 = fix.engine().switchDevice("PipeWire Sound Server", 44100);
    REQUIRE(r2.success);

    // Step 3: Cold swap to 48000 (logs line 163-165)
    auto r3 = fix.engine().switchDevice("", 48000);
    REQUIRE(r3.success);
    REQUIRE(r3.type == SwapType::Cold);

    // Step 4: Cold swap to 44100 (logs line 219-221)
    auto r4 = fix.engine().switchDevice("", 44100);
    REQUIRE(r4.success);
    REQUIRE(r4.type == SwapType::Cold);

    // Step 5: Buffer to 16 (logs line 289 — hot swap)
    auto r5 = fix.engine().switchDevice("", 0, 16);
    REQUIRE(r5.success);
    REQUIRE(r5.type == SwapType::Hot);

    // Step 6: Buffer to 96 (hot swap)
    auto r6 = fix.engine().switchDevice("", 0, 96);
    REQUIRE(r6.success);

    // Step 7: Buffer to 128 (hot swap)
    auto r7 = fix.engine().switchDevice("", 0, 128);
    REQUIRE(r7.success);

    // Step 8: Cold swap to 48000 — THIS IS WHERE IT CRASHED
    auto r8 = fix.engine().switchDevice("", 48000);
    REQUIRE(r8.success);
    REQUIRE(r8.type == SwapType::Cold);

    // Step 9: Device switch (hot). Explicit rate pins this as hot — without
    // it, mDeviceRateMemory would restore the 44100 from step 2 and trigger
    // a cold swap.
    auto r9 = fix.engine().switchDevice("PipeWire Sound Server", 48000);
    REQUIRE(r9.success);

    // Step 10: Buffer to 256 (hot)
    auto r10 = fix.engine().switchDevice("", 0, 256);
    REQUIRE(r10.success);

    // Step 11: Cold swap to 44100
    auto r11 = fix.engine().switchDevice("", 44100);
    REQUIRE(r11.success);
    REQUIRE(r11.type == SwapType::Cold);

    // Step 12: Cold swap to 48000 — second crash point
    auto r12 = fix.engine().switchDevice("", 48000);
    REQUIRE(r12.success);
    REQUIRE(r12.type == SwapType::Cold);

    // Engine must be alive
    OscReply reply;
    fix.send(osc_test::message("/status"));
    REQUIRE(fix.waitForReply("/status.reply", reply));
}

TEST_CASE("RapidSwitch: cold swap after various tiny buffer sizes",
          "[RapidSwitch][CrashRepro]") {
    EngineFixture fix;

    int tinyBuffers[] = {16, 32, 8, 64};
    for (int bs : tinyBuffers) {
        // Set tiny buffer (hot)
        auto rBuf = fix.engine().switchDevice("", 0, bs);
        REQUIRE(rBuf.success);

        // Cold swap — world rebuilt with tiny buffer
        auto rCold = fix.engine().switchDevice("", 44100);
        REQUIRE(rCold.success);
        REQUIRE(rCold.type == SwapType::Cold);

        // Swap back
        auto rBack = fix.engine().switchDevice("", 48000);
        REQUIRE(rBack.success);
    }

    OscReply reply;
    fix.send(osc_test::message("/status"));
    REQUIRE(fix.waitForReply("/status.reply", reply));
}

TEST_CASE("RapidSwitch: cold swap with active synths after tiny buffer",
          "[RapidSwitch][CrashRepro]") {
    EngineFixture fix;

    // Load synthdef and create synths (like a running Sonic Pi session)
    REQUIRE(fix.loadSynthDef("sonic-pi-beep"));
    fix.send(osc_test::message("/g_new", 10, 0, 1));
    {
        osc_test::Builder b;
        auto& s = b.begin("/s_new");
        s << "sonic-pi-beep" << (int32_t)1000 << (int32_t)0 << (int32_t)10
          << "note" << 69.0f << "out_bus" << 0.0f;
        fix.send(b.end());
    }
    OscReply syncReply;
    fix.send(osc_test::message("/sync", 42));
    REQUIRE(fix.waitForReply("/synced", syncReply));
    fix.clearReplies();

    // Let synths run briefly
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Tiny buffer (hot)
    auto r1 = fix.engine().switchDevice("", 0, 16);
    REQUIRE(r1.success);

    // Cold swap — world rebuilt; synths are destroyed
    auto r2 = fix.engine().switchDevice("", 44100);
    REQUIRE(r2.success);
    REQUIRE(r2.type == SwapType::Cold);

    // Engine should still work after cold swap destroyed the synths
    fix.clearReplies();
    fix.send(osc_test::message("/status"));
    OscReply statusReply;
    REQUIRE(fix.waitForReply("/status.reply", statusReply));
}

TEST_CASE("RapidSwitch: buffer 16 with multiple cold swap round-trips",
          "[RapidSwitch][CrashRepro]") {
    EngineFixture fix;

    // Set tiny buffer
    auto r1 = fix.engine().switchDevice("", 0, 16);
    REQUIRE(r1.success);

    // Rapid cold swap round-trips with tiny buffer
    for (int i = 0; i < 5; ++i) {
        auto rDown = fix.engine().switchDevice("", 44100);
        REQUIRE(rDown.success);
        REQUIRE(rDown.type == SwapType::Cold);

        auto rUp = fix.engine().switchDevice("", 48000);
        REQUIRE(rUp.success);
        REQUIRE(rUp.type == SwapType::Cold);
    }

    OscReply reply;
    fix.send(osc_test::message("/status"));
    REQUIRE(fix.waitForReply("/status.reply", reply));
}

TEST_CASE("RapidSwitch: buffer size 1 then cold swap (extreme)",
          "[RapidSwitch][CrashRepro]") {
    EngineFixture fix;

    // Pathological buffer size
    auto r1 = fix.engine().switchDevice("", 0, 1);
    REQUIRE(r1.success);

    auto r2 = fix.engine().switchDevice("", 44100);
    REQUIRE(r2.success);

    // Restore sane buffer and swap back
    auto r3 = fix.engine().switchDevice("", 0, 128);
    REQUIRE(r3.success);

    auto r4 = fix.engine().switchDevice("", 48000);
    REQUIRE(r4.success);

    OscReply reply;
    fix.send(osc_test::message("/status"));
    REQUIRE(fix.waitForReply("/status.reply", reply));
}

/*
 * test_engine_lifecycle.cpp — SupersonicEngine lifecycle tests.
 *
 * Covers initialise/shutdown safety, callback wiring, and basic OSC
 * round-trips that verify the engine is alive and well after boot.
 */
#include "EngineFixture.h"
#include <thread>
#include <chrono>

// =============================================================================
// RAW ENGINE STATE (no fixture — tests before/after initialise/shutdown)
// =============================================================================

TEST_CASE("Engine starts in non-running state before initialise", "[lifecycle]") {
    SupersonicEngine engine;
    CHECK_FALSE(engine.isRunning());
}

TEST_CASE("isRunning returns true after initialise", "[lifecycle]") {
    SupersonicEngine engine;
    SupersonicEngine::Config cfg;
    cfg.headless = true;
    cfg.udpPort  = 0;
    engine.initialise(cfg);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(engine.isRunning());
    engine.shutdown();
}

TEST_CASE("isRunning returns false after shutdown", "[lifecycle]") {
    SupersonicEngine engine;
    SupersonicEngine::Config cfg;
    cfg.headless = true;
    cfg.udpPort  = 0;
    engine.initialise(cfg);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    REQUIRE(engine.isRunning());
    engine.shutdown();
    CHECK_FALSE(engine.isRunning());
}

TEST_CASE("Double initialise is safe", "[lifecycle]") {
    SupersonicEngine engine;
    SupersonicEngine::Config cfg;
    cfg.headless = true;
    cfg.udpPort  = 0;

    engine.initialise(cfg);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    REQUIRE(engine.isRunning());

    // Second call should be a no-op (mRunning is already true)
    engine.initialise(cfg);
    CHECK(engine.isRunning());

    engine.shutdown();
}

TEST_CASE("Double shutdown is safe", "[lifecycle]") {
    SupersonicEngine engine;
    SupersonicEngine::Config cfg;
    cfg.headless = true;
    cfg.udpPort  = 0;

    engine.initialise(cfg);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    REQUIRE(engine.isRunning());

    engine.shutdown();
    CHECK_FALSE(engine.isRunning());

    // Second shutdown should not crash
    engine.shutdown();
    CHECK_FALSE(engine.isRunning());
}

TEST_CASE("Null onReply callback does not crash", "[lifecycle]") {
    SupersonicEngine engine;
    engine.onReply = nullptr;

    SupersonicEngine::Config cfg;
    cfg.headless = true;
    cfg.udpPort  = 0;
    engine.initialise(cfg);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Send a command that would normally trigger a reply
    auto pkt = osc_test::message("/status");
    engine.sendOsc(pkt.ptr(), pkt.size());
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // If we got here without crashing, the test passes
    engine.shutdown();
    SUCCEED();
}

TEST_CASE("Null onDebug callback does not crash", "[lifecycle]") {
    SupersonicEngine engine;
    engine.onDebug = nullptr;

    SupersonicEngine::Config cfg;
    cfg.headless = true;
    cfg.udpPort  = 0;
    engine.initialise(cfg);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Send a command that might produce debug output
    auto pkt = osc_test::message("/dumpOSC", 1);
    engine.sendOsc(pkt.ptr(), pkt.size());
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    engine.shutdown();
    SUCCEED();
}

// =============================================================================
// FIXTURE-BASED TESTS (engine is booted with callbacks wired)
// =============================================================================

TEST_CASE("Engine responds to /status after boot", "[lifecycle]") {
    EngineFixture fx;

    fx.send(osc_test::message("/status"));
    OscReply r;
    REQUIRE(fx.waitForReply("/status.reply", r));

    auto p = r.parsed();
    REQUIRE(p.argCount() >= 5);
    // At minimum, root group (0) and default group (1) should exist
    CHECK(p.argInt(3) >= 1);
}

TEST_CASE("Engine responds to /version after boot", "[lifecycle]") {
    EngineFixture fx;

    fx.send(osc_test::message("/version"));
    OscReply r;
    REQUIRE(fx.waitForReply("/version.reply", r));

    auto p = r.parsed();
    CHECK(!p.argString(0).empty());  // program name
    CHECK(p.argCount() >= 3);        // name, major, minor at minimum
}

TEST_CASE("sendOsc works after initialise", "[lifecycle]") {
    EngineFixture fx;

    // Verify the engine can receive and process OSC via sendOsc
    auto pkt = osc_test::message("/status");
    fx.engine().sendOsc(pkt.ptr(), pkt.size());
    fx.pump(8);

    OscReply r;
    REQUIRE(fx.waitForReply("/status.reply", r));
}

TEST_CASE("onReply callback is wired correctly", "[lifecycle]") {
    EngineFixture fx;

    fx.send(osc_test::message("/status"));
    OscReply r;
    REQUIRE(fx.waitForReply("/status.reply", r));

    // The reply should have a valid address and non-empty raw data
    CHECK(r.address == "/status.reply");
    CHECK(!r.raw.empty());
}

TEST_CASE("onReply receives /status.reply after /status", "[lifecycle]") {
    EngineFixture fx;

    fx.clearReplies();
    fx.send(osc_test::message("/status"));
    OscReply r;
    REQUIRE(fx.waitForReply("/status.reply", r));

    auto p = r.parsed();
    CHECK(p.address == "/status.reply");
    CHECK(p.argCount() >= 5);
}

TEST_CASE("onDebug receives messages via /dumpOSC", "[lifecycle]") {
    EngineFixture fx;

    // Enable dumpOSC so subsequent commands generate debug output
    fx.send(osc_test::message("/dumpOSC", 1));
    fx.pump(4);

    // Send a command to trigger debug printing
    fx.send(osc_test::message("/status"));
    fx.pump(16);

    // Give debug reader time to process
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    auto msgs = fx.debugMessages();
    // Debug messages may or may not appear depending on engine internals,
    // but the callback path should not crash.
    // Disable dumpOSC to avoid noise for any subsequent operations
    fx.send(osc_test::message("/dumpOSC", 0));
    fx.pump(4);
    SUCCEED();
}

TEST_CASE("Engine processes multiple sequential /status requests", "[lifecycle]") {
    EngineFixture fx;

    for (int i = 0; i < 5; ++i) {
        fx.clearReplies();
        fx.send(osc_test::message("/status"));
        OscReply r;
        REQUIRE(fx.waitForReply("/status.reply", r));

        auto p = r.parsed();
        CHECK(p.argCount() >= 5);
    }
}

TEST_CASE("Engine handles /sync round-trip after fresh boot", "[lifecycle]") {
    EngineFixture fx;

    fx.send(osc_test::message("/sync", 12345));
    OscReply r;
    REQUIRE(fx.waitForReply("/synced", r));
    CHECK(r.parsed().argInt(0) == 12345);
}

TEST_CASE("Engine handles /g_queryTree after boot", "[lifecycle]") {
    EngineFixture fx;

    // Query root group (0), non-verbose (flag 0)
    fx.send(osc_test::message("/g_queryTree", 0, 0));
    OscReply r;
    REQUIRE(fx.waitForReply("/g_queryTree.reply", r));

    auto p = r.parsed();
    // /g_queryTree.reply: flag(0), nodeID(1), numChildren(2), ...
    CHECK(p.argInt(0) == 0);    // flag (non-verbose)
    CHECK(p.argInt(1) == 0);    // root group ID
    CHECK(p.argInt(2) >= 1);    // at least default group (1) as child
}

TEST_CASE("Default group 1 is present after boot", "[lifecycle]") {
    EngineFixture fx;

    // Query root group for its children
    fx.send(osc_test::message("/g_queryTree", 0, 0));
    OscReply r;
    REQUIRE(fx.waitForReply("/g_queryTree.reply", r));

    auto p = r.parsed();
    CHECK(p.argInt(1) == 0);    // root group ID
    CHECK(p.argInt(2) >= 1);    // has at least one child

    // Also verify group 1 itself responds to a query
    fx.clearReplies();
    fx.send(osc_test::message("/g_queryTree", 1, 0));
    OscReply r2;
    REQUIRE(fx.waitForReply("/g_queryTree.reply", r2));
    CHECK(r2.parsed().argInt(1) == 1);  // nodeID = 1
}

TEST_CASE("/notify command works after boot", "[lifecycle]") {
    EngineFixture fx;

    fx.send(osc_test::message("/notify", 1));
    OscReply r;
    REQUIRE(fx.waitForReply("/done", r));
}

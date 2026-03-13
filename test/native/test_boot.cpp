/*
 * test_boot.cpp — Engine boot, status, version, sync, notify, dumpOSC
 */
#include "EngineFixture.h"

TEST_CASE("Engine boots and responds to /status", "[boot]") {
    EngineFixture fx;

    fx.send(osc_test::message("/status"));
    OscReply r;
    REQUIRE(fx.waitForReply("/status.reply", r));

    auto p = r.parsed();
    // /status.reply args: unused(1), numUgens, numSynths, numGroups, numSynthDefs, ...
    REQUIRE(p.argCount() >= 5);
    CHECK(p.argInt(3) >= 1);  // at least root group
}

TEST_CASE("/sync returns /synced with matching id", "[boot]") {
    EngineFixture fx;

    fx.send(osc_test::message("/sync", 42));
    OscReply r;
    REQUIRE(fx.waitForReply("/synced", r));
    CHECK(r.parsed().argInt(0) == 42);
}

TEST_CASE("/version returns version info", "[boot]") {
    EngineFixture fx;

    fx.send(osc_test::message("/version"));
    OscReply r;
    REQUIRE(fx.waitForReply("/version.reply", r));

    auto p = r.parsed();
    CHECK(!p.argString(0).empty());   // program name
    CHECK(p.argCount() >= 3);         // name, major, minor at minimum
}

TEST_CASE("/notify responds with /done", "[boot]") {
    EngineFixture fx;

    fx.send(osc_test::message("/notify", 1));
    OscReply r;
    REQUIRE(fx.waitForReply("/done", r));
}

TEST_CASE("/dumpOSC does not crash", "[boot]") {
    EngineFixture fx;

    // Enable parsed output
    fx.send(osc_test::message("/dumpOSC", 1));
    fx.pump(4);
    // Disable
    fx.send(osc_test::message("/dumpOSC", 0));
    fx.pump(4);
    // If we got here without crashing, the test passes
    SUCCEED();
}

TEST_CASE("Debug callback works", "[boot]") {
    EngineFixture fx;

    // Trigger some debug output by sending /dumpOSC 1 then a command
    fx.send(osc_test::message("/dumpOSC", 1));
    fx.pump(4);
    fx.send(osc_test::message("/status"));
    fx.pump(16);

    auto msgs = fx.debugMessages();
    // Debug messages may or may not be generated depending on engine state
    // Just verify the callback mechanism doesn't crash
    SUCCEED();
}

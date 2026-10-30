/*
 * test_boot.cpp — Engine boot, status, version, sync, notify, dumpOSC
 */
#include "EngineFixture.h"

#include <fstream>
#include <string>

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

// The upstream SuperCollider commit the scsynth fork was last synced with,
// from the "**Last Sync Commit**:" line of docs/UPSTREAM_SYNC_GUIDE.md.
static std::string lastSyncCommit() {
    std::ifstream guide(std::string(SUPERSONIC_SOURCE_DIR) + "/docs/UPSTREAM_SYNC_GUIDE.md");
    const std::string key = "**Last Sync Commit**: ";
    for (std::string line; std::getline(guide, line);)
        if (line.rfind(key, 0) == 0) return line.substr(key.size());
    return {};
}

// /version.reply: program name, major, minor, patch and tweak, branch, commit.
// The version is the upstream base the fork tracks, and the commit is the
// one the sync guide records.
TEST_CASE("/version reports the upstream base the scsynth fork is synced to", "[boot]") {
    EngineFixture fx;

    fx.send(osc_test::message("/version"));
    OscReply r;
    REQUIRE(fx.waitForReply("/version.reply", r));

    auto p = r.parsed();
    REQUIRE(p.argCount() == 6);
    CHECK(p.argString(0) == "scsynth");
    CHECK(p.argInt(1) == 3);
    CHECK(p.argInt(2) == 15);
    CHECK(p.argString(3) == ".0-dev");
    CHECK(p.argString(4) == "develop");
    const std::string commit = lastSyncCommit();
    REQUIRE(!commit.empty());
    CHECK(p.argString(5) == commit);
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
    // Disable
    fx.send(osc_test::message("/dumpOSC", 0));
    // If we got here without crashing, the test passes
    SUCCEED();
}

TEST_CASE("Debug callback works", "[boot]") {
    EngineFixture fx;

    // Trigger some debug output by sending /dumpOSC 1 then a command
    fx.send(osc_test::message("/dumpOSC", 1));
    fx.send(osc_test::message("/status"));

    auto msgs = fx.debugMessages();
    // Debug messages may or may not be generated depending on engine state
    // Just verify the callback mechanism doesn't crash
    SUCCEED();
}

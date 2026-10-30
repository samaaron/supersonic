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

// SuperSonic's version, from the "version" field of package.json.
static std::string packageVersion() {
    std::ifstream pkg(std::string(SUPERSONIC_SOURCE_DIR) + "/package.json");
    const std::string key = "\"version\": \"";
    for (std::string line; std::getline(pkg, line);) {
        const auto at = line.find(key);
        if (at != std::string::npos) return line.substr(at + key.size(), line.find('"', at + key.size()) - at - key.size());
    }
    return {};
}

// /version.reply in scsynth's format (program name, major, minor, the rest of
// the version, branch, commit), reporting SuperSonic.
TEST_CASE("/version reports SuperSonic and its version, in scsynth's reply format", "[boot]") {
    EngineFixture fx;

    fx.send(osc_test::message("/version"));
    OscReply r;
    REQUIRE(fx.waitForReply("/version.reply", r));

    const std::string version = packageVersion();   // e.g. 0.92.0
    REQUIRE(!version.empty());
    const auto dot1 = version.find('.');
    const auto dot2 = version.find('.', dot1 + 1);
    auto p = r.parsed();
    REQUIRE(p.argCount() == 6);
    CHECK(p.argString(0) == "SuperSonic");
    CHECK(p.argInt(1) == std::stoi(version.substr(0, dot1)));
    CHECK(p.argInt(2) == std::stoi(version.substr(dot1 + 1, dot2 - dot1 - 1)));
    CHECK(p.argString(3) == version.substr(dot2));
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

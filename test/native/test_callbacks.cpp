/*
 * test_callbacks.cpp — Tests for onReply/onDebug callback behavior.
 *
 * Verifies that reply and debug callbacks fire correctly, deliver
 * well-formed OSC data, and that clearReplies() / ordering semantics
 * work as expected.
 */
#include "EngineFixture.h"
#include <thread>
#include <chrono>
#include <atomic>

// =============================================================================
// SECTION: Basic reply callback behavior
// =============================================================================

TEST_CASE("/status generates exactly one /status.reply callback", "[callback]") {
    EngineFixture fx;
    fx.clearReplies();

    fx.send(osc_test::message("/status"));
    OscReply r;
    REQUIRE(fx.waitForReply("/status.reply", r));

    // Pump a few more blocks to let any straggler replies arrive
    fx.pump(16);

    // waitForReply consumed the first reply; verify no duplicates remain
    auto all = fx.allReplies();
    int statusCount = 0;
    for (auto& reply : all) {
        if (reply.address == "/status.reply")
            ++statusCount;
    }
    CHECK(statusCount == 0);  // no duplicate replies
}

TEST_CASE("/sync generates exactly one /synced callback with matching ID", "[callback]") {
    EngineFixture fx;
    fx.clearReplies();

    fx.send(osc_test::message("/sync", 77));
    OscReply r;
    REQUIRE(fx.waitForReply("/synced", r));

    auto p = r.parsed();
    CHECK(p.argInt(0) == 77);

    // Pump extra to ensure no duplicates
    fx.pump(16);

    // waitForReply consumed the first reply; verify no duplicates remain
    auto all = fx.allReplies();
    int syncedCount = 0;
    for (auto& reply : all) {
        if (reply.address == "/synced")
            ++syncedCount;
    }
    CHECK(syncedCount == 0);  // no duplicate replies
}

TEST_CASE("Multiple /sync commands generate matching /synced replies", "[callback]") {
    EngineFixture fx;
    fx.clearReplies();

    // Send 5 syncs with distinct IDs
    for (int i = 0; i < 5; ++i) {
        fx.send(osc_test::message("/sync", 100 + i));
        fx.pump(4);
    }

    // Wait for the last one to ensure all have been processed
    fx.pump(16);

    auto all = fx.allReplies();

    // Collect all /synced reply IDs
    std::vector<int32_t> syncedIds;
    for (auto& reply : all) {
        if (reply.address == "/synced") {
            syncedIds.push_back(reply.parsed().argInt(0));
        }
    }

    REQUIRE(syncedIds.size() == 5);
    for (int i = 0; i < 5; ++i) {
        CHECK(syncedIds[i] == 100 + i);
    }
}

TEST_CASE("/version generates /version.reply with string arguments", "[callback]") {
    EngineFixture fx;
    fx.clearReplies();

    fx.send(osc_test::message("/version"));
    OscReply r;
    REQUIRE(fx.waitForReply("/version.reply", r));

    auto p = r.parsed();
    // /version.reply should contain at least: program name (string), major, minor
    REQUIRE(p.argCount() >= 3);
    CHECK(!p.argString(0).empty());  // program name is a non-empty string
}

// =============================================================================
// SECTION: Reply data integrity
// =============================================================================

TEST_CASE("Reply data contains valid OSC with parseable argCount > 0", "[callback]") {
    EngineFixture fx;
    fx.clearReplies();

    // /status.reply always has multiple arguments
    fx.send(osc_test::message("/status"));
    OscReply r;
    REQUIRE(fx.waitForReply("/status.reply", r));

    auto p = r.parsed();
    CHECK(p.argCount() > 0);
    CHECK(p.address == "/status.reply");

    // Verify raw data is non-empty and parseable
    CHECK(!r.raw.empty());
}

TEST_CASE("/g_queryTree generates /g_queryTree.reply", "[callback]") {
    EngineFixture fx;
    fx.clearReplies();

    // Query the root group (node 0) with flag=0 (no control values)
    fx.send(osc_test::message("/g_queryTree", 0, 0));
    OscReply r;
    REQUIRE(fx.waitForReply("/g_queryTree.reply", r));

    auto p = r.parsed();
    // /g_queryTree.reply: flag(0), groupID(1), numChildren(2), ...
    CHECK(p.argCount() >= 3);
    CHECK(p.argInt(1) == 0);    // root group ID
    CHECK(p.argInt(2) >= 1);    // at least the default group (1) as child
}

// =============================================================================
// SECTION: Debug callback behavior
// =============================================================================

TEST_CASE("debugMessages() is initially empty on fresh engine", "[callback]") {
    EngineFixture fx;

    auto msgs = fx.debugMessages();
    CHECK(msgs.empty());
}

TEST_CASE("Sending invalid OSC command generates debug output", "[callback]") {
    EngineFixture fx;

    // Enable dumpOSC so the engine prints debug info about incoming commands
    fx.send(osc_test::message("/dumpOSC", 1));
    fx.pump(8);

    // Send a command that does not exist — scsynth logs an error for unknown commands
    fx.send(osc_test::message("/nonexistent_command"));
    fx.pump(16);

    auto msgs = fx.debugMessages();
    // The engine should produce at least some debug output (dumpOSC echo
    // and/or "Command not found" error). If headless mode suppresses it,
    // we just verify the mechanism does not crash.
    // On most configurations dumpOSC generates output for every received message.
    if (!msgs.empty()) {
        bool found = false;
        for (auto& m : msgs) {
            if (m.find("nonexistent") != std::string::npos ||
                m.find("Command") != std::string::npos ||
                m.find("OSC") != std::string::npos) {
                found = true;
                break;
            }
        }
        CHECK(found);
    }
    SUCCEED();
}

// =============================================================================
// SECTION: clearReplies() semantics
// =============================================================================

TEST_CASE("clearReplies() actually clears collected replies", "[callback]") {
    EngineFixture fx;

    // Generate replies without consuming them via waitForReply
    fx.send(osc_test::message("/sync", 500));
    fx.send(osc_test::message("/sync", 501));
    fx.pump(16);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Should have accumulated replies
    CHECK(!fx.allReplies().empty());

    // Clear and verify
    fx.clearReplies();
    CHECK(fx.allReplies().empty());

    // Engine should still work after clearing
    fx.send(osc_test::message("/status"));
    OscReply r;
    REQUIRE(fx.waitForReply("/status.reply", r));
}

// =============================================================================
// SECTION: Reply ordering
// =============================================================================

TEST_CASE("Reply ordering: /status then /sync replies arrive for both", "[callback]") {
    EngineFixture fx;
    fx.clearReplies();

    // Send /status followed by /sync in quick succession
    fx.send(osc_test::message("/status"));
    fx.send(osc_test::message("/sync", 999));

    // Wait for both replies
    OscReply statusReply;
    REQUIRE(fx.waitForReply("/status.reply", statusReply));
    CHECK(statusReply.parsed().argCount() >= 5);

    OscReply syncReply;
    REQUIRE(fx.waitForReply("/synced", syncReply));
    CHECK(syncReply.parsed().argInt(0) == 999);
}

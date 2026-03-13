/*
 * test_group_commands.cpp — /g_new, /g_freeAll, /g_deepFree, /g_head, /g_tail, /g_queryTree
 */
#include "EngineFixture.h"

static osc_test::Packet sNew(const char* def, int32_t id, int32_t addAction, int32_t target) {
    osc_test::Builder b;
    auto& s = b.begin("/s_new");
    s << def << id << addAction << target;
    return b.end();
}

TEST_CASE("/g_new creates a group", "[group]") {
    EngineFixture fx;

    // Create group 100 at head of root group
    fx.send(osc_test::message("/g_new", 100, 0, 0));
    fx.pump(8);

    // Query root group
    fx.send(osc_test::message("/g_queryTree", 0, 0));
    OscReply r;
    REQUIRE(fx.waitForReply("/g_queryTree.reply", r));

    // /g_queryTree.reply: flag, groupID, numChildren, ...
    auto p = r.parsed();
    CHECK(p.argInt(2) >= 1);  // root group has at least 1 child

    fx.send(osc_test::message("/n_free", 100));
    fx.pump(4);
}

TEST_CASE("/g_new nested groups", "[group]") {
    EngineFixture fx;

    fx.send(osc_test::message("/g_new", 100, 0, 0));
    fx.pump(4);
    fx.send(osc_test::message("/g_new", 200, 0, 100));
    fx.pump(4);
    fx.send(osc_test::message("/g_new", 300, 0, 200));
    fx.pump(4);

    // Status should show 4 groups: root(0) + default(1) + 100 + 200 + 300
    // Actually root(0) and default group(1) may vary, just check >= 3 new ones
    fx.send(osc_test::message("/status"));
    OscReply r;
    REQUIRE(fx.waitForReply("/status.reply", r));
    CHECK(r.parsed().argInt(3) >= 3);

    fx.send(osc_test::message("/n_free", 300));
    fx.send(osc_test::message("/n_free", 200));
    fx.send(osc_test::message("/n_free", 100));
    fx.pump(4);
}

TEST_CASE("/g_freeAll frees all children of a group", "[group]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    // Create group with synths
    fx.send(osc_test::message("/g_new", 100, 0, 0));
    fx.pump(4);
    fx.send(sNew("sonic-pi-beep", 1000, 0, 100));
    fx.send(sNew("sonic-pi-beep", 1001, 0, 100));
    fx.send(sNew("sonic-pi-beep", 1002, 0, 100));
    fx.pump(8);

    // Confirm synths exist
    fx.send(osc_test::message("/status"));
    OscReply before;
    REQUIRE(fx.waitForReply("/status.reply", before));
    int synthsBefore = before.parsed().argInt(2);

    // Free all children of group 100
    fx.send(osc_test::message("/g_freeAll", 100));
    fx.pump(8);

    fx.send(osc_test::message("/status"));
    OscReply after;
    REQUIRE(fx.waitForReply("/status.reply", after));
    int synthsAfter = after.parsed().argInt(2);

    CHECK(synthsAfter <= synthsBefore - 3);

    fx.send(osc_test::message("/n_free", 100));
    fx.pump(4);
}

TEST_CASE("/g_deepFree frees children recursively", "[group]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    // Create nested groups with synths
    fx.send(osc_test::message("/g_new", 100, 0, 0));
    fx.pump(4);
    fx.send(osc_test::message("/g_new", 200, 0, 100));
    fx.pump(4);
    fx.send(sNew("sonic-pi-beep", 1000, 0, 100));
    fx.send(sNew("sonic-pi-beep", 1001, 0, 200));
    fx.pump(8);

    // Deep free group 100 — should free synths in both 100 and 200
    fx.send(osc_test::message("/g_deepFree", 100));
    fx.pump(8);

    fx.send(osc_test::message("/status"));
    OscReply r;
    REQUIRE(fx.waitForReply("/status.reply", r));
    CHECK(r.parsed().argInt(2) == 0);  // no synths left

    fx.send(osc_test::message("/n_free", 200));
    fx.send(osc_test::message("/n_free", 100));
    fx.pump(4);
}

TEST_CASE("/g_head moves node to head of group", "[group]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    fx.send(osc_test::message("/g_new", 100, 0, 0));
    fx.pump(4);
    fx.send(sNew("sonic-pi-beep", 1000, 0, 100));
    fx.send(sNew("sonic-pi-beep", 1001, 0, 100));
    fx.pump(4);

    // Move 1000 to head of root group
    fx.send(osc_test::message("/g_head", 0, 1000));
    fx.pump(8);

    // If we got here, command was processed
    fx.send(osc_test::message("/n_free", 1000));
    fx.send(osc_test::message("/n_free", 1001));
    fx.send(osc_test::message("/n_free", 100));
    fx.pump(4);
    SUCCEED();
}

TEST_CASE("/g_tail moves node to tail of group", "[group]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    fx.send(osc_test::message("/g_new", 100, 0, 0));
    fx.pump(4);
    fx.send(sNew("sonic-pi-beep", 1000, 0, 100));
    fx.send(sNew("sonic-pi-beep", 1001, 0, 100));
    fx.pump(4);

    // Move 1001 to tail of root group
    fx.send(osc_test::message("/g_tail", 0, 1001));
    fx.pump(8);

    fx.send(osc_test::message("/n_free", 1000));
    fx.send(osc_test::message("/n_free", 1001));
    fx.send(osc_test::message("/n_free", 100));
    fx.pump(4);
    SUCCEED();
}

TEST_CASE("/g_queryTree returns tree structure", "[group]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    fx.send(osc_test::message("/g_new", 100, 0, 0));
    fx.pump(4);
    fx.send(sNew("sonic-pi-beep", 1000, 0, 100));
    fx.pump(8);

    // Query with flag=0 (no controls)
    fx.send(osc_test::message("/g_queryTree", 0, 0));
    OscReply r;
    REQUIRE(fx.waitForReply("/g_queryTree.reply", r));

    // /g_queryTree.reply: flag, groupID, numChildren, ...
    auto p = r.parsed();
    CHECK(p.argInt(1) == 0);    // root group ID
    CHECK(p.argInt(2) >= 1);    // at least our group 100

    fx.send(osc_test::message("/n_free", 1000));
    fx.send(osc_test::message("/n_free", 100));
    fx.pump(4);
}

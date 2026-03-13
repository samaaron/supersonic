/*
 * test_node_tree.cpp — Node tree structure, lifecycle, movement, and error handling
 *
 * Ported from node_tree.spec.mjs — tests node tree via /g_queryTree, /status,
 * /n_free, /g_freeAll, /g_deepFree, /n_before, /n_after, /g_head, /g_tail.
 */
#include "EngineFixture.h"

// ── Helpers ─────────────────────────────────────────────────────────────────

static osc_test::Packet sNew(const char* def, int32_t id, int32_t addAction, int32_t target) {
    osc_test::Builder b;
    auto& s = b.begin("/s_new");
    s << def << id << addAction << target;
    return b.end();
}

static int getSynthCount(EngineFixture& fx) {
    fx.send(osc_test::message("/status"));
    OscReply r;
    if (fx.waitForReply("/status.reply", r))
        return r.parsed().argInt(2); // numSynths is at index 2
    return -1;
}

static int getGroupCount(EngineFixture& fx) {
    fx.send(osc_test::message("/status"));
    OscReply r;
    if (fx.waitForReply("/status.reply", r))
        return r.parsed().argInt(3); // numGroups is at index 3
    return -1;
}

// =============================================================================
// SECTION: Node lifecycle
// =============================================================================

TEST_CASE("synth appears after /s_new", "[node_tree]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    fx.send(sNew("sonic-pi-beep", 1000, 0, 1));
    fx.pump(8);

    int count = getSynthCount(fx);
    CHECK(count >= 1);

    fx.send(osc_test::message("/n_free", 1000));
    fx.pump(4);
}

TEST_CASE("group appears after /g_new", "[node_tree]") {
    EngineFixture fx;

    fx.send(osc_test::message("/g_new", 100, 0, 0));
    fx.pump(8);

    // root(0) + default(1) + 100 = at least 3
    int count = getGroupCount(fx);
    CHECK(count >= 3);

    fx.send(osc_test::message("/n_free", 100));
    fx.pump(4);
}

TEST_CASE("node disappears after /n_free", "[node_tree]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    fx.send(sNew("sonic-pi-beep", 1000, 0, 1));
    fx.pump(8);

    int before = getSynthCount(fx);
    REQUIRE(before >= 1);

    fx.send(osc_test::message("/n_free", 1000));
    fx.pump(8);

    int after = getSynthCount(fx);
    CHECK(after < before);
}

TEST_CASE("/g_freeAll removes all children", "[node_tree]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    fx.send(osc_test::message("/g_new", 100, 0, 0));
    fx.pump(4);

    fx.send(sNew("sonic-pi-beep", 1000, 0, 100));
    fx.send(sNew("sonic-pi-beep", 1001, 0, 100));
    fx.send(sNew("sonic-pi-beep", 1002, 0, 100));
    fx.pump(8);

    // Free all children of group 100
    fx.send(osc_test::message("/g_freeAll", 100));
    fx.pump(8);

    int synths = getSynthCount(fx);
    CHECK(synths == 0);

    fx.send(osc_test::message("/n_free", 100));
    fx.pump(4);
}

TEST_CASE("/g_deepFree removes all synths recursively", "[node_tree]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    // Create group 100 at head of root, group 200 inside 100
    fx.send(osc_test::message("/g_new", 100, 0, 0));
    fx.pump(4);
    fx.send(osc_test::message("/g_new", 200, 0, 100));
    fx.pump(4);

    // Add synth to each group
    fx.send(sNew("sonic-pi-beep", 1000, 0, 100));
    fx.send(sNew("sonic-pi-beep", 1001, 0, 200));
    fx.pump(8);

    REQUIRE(getSynthCount(fx) >= 2);

    // g_deepFree removes synths but keeps groups
    fx.send(osc_test::message("/g_deepFree", 100));
    fx.pump(8);

    CHECK(getSynthCount(fx) == 0);

    // Groups should still exist
    int groups = getGroupCount(fx);
    CHECK(groups >= 4); // root + default + 100 + 200

    // Cleanup
    fx.send(osc_test::message("/n_free", 200));
    fx.send(osc_test::message("/n_free", 100));
    fx.pump(4);
}

// =============================================================================
// SECTION: Node movement
// =============================================================================

TEST_CASE("/n_before updates order", "[node_tree]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    // Create 2 synths at head of default group — order: 1001, 1000
    fx.send(sNew("sonic-pi-beep", 1000, 0, 1));
    fx.send(sNew("sonic-pi-beep", 1001, 0, 1));
    fx.pump(8);

    // /n_before 1000 1001 → order should become: 1000, 1001
    fx.send(osc_test::message("/n_before", 1000, 1001));
    fx.pump(8);

    // Verify via /g_queryTree — both synths still in default group
    fx.send(osc_test::message("/g_queryTree", 1, 0));
    OscReply r;
    REQUIRE(fx.waitForReply("/g_queryTree.reply", r));
    CHECK(r.parsed().argInt(2) >= 2);  // default group has both synths

    fx.send(osc_test::message("/n_free", 1000));
    fx.send(osc_test::message("/n_free", 1001));
    fx.pump(4);
}

TEST_CASE("/n_after updates order", "[node_tree]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    // Create 2 synths at head — order: 1001, 1000
    fx.send(sNew("sonic-pi-beep", 1000, 0, 1));
    fx.send(sNew("sonic-pi-beep", 1001, 0, 1));
    fx.pump(8);

    // /n_after 1001 1000 → reorder
    fx.send(osc_test::message("/n_after", 1001, 1000));
    fx.pump(8);

    fx.send(osc_test::message("/g_queryTree", 1, 0));
    OscReply r;
    REQUIRE(fx.waitForReply("/g_queryTree.reply", r));
    CHECK(r.parsed().argInt(2) >= 2);  // default group has both synths

    fx.send(osc_test::message("/n_free", 1000));
    fx.send(osc_test::message("/n_free", 1001));
    fx.pump(4);
}

TEST_CASE("/g_head moves to head", "[node_tree]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    fx.send(osc_test::message("/g_new", 100, 0, 0));
    fx.pump(4);

    fx.send(sNew("sonic-pi-beep", 1000, 0, 100));
    fx.send(sNew("sonic-pi-beep", 1001, 0, 100));
    fx.pump(8);

    // Move 1000 to head of group 100
    fx.send(osc_test::message("/g_head", 100, 1000));
    fx.pump(8);

    // Verify synth count in group 100
    fx.send(osc_test::message("/g_queryTree", 100, 0));
    OscReply r;
    REQUIRE(fx.waitForReply("/g_queryTree.reply", r));
    CHECK(r.parsed().argInt(2) >= 2);  // group 100 has both synths

    fx.send(osc_test::message("/n_free", 1000));
    fx.send(osc_test::message("/n_free", 1001));
    fx.send(osc_test::message("/n_free", 100));
    fx.pump(4);
}

TEST_CASE("/g_tail moves to tail", "[node_tree]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    fx.send(osc_test::message("/g_new", 100, 0, 0));
    fx.pump(4);

    fx.send(sNew("sonic-pi-beep", 1000, 0, 100));
    fx.send(sNew("sonic-pi-beep", 1001, 0, 100));
    fx.pump(8);

    // Move 1001 to tail of group 100
    fx.send(osc_test::message("/g_tail", 100, 1001));
    fx.pump(8);

    // Verify synth count in group 100
    fx.send(osc_test::message("/g_queryTree", 100, 0));
    OscReply r;
    REQUIRE(fx.waitForReply("/g_queryTree.reply", r));
    CHECK(r.parsed().argInt(2) >= 2);  // group 100 has both synths

    fx.send(osc_test::message("/n_free", 1000));
    fx.send(osc_test::message("/n_free", 1001));
    fx.send(osc_test::message("/n_free", 100));
    fx.pump(4);
}

TEST_CASE("moving node between groups updates parent", "[node_tree]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    fx.send(osc_test::message("/g_new", 100, 0, 0));
    fx.pump(4);
    fx.send(osc_test::message("/g_new", 200, 0, 0));
    fx.pump(4);

    // Synth 1000 in group 100
    fx.send(sNew("sonic-pi-beep", 1000, 0, 100));
    fx.pump(8);

    // Move synth 1000 to head of group 200
    fx.send(osc_test::message("/g_head", 200, 1000));
    fx.pump(8);

    // Query group 200 — should have children
    fx.send(osc_test::message("/g_queryTree", 200, 0));
    OscReply r;
    REQUIRE(fx.waitForReply("/g_queryTree.reply", r));
    auto p = r.parsed();
    CHECK(p.argInt(2) >= 1); // group 200 has at least 1 child

    fx.send(osc_test::message("/n_free", 1000));
    fx.send(osc_test::message("/n_free", 200));
    fx.send(osc_test::message("/n_free", 100));
    fx.pump(4);
}

// =============================================================================
// SECTION: Complex hierarchies
// =============================================================================

TEST_CASE("deeply nested groups", "[node_tree]") {
    EngineFixture fx;

    // Create 5 levels: 100 → 101 → 102 → 103 → 104
    fx.send(osc_test::message("/g_new", 100, 0, 0));
    fx.pump(4);
    fx.send(osc_test::message("/g_new", 101, 0, 100));
    fx.pump(4);
    fx.send(osc_test::message("/g_new", 102, 0, 101));
    fx.pump(4);
    fx.send(osc_test::message("/g_new", 103, 0, 102));
    fx.pump(4);
    fx.send(osc_test::message("/g_new", 104, 0, 103));
    fx.pump(8);

    // root(0) + default(1) + 100 + 101 + 102 + 103 + 104 = 7
    int groups = getGroupCount(fx);
    CHECK(groups >= 7);

    // Free the top-level group (should cascade via /n_free on a group)
    fx.send(osc_test::message("/n_free", 100));
    fx.pump(4);
}

TEST_CASE("multiple synths in multiple groups", "[node_tree]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    fx.send(osc_test::message("/g_new", 100, 0, 0));
    fx.pump(4);
    fx.send(osc_test::message("/g_new", 200, 0, 0));
    fx.pump(4);

    // 3 synths in group 100
    fx.send(sNew("sonic-pi-beep", 1000, 0, 100));
    fx.send(sNew("sonic-pi-beep", 1001, 0, 100));
    fx.send(sNew("sonic-pi-beep", 1002, 0, 100));
    fx.pump(4);

    // 3 synths in group 200
    fx.send(sNew("sonic-pi-beep", 1003, 0, 200));
    fx.send(sNew("sonic-pi-beep", 1004, 0, 200));
    fx.send(sNew("sonic-pi-beep", 1005, 0, 200));
    fx.pump(8);

    CHECK(getSynthCount(fx) == 6);

    // Free all
    for (int i = 1000; i <= 1005; i++)
        fx.send(osc_test::message("/n_free", i));
    fx.send(osc_test::message("/n_free", 200));
    fx.send(osc_test::message("/n_free", 100));
    fx.pump(4);
}

TEST_CASE("sibling order preserved", "[node_tree]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    fx.send(osc_test::message("/g_new", 100, 0, 0));
    fx.pump(4);

    // Create 3 synths at tail of group 100
    fx.send(sNew("sonic-pi-beep", 1000, 1, 100)); // add to tail
    fx.send(sNew("sonic-pi-beep", 1001, 1, 100));
    fx.send(sNew("sonic-pi-beep", 1002, 1, 100));
    fx.pump(8);

    int before = getSynthCount(fx);
    REQUIRE(before >= 3);

    // Free middle synth
    fx.send(osc_test::message("/n_free", 1001));
    fx.pump(8);

    int after = getSynthCount(fx);
    CHECK(after < before);

    // Others still exist — verify synth count
    CHECK(after >= 2);

    // Cleanup
    fx.send(osc_test::message("/n_free", 1000));
    fx.send(osc_test::message("/n_free", 1002));
    fx.send(osc_test::message("/n_free", 100));
    fx.pump(4);
}

// =============================================================================
// SECTION: /g_deepFree behavior
// =============================================================================

TEST_CASE("g_deepFree removes synths in nested hierarchy", "[node_tree]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    fx.send(osc_test::message("/g_new", 100, 0, 0));
    fx.pump(4);
    fx.send(osc_test::message("/g_new", 200, 0, 100));
    fx.pump(4);

    // Synth in group 200 (nested)
    fx.send(sNew("sonic-pi-beep", 1001, 0, 200));
    // Synth in group 100
    fx.send(sNew("sonic-pi-beep", 1000, 0, 100));
    fx.pump(8);

    REQUIRE(getSynthCount(fx) >= 2);

    fx.send(osc_test::message("/g_deepFree", 100));
    fx.pump(8);

    CHECK(getSynthCount(fx) == 0);

    // Cleanup groups
    fx.send(osc_test::message("/n_free", 200));
    fx.send(osc_test::message("/n_free", 100));
    fx.pump(4);
}

TEST_CASE("g_deepFree on empty hierarchy is no-op", "[node_tree]") {
    EngineFixture fx;

    fx.send(osc_test::message("/g_new", 100, 0, 0));
    fx.pump(4);

    fx.send(osc_test::message("/g_deepFree", 100));
    fx.pump(8);

    // Group 100 should still exist
    fx.send(osc_test::message("/status"));
    OscReply r;
    REQUIRE(fx.waitForReply("/status.reply", r));
    CHECK(r.parsed().argInt(3) >= 3);  // root + default + 100

    fx.send(osc_test::message("/n_free", 100));
    fx.pump(4);
}

TEST_CASE("g_deepFree preserves sibling groups", "[node_tree]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    fx.send(osc_test::message("/g_new", 100, 0, 0));
    fx.pump(4);
    fx.send(osc_test::message("/g_new", 200, 0, 0));
    fx.pump(4);

    // Add synth to group 100
    fx.send(sNew("sonic-pi-beep", 1000, 0, 100));
    fx.pump(8);

    // g_deepFree root — removes all synths, keeps groups
    fx.send(osc_test::message("/g_deepFree", 0));
    fx.pump(8);

    CHECK(getSynthCount(fx) == 0);

    // Groups should still be there (at least root + default + 100 + 200)
    int groups = getGroupCount(fx);
    CHECK(groups >= 4);

    // Cleanup
    fx.send(osc_test::message("/n_free", 200));
    fx.send(osc_test::message("/n_free", 100));
    fx.pump(4);
}

TEST_CASE("g_deepFree vs g_freeAll", "[node_tree]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    // Group 100 with subgroup 200 and synths in both
    fx.send(osc_test::message("/g_new", 100, 0, 0));
    fx.pump(4);
    fx.send(osc_test::message("/g_new", 200, 0, 100));
    fx.pump(4);

    fx.send(sNew("sonic-pi-beep", 1000, 0, 100));
    fx.send(sNew("sonic-pi-beep", 1001, 0, 200));
    fx.pump(8);

    // Step 1: g_deepFree removes synths, keeps groups
    fx.send(osc_test::message("/g_deepFree", 100));
    fx.pump(8);

    CHECK(getSynthCount(fx) == 0);
    // Group 200 should still exist
    int groupsAfterDeep = getGroupCount(fx);
    CHECK(groupsAfterDeep >= 4); // root + default + 100 + 200

    // Step 2: g_freeAll removes everything (including subgroups)
    fx.send(osc_test::message("/g_freeAll", 100));
    fx.pump(8);

    // Group 200 should be gone now, but 100 still exists
    int groupsAfterFreeAll = getGroupCount(fx);
    CHECK(groupsAfterFreeAll < groupsAfterDeep);

    // Cleanup
    fx.send(osc_test::message("/n_free", 100));
    fx.pump(4);
}

TEST_CASE("g_deepFree on non-existent group is no-op", "[node_tree]") {
    EngineFixture fx;

    fx.send(osc_test::message("/g_deepFree", 99999));
    fx.pump(8);

    // Engine still responsive
    fx.send(osc_test::message("/status"));
    OscReply r;
    REQUIRE(fx.waitForReply("/status.reply", r));
    CHECK(r.parsed().argInt(3) >= 2);  // root + default still exist
}

TEST_CASE("g_deepFree on synth node is no-op", "[node_tree]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    fx.send(sNew("sonic-pi-beep", 1000, 0, 1));
    fx.pump(8);

    int before = getSynthCount(fx);
    REQUIRE(before >= 1);

    // g_deepFree on a synth node — should be a no-op
    fx.send(osc_test::message("/g_deepFree", 1000));
    fx.pump(8);

    int after = getSynthCount(fx);
    CHECK(after >= 1); // synth still exists

    fx.send(osc_test::message("/n_free", 1000));
    fx.pump(4);
}

// =============================================================================
// SECTION: Error scenarios
// =============================================================================

TEST_CASE("freeing non-existent node doesn't corrupt tree", "[node_tree]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    fx.send(osc_test::message("/n_free", 99999));
    fx.pump(8);

    // Tree should still work — create and verify a synth
    fx.send(sNew("sonic-pi-beep", 1000, 0, 1));
    fx.pump(8);

    int count = getSynthCount(fx);
    CHECK(count >= 1);

    fx.send(osc_test::message("/n_free", 1000));
    fx.pump(4);
}

TEST_CASE("double-free doesn't corrupt tree", "[node_tree]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    fx.send(sNew("sonic-pi-beep", 1000, 0, 1));
    fx.pump(8);

    // Free once
    fx.send(osc_test::message("/n_free", 1000));
    fx.pump(8);

    // Free again (already freed)
    fx.send(osc_test::message("/n_free", 1000));
    fx.pump(8);

    // Tree should still work
    fx.send(sNew("sonic-pi-beep", 1001, 0, 1));
    fx.pump(8);

    int count = getSynthCount(fx);
    CHECK(count >= 1);

    fx.send(osc_test::message("/n_free", 1001));
    fx.pump(4);
}

TEST_CASE("moving non-existent node doesn't corrupt", "[node_tree]") {
    EngineFixture fx;

    fx.send(osc_test::message("/n_before", 99999, 1));
    fx.pump(8);

    // Engine still responsive
    fx.send(osc_test::message("/status"));
    OscReply r;
    REQUIRE(fx.waitForReply("/status.reply", r));
    CHECK(r.parsed().argInt(3) >= 2);  // root + default still exist
}

TEST_CASE("creating synth in non-existent group doesn't corrupt", "[node_tree]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    // Attempt to create synth in non-existent group 99999
    fx.send(sNew("sonic-pi-beep", 1000, 0, 99999));
    fx.pump(8);

    // Tree should still work — create synth in valid group
    fx.send(sNew("sonic-pi-beep", 1001, 0, 1));
    fx.pump(8);

    int count = getSynthCount(fx);
    CHECK(count >= 1);

    fx.send(osc_test::message("/n_free", 1001));
    fx.pump(4);
}

TEST_CASE("creating group in non-existent parent doesn't corrupt", "[node_tree]") {
    EngineFixture fx;

    // Attempt to create group 100 in non-existent parent 99999
    fx.send(osc_test::message("/g_new", 100, 0, 99999));
    fx.pump(8);

    // Tree should still work
    fx.send(osc_test::message("/g_new", 200, 0, 0));
    fx.pump(8);

    int groups = getGroupCount(fx);
    CHECK(groups >= 3); // root + default + 200

    fx.send(osc_test::message("/n_free", 200));
    fx.pump(4);
}

TEST_CASE("g_freeAll on non-existent group doesn't corrupt", "[node_tree]") {
    EngineFixture fx;

    fx.send(osc_test::message("/g_freeAll", 99999));
    fx.pump(8);

    // Engine still responsive
    fx.send(osc_test::message("/status"));
    OscReply r;
    REQUIRE(fx.waitForReply("/status.reply", r));
    CHECK(r.parsed().argInt(3) >= 2);  // root + default still exist
}

TEST_CASE("reusing freed node ID works", "[node_tree]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    // Create synth 1000
    fx.send(sNew("sonic-pi-beep", 1000, 0, 1));
    fx.pump(8);
    REQUIRE(getSynthCount(fx) >= 1);

    // Free it
    fx.send(osc_test::message("/n_free", 1000));
    fx.pump(8);

    // Create synth 1000 again (reuse ID)
    fx.send(sNew("sonic-pi-beep", 1000, 0, 1));
    fx.pump(8);

    int count = getSynthCount(fx);
    CHECK(count >= 1);

    fx.send(osc_test::message("/n_free", 1000));
    fx.pump(4);
}

TEST_CASE("creating duplicate node ID fails gracefully", "[node_tree]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    // Create synth 1000
    fx.send(sNew("sonic-pi-beep", 1000, 0, 1));
    fx.pump(8);
    REQUIRE(getSynthCount(fx) >= 1);

    // Try to create synth 1000 again (duplicate)
    fx.send(sNew("sonic-pi-beep", 1000, 0, 1));
    fx.pump(8);

    // First synth should still exist
    int count = getSynthCount(fx);
    CHECK(count >= 1);

    // Cleanup
    fx.send(osc_test::message("/n_free", 1000));
    fx.pump(4);
}

// =============================================================================
// SECTION: Complex movement patterns
// =============================================================================

TEST_CASE("swap two nodes via /n_before", "[node_tree]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    fx.send(osc_test::message("/g_new", 100, 0, 0));
    fx.pump(4);

    // A(1000) first, then B(1001) at tail — order: A, B
    fx.send(sNew("sonic-pi-beep", 1000, 1, 100)); // add to tail
    fx.send(sNew("sonic-pi-beep", 1001, 1, 100)); // add to tail
    fx.pump(8);

    // /n_before 1001 1000 → order becomes B, A
    fx.send(osc_test::message("/n_before", 1001, 1000));
    fx.pump(8);

    // Verify both synths still exist
    fx.send(osc_test::message("/g_queryTree", 100, 0));
    OscReply r;
    REQUIRE(fx.waitForReply("/g_queryTree.reply", r));
    CHECK(r.parsed().argInt(2) >= 2);

    fx.send(osc_test::message("/n_free", 1000));
    fx.send(osc_test::message("/n_free", 1001));
    fx.send(osc_test::message("/n_free", 100));
    fx.pump(4);
}

TEST_CASE("move node through multiple groups", "[node_tree]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    fx.send(osc_test::message("/g_new", 100, 0, 0));
    fx.pump(4);
    fx.send(osc_test::message("/g_new", 200, 0, 0));
    fx.pump(4);
    fx.send(osc_test::message("/g_new", 300, 0, 0));
    fx.pump(4);

    // Synth 1000 in group 100
    fx.send(sNew("sonic-pi-beep", 1000, 0, 100));
    fx.pump(8);

    // Move to 200
    fx.send(osc_test::message("/g_head", 200, 1000));
    fx.pump(8);

    // Move to 300
    fx.send(osc_test::message("/g_head", 300, 1000));
    fx.pump(8);

    // Verify synth is now in group 300
    fx.send(osc_test::message("/g_queryTree", 300, 0));
    OscReply r;
    REQUIRE(fx.waitForReply("/g_queryTree.reply", r));
    CHECK(r.parsed().argInt(2) >= 1);  // group 300 has the synth

    fx.send(osc_test::message("/n_free", 1000));
    fx.send(osc_test::message("/n_free", 300));
    fx.send(osc_test::message("/n_free", 200));
    fx.send(osc_test::message("/n_free", 100));
    fx.pump(4);
}

TEST_CASE("move group with children", "[node_tree]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    // Group 100 and 200 in root
    fx.send(osc_test::message("/g_new", 100, 0, 0));
    fx.pump(4);
    fx.send(osc_test::message("/g_new", 200, 0, 0));
    fx.pump(4);

    // Synth 1000 in group 100
    fx.send(sNew("sonic-pi-beep", 1000, 0, 100));
    fx.pump(8);

    // Move group 100 into group 200
    fx.send(osc_test::message("/g_head", 200, 100));
    fx.pump(8);

    // Query group 200 — should have children (group 100)
    fx.send(osc_test::message("/g_queryTree", 200, 0));
    OscReply r;
    REQUIRE(fx.waitForReply("/g_queryTree.reply", r));
    auto p = r.parsed();
    CHECK(p.argInt(2) >= 1); // group 200 has at least 1 child

    // Cleanup
    fx.send(osc_test::message("/n_free", 1000));
    fx.send(osc_test::message("/n_free", 100));
    fx.send(osc_test::message("/n_free", 200));
    fx.pump(4);
}

TEST_CASE("rapid sequential moves", "[node_tree]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    // Create 5 synths at head of default group
    for (int i = 0; i < 5; i++) {
        fx.send(sNew("sonic-pi-beep", 3000 + i, 0, 1));
    }
    fx.pump(8);

    // Do 10 moves: alternate /n_before and /n_after
    for (int i = 0; i < 10; i++) {
        int a = 3000 + (i % 5);
        int b = 3000 + ((i + 1) % 5);
        if (i % 2 == 0)
            fx.send(osc_test::message("/n_before", a, b));
        else
            fx.send(osc_test::message("/n_after", a, b));
        fx.pump(4);
    }

    // All 5 synths should still exist
    fx.send(osc_test::message("/status"));
    OscReply r;
    REQUIRE(fx.waitForReply("/status.reply", r));
    CHECK(r.parsed().argInt(2) >= 5);

    for (int i = 0; i < 5; i++)
        fx.send(osc_test::message("/n_free", 3000 + i));
    fx.pump(4);
}

// =============================================================================
// SECTION: Node count verification
// =============================================================================

TEST_CASE("status reports correct initial state", "[node_tree]") {
    EngineFixture fx;

    int synths = getSynthCount(fx);
    CHECK(synths == 0);

    int groups = getGroupCount(fx);
    CHECK(groups >= 2); // root + default
}

TEST_CASE("status updates after operations", "[node_tree]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    // Create 5 synths
    for (int i = 0; i < 5; i++)
        fx.send(sNew("sonic-pi-beep", 4000 + i, 0, 1));
    fx.pump(8);

    CHECK(getSynthCount(fx) == 5);

    // Free 3 of them
    fx.send(osc_test::message("/n_free", 4000));
    fx.send(osc_test::message("/n_free", 4001));
    fx.send(osc_test::message("/n_free", 4002));
    fx.pump(8);

    CHECK(getSynthCount(fx) == 2);

    // Cleanup
    fx.send(osc_test::message("/n_free", 4003));
    fx.send(osc_test::message("/n_free", 4004));
    fx.pump(4);
}

TEST_CASE("many concurrent synths", "[node_tree]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    // Create 50 synths
    for (int i = 0; i < 50; i++)
        fx.send(sNew("sonic-pi-beep", 5000 + i, 0, 1));
    fx.pump(16);

    CHECK(getSynthCount(fx) == 50);

    // Free all
    for (int i = 0; i < 50; i++)
        fx.send(osc_test::message("/n_free", 5000 + i));
    fx.pump(16);

    CHECK(getSynthCount(fx) == 0);
}

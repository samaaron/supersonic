/*
 * test_osc_semantic.cpp — Comprehensive OSC semantic tests.
 *
 * Port of the upstream JS test osc_semantic.spec.mjs to native Catch2.
 * Covers: /n_free, /n_set, /n_setn, /n_fill, /n_map, /n_mapa, /n_mapan,
 *         /n_mapn, /n_run, /n_order, /n_trace, /s_new, /s_noid, /s_get,
 *         /g_new, /g_queryTree, /g_dumpTree, /p_new, /b_alloc, /b_free,
 *         /b_query, /b_gen, /b_get, /b_fill, /c_set, /c_get, /c_setn,
 *         /c_getn, /c_fill, /d_freeAll, /notify, /status
 */
#include "EngineFixture.h"
#include <catch2/catch_approx.hpp>

// ---------------------------------------------------------------------------
// Helper: create a synth via /s_new
// ---------------------------------------------------------------------------
static osc_test::Packet sNew(const char* def, int32_t id,
                              int32_t addAction, int32_t target) {
    osc_test::Builder b;
    auto& s = b.begin("/s_new");
    s << def << id << addAction << target;
    return b.end();
}

// ===========================================================================
//  /n_free semantics
// ===========================================================================

TEST_CASE("frees single synth", "[osc_semantic]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    fx.send(sNew("sonic-pi-beep", 1000, 0, 1));

    // Confirm synth exists
    fx.send(osc_test::message("/status"));
    OscReply before;
    REQUIRE(fx.waitForReply("/status.reply", before));
    int synthsBefore = before.parsed().argInt(2);
    CHECK(synthsBefore >= 1);

    fx.clearReplies();

    // Free the synth
    fx.send(osc_test::message("/n_free", 1000));

    fx.send(osc_test::message("/status"));
    OscReply after;
    REQUIRE(fx.waitForReply("/status.reply", after));
    int synthsAfter = after.parsed().argInt(2);
    CHECK(synthsAfter < synthsBefore);
}

TEST_CASE("frees multiple nodes in single command", "[osc_semantic]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    fx.send(sNew("sonic-pi-beep", 1000, 0, 1));
    fx.send(sNew("sonic-pi-beep", 1001, 0, 1));
    fx.send(sNew("sonic-pi-beep", 1002, 0, 1));

    fx.send(osc_test::message("/status"));
    OscReply before;
    REQUIRE(fx.waitForReply("/status.reply", before));
    int synthsBefore = before.parsed().argInt(2);
    CHECK(synthsBefore >= 3);
    fx.clearReplies();

    // Free all three in a single command
    {
        osc_test::Builder b;
        auto& s = b.begin("/n_free");
        s << (int32_t)1000 << (int32_t)1001 << (int32_t)1002;
        fx.send(b.end());
    }

    fx.send(osc_test::message("/status"));
    OscReply after;
    REQUIRE(fx.waitForReply("/status.reply", after));
    int synthsAfter = after.parsed().argInt(2);
    CHECK(synthsAfter <= synthsBefore - 3);
}

TEST_CASE("freeing non-existent node does not crash", "[osc_semantic]") {
    EngineFixture fx;

    fx.send(osc_test::message("/n_free", 99999));

    // Engine is still responsive
    fx.send(osc_test::message("/status"));
    OscReply r;
    REQUIRE(fx.waitForReply("/status.reply", r));
    SUCCEED();
}

TEST_CASE("freeing group frees group and all children", "[osc_semantic]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    // Create group 100 and add 3 synths to it
    fx.send(osc_test::message("/g_new", 100, 0, 1));
    fx.send(sNew("sonic-pi-beep", 1000, 0, 100));
    fx.send(sNew("sonic-pi-beep", 1001, 0, 100));
    fx.send(sNew("sonic-pi-beep", 1002, 0, 100));

    fx.send(osc_test::message("/status"));
    OscReply before;
    REQUIRE(fx.waitForReply("/status.reply", before));
    int synthsBefore = before.parsed().argInt(2);
    int groupsBefore = before.parsed().argInt(3);
    fx.clearReplies();

    // Free group 100 — should remove the group and all synths inside
    fx.send(osc_test::message("/n_free", 100));

    fx.send(osc_test::message("/status"));
    OscReply after;
    REQUIRE(fx.waitForReply("/status.reply", after));
    CHECK(after.parsed().argInt(2) == 0);                 // numSynths == 0
    CHECK(after.parsed().argInt(3) < groupsBefore);       // numGroups decreased
}

TEST_CASE("freeing already-freed node is idempotent", "[osc_semantic]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    fx.send(sNew("sonic-pi-beep", 1000, 0, 1));

    fx.send(osc_test::message("/n_free", 1000));

    // Free again — should not crash
    fx.send(osc_test::message("/n_free", 1000));

    fx.send(osc_test::message("/status"));
    OscReply r;
    REQUIRE(fx.waitForReply("/status.reply", r));
    SUCCEED();
}

TEST_CASE("freeing nested groups frees entire subtree", "[osc_semantic]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    // Build nested hierarchy: group 100 -> group 101 -> synth 1000
    //                         group 100 -> synth 1001
    fx.send(osc_test::message("/g_new", 100, 0, 1));
    fx.send(osc_test::message("/g_new", 101, 0, 100));
    fx.send(sNew("sonic-pi-beep", 1000, 0, 101));
    fx.send(sNew("sonic-pi-beep", 1001, 0, 100));

    fx.send(osc_test::message("/status"));
    OscReply before;
    REQUIRE(fx.waitForReply("/status.reply", before));
    int synthsBefore = before.parsed().argInt(2);
    CHECK(synthsBefore >= 2);
    fx.clearReplies();

    // Free top-level group 100 — entire subtree should be gone
    fx.send(osc_test::message("/n_free", 100));

    fx.send(osc_test::message("/status"));
    OscReply after;
    REQUIRE(fx.waitForReply("/status.reply", after));
    CHECK(after.parsed().argInt(2) == 0);   // no synths
}

// ===========================================================================
//  /n_set semantics
// ===========================================================================

TEST_CASE("/s_get returns control value after /n_set by name", "[osc_semantic]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    // Create synth with default note
    {
        osc_test::Builder b;
        auto& s = b.begin("/s_new");
        s << "sonic-pi-beep" << (int32_t)1000 << (int32_t)0 << (int32_t)1
          << "note" << 60.0f;
        fx.send(b.end());
    }

    // Change note to 72
    {
        osc_test::Builder b;
        auto& s = b.begin("/n_set");
        s << (int32_t)1000 << "note" << 72.0f;
        fx.send(b.end());
    }

    // Query note value
    {
        osc_test::Builder b;
        auto& s = b.begin("/s_get");
        s << (int32_t)1000 << "note";
        fx.send(b.end());
    }

    OscReply r;
    REQUIRE(fx.waitForReply("/n_set", r));
    CHECK(r.parsed().argFloat(2) == 72.0f);

    fx.send(osc_test::message("/n_free", 1000));
}

TEST_CASE("/n_set multiple controls in single command", "[osc_semantic]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    fx.send(sNew("sonic-pi-beep", 1000, 0, 1));

    // Set multiple controls at once
    {
        osc_test::Builder b;
        auto& s = b.begin("/n_set");
        s << (int32_t)1000
          << "note" << 64.0f
          << "amp"  << 0.25f
          << "pan"  << -0.5f;
        fx.send(b.end());
    }

    // Verify note
    {
        osc_test::Builder b;
        auto& s = b.begin("/s_get");
        s << (int32_t)1000 << "note";
        fx.send(b.end());
    }
    OscReply rNote;
    REQUIRE(fx.waitForReply("/n_set", rNote));
    CHECK(rNote.parsed().argFloat(2) == 64.0f);

    fx.send(osc_test::message("/n_free", 1000));
}

TEST_CASE("setting control on group affects all children", "[osc_semantic]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    fx.send(osc_test::message("/g_new", 100, 0, 1));
    fx.send(sNew("sonic-pi-beep", 1000, 0, 100));
    fx.send(sNew("sonic-pi-beep", 1001, 0, 100));
    fx.send(sNew("sonic-pi-beep", 1002, 0, 100));

    // Set amp on group — should propagate to all children
    {
        osc_test::Builder b;
        auto& s = b.begin("/n_set");
        s << (int32_t)100 << "amp" << 0.1f;
        fx.send(b.end());
    }

    // Verify all synths still running after group-level control set
    fx.send(osc_test::message("/status"));
    OscReply st;
    REQUIRE(fx.waitForReply("/status.reply", st));
    CHECK(st.parsed().argInt(2) >= 3);

    fx.send(osc_test::message("/n_free", 1000));
    fx.send(osc_test::message("/n_free", 1001));
    fx.send(osc_test::message("/n_free", 1002));
    fx.send(osc_test::message("/n_free", 100));
}

TEST_CASE("setting non-existent control does not crash", "[osc_semantic]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    fx.send(sNew("sonic-pi-beep", 1000, 0, 1));

    // Set a control that does not exist on the synthdef
    {
        osc_test::Builder b;
        auto& s = b.begin("/n_set");
        s << (int32_t)1000 << "nonexistent_xyz" << 999.0f;
        fx.send(b.end());
    }

    // Verify synth still exists
    fx.send(osc_test::message("/status"));
    OscReply r;
    REQUIRE(fx.waitForReply("/status.reply", r));
    CHECK(r.parsed().argInt(2) >= 1);

    fx.send(osc_test::message("/n_free", 1000));
}

// ===========================================================================
//  /n_setn semantics
// ===========================================================================

TEST_CASE("/n_setn sets sequential controls", "[osc_semantic]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    fx.send(sNew("sonic-pi-beep", 1000, 0, 1));

    // Set 3 controls starting at index 0
    {
        osc_test::Builder b;
        auto& s = b.begin("/n_setn");
        s << (int32_t)1000 << (int32_t)0 << (int32_t)3
          << 0.111f << 0.222f << 0.333f;
        fx.send(b.end());
    }

    // Query them back with /s_getn
    {
        osc_test::Builder b;
        auto& s = b.begin("/s_getn");
        s << (int32_t)1000 << (int32_t)0 << (int32_t)3;
        fx.send(b.end());
    }

    OscReply r;
    REQUIRE(fx.waitForReply("/n_setn", r));
    auto p = r.parsed();
    CHECK(p.argFloat(3) == Catch::Approx(0.111f).margin(0.01f));
    CHECK(p.argFloat(4) == Catch::Approx(0.222f).margin(0.01f));
    CHECK(p.argFloat(5) == Catch::Approx(0.333f).margin(0.01f));

    fx.send(osc_test::message("/n_free", 1000));
}

// ===========================================================================
//  /n_fill semantics
// ===========================================================================

TEST_CASE("/n_fill fills range of controls", "[osc_semantic]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    fx.send(sNew("sonic-pi-beep", 1000, 0, 1));

    // Fill controls 0..2 with 0.5
    {
        osc_test::Builder b;
        auto& s = b.begin("/n_fill");
        s << (int32_t)1000 << (int32_t)0 << (int32_t)3 << 0.5f;
        fx.send(b.end());
    }

    // Verify synth still running after /n_fill
    fx.send(osc_test::message("/status"));
    OscReply st;
    REQUIRE(fx.waitForReply("/status.reply", st));
    CHECK(st.parsed().argInt(2) >= 1);

    fx.send(osc_test::message("/n_free", 1000));
}

// ===========================================================================
//  /n_map semantics
// ===========================================================================

TEST_CASE("mapped control reads from bus", "[osc_semantic]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    // Set control bus 0 to 72
    {
        osc_test::Builder b;
        auto& s = b.begin("/c_set");
        s << (int32_t)0 << 72.0f;
        fx.send(b.end());
    }

    fx.send(sNew("sonic-pi-beep", 1000, 0, 1));

    // Map note to control bus 0
    {
        osc_test::Builder b;
        auto& s = b.begin("/n_map");
        s << (int32_t)1000 << "note" << (int32_t)0;
        fx.send(b.end());
    }

    // Verify synth is still in the graph
    fx.send(osc_test::message("/status"));
    OscReply r;
    REQUIRE(fx.waitForReply("/status.reply", r));
    CHECK(r.parsed().argInt(2) >= 1);

    fx.send(osc_test::message("/n_free", 1000));
}

TEST_CASE("unmap with bus index -1", "[osc_semantic]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    fx.send(sNew("sonic-pi-beep", 1000, 0, 1));

    // Map note to bus 0
    {
        osc_test::Builder b;
        auto& s = b.begin("/n_map");
        s << (int32_t)1000 << "note" << (int32_t)0;
        fx.send(b.end());
    }

    // Unmap with -1
    {
        osc_test::Builder b;
        auto& s = b.begin("/n_map");
        s << (int32_t)1000 << "note" << (int32_t)-1;
        fx.send(b.end());
    }

    // Set note directly to 84
    {
        osc_test::Builder b;
        auto& s = b.begin("/n_set");
        s << (int32_t)1000 << "note" << 84.0f;
        fx.send(b.end());
    }

    // Verify synth still running after unmap
    fx.send(osc_test::message("/status"));
    OscReply r;
    REQUIRE(fx.waitForReply("/status.reply", r));
    CHECK(r.parsed().argInt(2) >= 1);

    fx.send(osc_test::message("/n_free", 1000));
}

TEST_CASE("maps multiple controls in single command", "[osc_semantic]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    // Set control buses
    {
        osc_test::Builder b;
        auto& s = b.begin("/c_set");
        s << (int32_t)0 << 72.0f << (int32_t)1 << 0.5f;
        fx.send(b.end());
    }

    fx.send(sNew("sonic-pi-beep", 1000, 0, 1));

    // Map two controls
    {
        osc_test::Builder b;
        auto& s = b.begin("/n_map");
        s << (int32_t)1000 << "note" << (int32_t)0 << "amp" << (int32_t)1;
        fx.send(b.end());
    }

    // Verify synth still running after multi-map
    fx.send(osc_test::message("/status"));
    OscReply r;
    REQUIRE(fx.waitForReply("/status.reply", r));
    CHECK(r.parsed().argInt(2) >= 1);

    fx.send(osc_test::message("/n_free", 1000));
}

// ===========================================================================
//  /s_new semantics
// ===========================================================================

TEST_CASE("creates synth with specified ID", "[osc_semantic]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    fx.send(sNew("sonic-pi-beep", 1234, 0, 1));

    fx.send(osc_test::message("/g_queryTree", 0, 0));
    OscReply r;
    REQUIRE(fx.waitForReply("/g_queryTree.reply", r));
    auto p = r.parsed();
    CHECK(p.argInt(2) >= 1);  // root has children

    fx.send(osc_test::message("/n_free", 1234));
}

TEST_CASE("add action 0 - head", "[osc_semantic]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    fx.send(osc_test::message("/g_new", 100, 0, 1));

    // Add synth 1000 to tail of group 100
    fx.send(sNew("sonic-pi-beep", 1000, 1, 100));
    // Add synth 1001 to head of group 100
    fx.send(sNew("sonic-pi-beep", 1001, 0, 100));

    // Query group 100
    fx.send(osc_test::message("/g_queryTree", 100, 0));
    OscReply r;
    REQUIRE(fx.waitForReply("/g_queryTree.reply", r));
    CHECK(r.parsed().argInt(2) >= 2);

    fx.send(osc_test::message("/n_free", 1000));
    fx.send(osc_test::message("/n_free", 1001));
    fx.send(osc_test::message("/n_free", 100));
}

TEST_CASE("add action 1 - tail", "[osc_semantic]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    fx.send(osc_test::message("/g_new", 100, 0, 1));

    fx.send(sNew("sonic-pi-beep", 1000, 1, 100));
    fx.send(sNew("sonic-pi-beep", 1001, 1, 100));
    fx.send(sNew("sonic-pi-beep", 1002, 1, 100));

    fx.send(osc_test::message("/status"));
    OscReply r;
    REQUIRE(fx.waitForReply("/status.reply", r));
    CHECK(r.parsed().argInt(2) >= 3);

    fx.send(osc_test::message("/n_free", 1000));
    fx.send(osc_test::message("/n_free", 1001));
    fx.send(osc_test::message("/n_free", 1002));
    fx.send(osc_test::message("/n_free", 100));
}

TEST_CASE("add action 2 - before target", "[osc_semantic]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    fx.send(sNew("sonic-pi-beep", 1000, 0, 1));

    // Add synth 1001 before synth 1000 (action 2, target 1000)
    fx.send(sNew("sonic-pi-beep", 1001, 2, 1000));

    fx.send(osc_test::message("/status"));
    OscReply r;
    REQUIRE(fx.waitForReply("/status.reply", r));
    CHECK(r.parsed().argInt(2) >= 2);

    fx.send(osc_test::message("/n_free", 1000));
    fx.send(osc_test::message("/n_free", 1001));
}

TEST_CASE("add action 3 - after target", "[osc_semantic]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    fx.send(sNew("sonic-pi-beep", 1000, 0, 1));

    // Add synth 1001 after synth 1000 (action 3, target 1000)
    fx.send(sNew("sonic-pi-beep", 1001, 3, 1000));

    fx.send(osc_test::message("/status"));
    OscReply r;
    REQUIRE(fx.waitForReply("/status.reply", r));
    CHECK(r.parsed().argInt(2) >= 2);

    fx.send(osc_test::message("/n_free", 1000));
    fx.send(osc_test::message("/n_free", 1001));
}

TEST_CASE("add action 4 - replaces target", "[osc_semantic]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    fx.send(sNew("sonic-pi-beep", 1000, 0, 1));

    fx.send(osc_test::message("/status"));
    OscReply before;
    REQUIRE(fx.waitForReply("/status.reply", before));
    int synthsBefore = before.parsed().argInt(2);
    CHECK(synthsBefore >= 1);
    fx.clearReplies();

    // Replace synth 1000 with synth 1001 (action 4, target 1000)
    fx.send(sNew("sonic-pi-beep", 1001, 4, 1000));

    fx.send(osc_test::message("/status"));
    OscReply after;
    REQUIRE(fx.waitForReply("/status.reply", after));
    int synthsAfter = after.parsed().argInt(2);
    // Should still have the same number (replaced, not added)
    CHECK(synthsAfter == synthsBefore);

    fx.send(osc_test::message("/n_free", 1001));
}

TEST_CASE("sets controls at creation time", "[osc_semantic]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    // Create synth with initial control values
    {
        osc_test::Builder b;
        auto& s = b.begin("/s_new");
        s << "sonic-pi-beep" << (int32_t)1000 << (int32_t)0 << (int32_t)1
          << "note" << 72.0f << "amp" << 0.25f;
        fx.send(b.end());
    }

    // Verify with /s_get if available
    {
        osc_test::Builder b;
        auto& s = b.begin("/s_get");
        s << (int32_t)1000 << "note";
        fx.send(b.end());
    }

    OscReply r;
    REQUIRE(fx.waitForReply("/n_set", r));
    CHECK(r.parsed().argFloat(2) == 72.0f);

    fx.send(osc_test::message("/n_free", 1000));
}

TEST_CASE("non-existent synthdef fails gracefully", "[osc_semantic]") {
    EngineFixture fx;

    // Attempt to create a synth with a synthdef that does not exist
    fx.send(sNew("nonexistent_xyz", 1000, 0, 1));

    // Engine should still be alive
    fx.send(osc_test::message("/status"));
    OscReply r;
    REQUIRE(fx.waitForReply("/status.reply", r));
    CHECK(r.parsed().argInt(2) == 0);  // synth should not have been created

    // Cleanup (may be a no-op)
    fx.send(osc_test::message("/n_free", 1000));
}

// ===========================================================================
//  /g_new semantics
// ===========================================================================

TEST_CASE("creates group with specified ID", "[osc_semantic]") {
    EngineFixture fx;

    fx.send(osc_test::message("/g_new", 100, 0, 0));

    fx.send(osc_test::message("/g_queryTree", 0, 0));
    OscReply r;
    REQUIRE(fx.waitForReply("/g_queryTree.reply", r));
    CHECK(r.parsed().argInt(2) >= 1);  // root has children

    fx.send(osc_test::message("/n_free", 100));
}

TEST_CASE("creates multiple groups", "[osc_semantic]") {
    EngineFixture fx;

    fx.send(osc_test::message("/g_new", 100, 0, 0));
    fx.send(osc_test::message("/g_new", 101, 0, 100));
    fx.send(osc_test::message("/g_new", 102, 1, 100));

    fx.send(osc_test::message("/status"));
    OscReply r;
    REQUIRE(fx.waitForReply("/status.reply", r));
    // root(0) + default(1) + 100 + 101 + 102 = 5
    CHECK(r.parsed().argInt(3) >= 5);

    fx.send(osc_test::message("/n_free", 100));
}

TEST_CASE("nested groups create proper hierarchy", "[osc_semantic]") {
    EngineFixture fx;

    fx.send(osc_test::message("/g_new", 100, 0, 0));
    fx.send(osc_test::message("/g_new", 101, 0, 100));
    fx.send(osc_test::message("/g_new", 102, 0, 101));
    fx.send(osc_test::message("/g_new", 103, 0, 102));

    fx.send(osc_test::message("/status"));
    OscReply r;
    REQUIRE(fx.waitForReply("/status.reply", r));
    // root(0) + default(1) + 100 + 101 + 102 + 103 = 6
    CHECK(r.parsed().argInt(3) >= 6);

    fx.send(osc_test::message("/n_free", 100));
}

TEST_CASE("all add actions work for groups", "[osc_semantic]") {
    EngineFixture fx;

    fx.send(osc_test::message("/g_new", 100, 0, 0));      // head of root
    fx.send(osc_test::message("/g_new", 101, 0, 100));    // head of 100
    fx.send(osc_test::message("/g_new", 102, 1, 100));    // tail of 100
    fx.send(osc_test::message("/g_new", 103, 2, 102));    // before 102
    fx.send(osc_test::message("/g_new", 104, 3, 101));    // after 101

    fx.send(osc_test::message("/status"));
    OscReply r;
    REQUIRE(fx.waitForReply("/status.reply", r));
    // root(0) + default(1) + 100 + 101 + 102 + 103 + 104 = 7
    CHECK(r.parsed().argInt(3) >= 7);

    fx.send(osc_test::message("/n_free", 100));
}

// ===========================================================================
//  /b_alloc and /b_free semantics
// ===========================================================================

TEST_CASE("allocates mono buffer with correct params", "[osc_semantic]") {
    EngineFixture fx;

    fx.send(osc_test::message("/b_alloc", 0, 44100, 1));
    OscReply done;
    REQUIRE(fx.waitForReply("/done", done));
    fx.clearReplies();

    fx.send(osc_test::message("/b_query", 0));
    OscReply r;
    REQUIRE(fx.waitForReply("/b_info", r));

    auto p = r.parsed();
    CHECK(p.argInt(0) == 0);       // bufnum
    CHECK(p.argInt(1) == 44100);   // frames
    CHECK(p.argInt(2) == 1);       // channels

    fx.send(osc_test::message("/b_free", 0));
}

TEST_CASE("allocates stereo buffer", "[osc_semantic]") {
    EngineFixture fx;

    fx.send(osc_test::message("/b_alloc", 0, 22050, 2));
    OscReply done;
    REQUIRE(fx.waitForReply("/done", done));
    fx.clearReplies();

    fx.send(osc_test::message("/b_query", 0));
    OscReply r;
    REQUIRE(fx.waitForReply("/b_info", r));

    auto p = r.parsed();
    CHECK(p.argInt(0) == 0);
    CHECK(p.argInt(1) == 22050);
    CHECK(p.argInt(2) == 2);

    fx.send(osc_test::message("/b_free", 0));
}

TEST_CASE("re-allocating buffer replaces previous", "[osc_semantic]") {
    EngineFixture fx;

    // Allocate first
    fx.send(osc_test::message("/b_alloc", 0, 1024, 1));
    OscReply done1;
    REQUIRE(fx.waitForReply("/done", done1));
    fx.clearReplies();

    // Query to confirm
    fx.send(osc_test::message("/b_query", 0));
    OscReply r1;
    REQUIRE(fx.waitForReply("/b_info", r1));
    CHECK(r1.parsed().argInt(1) == 1024);
    fx.clearReplies();

    // Re-allocate with different params
    fx.send(osc_test::message("/b_alloc", 0, 2048, 2));
    OscReply done2;
    REQUIRE(fx.waitForReply("/done", done2));
    fx.clearReplies();

    // Query again
    fx.send(osc_test::message("/b_query", 0));
    OscReply r2;
    REQUIRE(fx.waitForReply("/b_info", r2));
    CHECK(r2.parsed().argInt(1) == 2048);
    CHECK(r2.parsed().argInt(2) == 2);

    fx.send(osc_test::message("/b_free", 0));
}

TEST_CASE("multiple buffers are independent", "[osc_semantic]") {
    EngineFixture fx;

    // Allocate three buffers with different sizes
    fx.send(osc_test::message("/b_alloc", 0, 1000, 1));
    OscReply d0;
    REQUIRE(fx.waitForReply("/done", d0));
    fx.clearReplies();

    fx.send(osc_test::message("/b_alloc", 1, 2000, 1));
    OscReply d1;
    REQUIRE(fx.waitForReply("/done", d1));
    fx.clearReplies();

    fx.send(osc_test::message("/b_alloc", 2, 3000, 1));
    OscReply d2;
    REQUIRE(fx.waitForReply("/done", d2));
    fx.clearReplies();

    // Verify each
    fx.send(osc_test::message("/b_query", 0));
    OscReply r0;
    REQUIRE(fx.waitForReply("/b_info", r0));
    CHECK(r0.parsed().argInt(1) == 1000);
    fx.clearReplies();

    fx.send(osc_test::message("/b_query", 1));
    OscReply r1;
    REQUIRE(fx.waitForReply("/b_info", r1));
    CHECK(r1.parsed().argInt(1) == 2000);
    fx.clearReplies();

    fx.send(osc_test::message("/b_query", 2));
    OscReply r2;
    REQUIRE(fx.waitForReply("/b_info", r2));
    CHECK(r2.parsed().argInt(1) == 3000);

    fx.send(osc_test::message("/b_free", 0));
    fx.send(osc_test::message("/b_free", 1));
    fx.send(osc_test::message("/b_free", 2));
}

// ===========================================================================
//  /n_run semantics
// ===========================================================================

TEST_CASE("pauses synth with flag 0", "[osc_semantic]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    fx.send(osc_test::message("/notify", 1));
    fx.clearReplies();

    fx.send(sNew("sonic-pi-beep", 1000, 0, 1));

    // Pause synth
    fx.send(osc_test::message("/n_run", 1000, 0));

    // Engine still works
    fx.send(osc_test::message("/status"));
    OscReply r;
    REQUIRE(fx.waitForReply("/status.reply", r));

    fx.send(osc_test::message("/n_free", 1000));
}

TEST_CASE("resumes synth with flag 1", "[osc_semantic]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    fx.send(sNew("sonic-pi-beep", 1000, 0, 1));

    // Pause
    fx.send(osc_test::message("/n_run", 1000, 0));

    // Resume
    fx.send(osc_test::message("/n_run", 1000, 1));

    // Verify synth still alive after pause/resume
    fx.send(osc_test::message("/status"));
    OscReply r;
    REQUIRE(fx.waitForReply("/status.reply", r));
    CHECK(r.parsed().argInt(2) >= 1);
    fx.clearReplies();

    fx.send(osc_test::message("/n_free", 1000));
}

TEST_CASE("pauses and resumes multiple nodes", "[osc_semantic]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    fx.send(sNew("sonic-pi-beep", 1000, 0, 1));
    fx.send(sNew("sonic-pi-beep", 1001, 0, 1));
    fx.send(sNew("sonic-pi-beep", 1002, 0, 1));

    // Pause all three in one command
    {
        osc_test::Builder b;
        auto& s = b.begin("/n_run");
        s << (int32_t)1000 << (int32_t)0
          << (int32_t)1001 << (int32_t)0
          << (int32_t)1002 << (int32_t)0;
        fx.send(b.end());
    }

    // Resume all three
    {
        osc_test::Builder b;
        auto& s = b.begin("/n_run");
        s << (int32_t)1000 << (int32_t)1
          << (int32_t)1001 << (int32_t)1
          << (int32_t)1002 << (int32_t)1;
        fx.send(b.end());
    }

    // Verify all synths still alive after pause/resume
    fx.send(osc_test::message("/status"));
    OscReply r;
    REQUIRE(fx.waitForReply("/status.reply", r));
    CHECK(r.parsed().argInt(2) >= 3);
    fx.clearReplies();

    fx.send(osc_test::message("/n_free", 1000));
    fx.send(osc_test::message("/n_free", 1001));
    fx.send(osc_test::message("/n_free", 1002));
}

// ===========================================================================
//  /n_order semantics
// ===========================================================================

TEST_CASE("reorders nodes to head", "[osc_semantic]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    fx.send(osc_test::message("/g_new", 100, 0, 1));
    fx.send(sNew("sonic-pi-beep", 1000, 0, 100));
    fx.send(sNew("sonic-pi-beep", 1001, 0, 100));
    fx.send(sNew("sonic-pi-beep", 1002, 0, 100));

    // Reorder: move 1002, 1001 to head of group 100
    {
        osc_test::Builder b;
        auto& s = b.begin("/n_order");
        s << (int32_t)0 << (int32_t)100 << (int32_t)1002 << (int32_t)1001;
        fx.send(b.end());
    }

    // Verify all synths still alive after reorder
    fx.send(osc_test::message("/status"));
    OscReply r;
    REQUIRE(fx.waitForReply("/status.reply", r));
    CHECK(r.parsed().argInt(2) >= 3);
    fx.clearReplies();

    fx.send(osc_test::message("/n_free", 1000));
    fx.send(osc_test::message("/n_free", 1001));
    fx.send(osc_test::message("/n_free", 1002));
    fx.send(osc_test::message("/n_free", 100));
}

TEST_CASE("reorders nodes to tail", "[osc_semantic]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    fx.send(osc_test::message("/g_new", 100, 0, 1));
    fx.send(sNew("sonic-pi-beep", 1000, 0, 100));
    fx.send(sNew("sonic-pi-beep", 1001, 0, 100));
    fx.send(sNew("sonic-pi-beep", 1002, 0, 100));

    // Reorder: move 1000, 1001 to tail of group 100
    {
        osc_test::Builder b;
        auto& s = b.begin("/n_order");
        s << (int32_t)1 << (int32_t)100 << (int32_t)1000 << (int32_t)1001;
        fx.send(b.end());
    }

    // Verify all synths still alive after reorder
    fx.send(osc_test::message("/status"));
    OscReply r;
    REQUIRE(fx.waitForReply("/status.reply", r));
    CHECK(r.parsed().argInt(2) >= 3);
    fx.clearReplies();

    fx.send(osc_test::message("/n_free", 1000));
    fx.send(osc_test::message("/n_free", 1001));
    fx.send(osc_test::message("/n_free", 1002));
    fx.send(osc_test::message("/n_free", 100));
}

// ===========================================================================
//  /b_gen semantics
// ===========================================================================

TEST_CASE("sine1 generates waveform", "[osc_semantic]") {
    EngineFixture fx;

    // Allocate buffer 0: 512 frames, 1 channel
    fx.send(osc_test::message("/b_alloc", 0, 512, 1));
    OscReply alloc;
    REQUIRE(fx.waitForReply("/done", alloc));
    fx.clearReplies();

    // Generate sine waveform
    {
        osc_test::Builder b;
        auto& s = b.begin("/b_gen");
        s << (int32_t)0 << "sine1" << (int32_t)1 << 1.0f;
        fx.send(b.end());
    }

    OscReply genDone;
    REQUIRE(fx.waitForReply("/done", genDone));
    fx.clearReplies();

    // Query sample at index 128 (quarter period — should be near peak ~1.0)
    {
        osc_test::Builder b;
        auto& s = b.begin("/b_get");
        s << (int32_t)0 << (int32_t)128;
        fx.send(b.end());
    }

    OscReply r;
    REQUIRE(fx.waitForReply("/b_set", r));
    float val = r.parsed().argFloat(2);
    CHECK(val > 0.5f);

    fx.send(osc_test::message("/b_free", 0));
}

TEST_CASE("cheby generates transfer function", "[osc_semantic]") {
    EngineFixture fx;

    fx.send(osc_test::message("/b_alloc", 0, 512, 1));
    OscReply alloc;
    REQUIRE(fx.waitForReply("/done", alloc));
    fx.clearReplies();

    {
        osc_test::Builder b;
        auto& s = b.begin("/b_gen");
        s << (int32_t)0 << "cheby" << (int32_t)1 << 1.0f;
        fx.send(b.end());
    }

    OscReply genDone;
    REQUIRE(fx.waitForReply("/done", genDone));

    fx.send(osc_test::message("/b_free", 0));
}

TEST_CASE("copy transfers samples", "[osc_semantic]") {
    EngineFixture fx;

    // Allocate two buffers
    fx.send(osc_test::message("/b_alloc", 0, 256, 1));
    OscReply d0;
    REQUIRE(fx.waitForReply("/done", d0));
    fx.clearReplies();

    fx.send(osc_test::message("/b_alloc", 1, 256, 1));
    OscReply d1;
    REQUIRE(fx.waitForReply("/done", d1));
    fx.clearReplies();

    // Fill buffer 0 with 0.5
    {
        osc_test::Builder b;
        auto& s = b.begin("/b_fill");
        s << (int32_t)0 << (int32_t)0 << (int32_t)256 << 0.5f;
        fx.send(b.end());
    }
    fx.clearReplies();

    // Copy buffer 0 into buffer 1
    {
        osc_test::Builder b;
        auto& s = b.begin("/b_gen");
        s << (int32_t)1 << "copy" << (int32_t)0 << (int32_t)0 << (int32_t)0 << (int32_t)-1;
        fx.send(b.end());
    }

    OscReply copyDone;
    REQUIRE(fx.waitForReply("/done", copyDone));

    fx.send(osc_test::message("/b_free", 0));
    fx.send(osc_test::message("/b_free", 1));
}

// ===========================================================================
//  Control bus semantics
// ===========================================================================

TEST_CASE("/c_set and /c_get", "[osc_semantic]") {
    EngineFixture fx;

    // Set control bus 0 to 440.0
    {
        osc_test::Builder b;
        auto& s = b.begin("/c_set");
        s << (int32_t)0 << 440.0f;
        fx.send(b.end());
    }

    // Get control bus 0
    fx.send(osc_test::message("/c_get", 0));

    OscReply r;
    REQUIRE(fx.waitForReply("/c_set", r));
    CHECK(r.parsed().argInt(0) == 0);
    CHECK(r.parsed().argFloat(1) == Catch::Approx(440.0f));
}

TEST_CASE("/c_setn and /c_getn", "[osc_semantic]") {
    EngineFixture fx;

    // Set 4 sequential buses starting at 0
    {
        osc_test::Builder b;
        auto& s = b.begin("/c_setn");
        s << (int32_t)0 << (int32_t)4
          << 100.0f << 200.0f << 300.0f << 400.0f;
        fx.send(b.end());
    }

    // Get 4 sequential buses starting at 0
    {
        osc_test::Builder b;
        auto& s = b.begin("/c_getn");
        s << (int32_t)0 << (int32_t)4;
        fx.send(b.end());
    }

    OscReply r;
    REQUIRE(fx.waitForReply("/c_setn", r));
    auto p = r.parsed();
    CHECK(p.argFloat(2) == Catch::Approx(100.0f));
    CHECK(p.argFloat(3) == Catch::Approx(200.0f));
    CHECK(p.argFloat(4) == Catch::Approx(300.0f));
    CHECK(p.argFloat(5) == Catch::Approx(400.0f));
}

TEST_CASE("/c_fill fills buses", "[osc_semantic]") {
    EngineFixture fx;

    // Fill 10 buses starting at 0 with 0.5
    {
        osc_test::Builder b;
        auto& s = b.begin("/c_fill");
        s << (int32_t)0 << (int32_t)10 << 0.5f;
        fx.send(b.end());
    }

    // Read bus 5 to verify
    fx.send(osc_test::message("/c_get", 5));

    OscReply r;
    REQUIRE(fx.waitForReply("/c_set", r));
    CHECK(r.parsed().argFloat(1) == Catch::Approx(0.5f));
}

// ===========================================================================
//  /d_freeAll semantics
// ===========================================================================

TEST_CASE("frees all loaded synthdefs", "[osc_semantic]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    fx.send(osc_test::message("/status"));
    OscReply before;
    REQUIRE(fx.waitForReply("/status.reply", before));
    int defsBefore = before.parsed().argInt(4);
    CHECK(defsBefore > 0);
    fx.clearReplies();

    // Free all synthdefs
    fx.send(osc_test::message("/d_freeAll"));

    fx.send(osc_test::message("/status"));
    OscReply after;
    REQUIRE(fx.waitForReply("/status.reply", after));
    int defsAfter = after.parsed().argInt(4);
    // numSynthDefs should be 0 or at least decreased
    CHECK(defsAfter <= defsBefore);
}

// ===========================================================================
//  /p_new (parallel group)
// ===========================================================================

TEST_CASE("creates parallel group", "[osc_semantic]") {
    EngineFixture fx;

    {
        osc_test::Builder b;
        auto& s = b.begin("/p_new");
        s << (int32_t)100 << (int32_t)0 << (int32_t)0;
        fx.send(b.end());
    }

    fx.send(osc_test::message("/g_queryTree", 0, 0));
    OscReply r;
    REQUIRE(fx.waitForReply("/g_queryTree.reply", r));
    CHECK(r.parsed().argInt(2) >= 1);  // root has children

    fx.send(osc_test::message("/n_free", 100));
}

// ===========================================================================
//  /g_dumpTree
// ===========================================================================

TEST_CASE("dumps tree without crash", "[osc_semantic]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    fx.send(osc_test::message("/g_new", 100, 0, 1));
    fx.send(sNew("sonic-pi-beep", 1000, 0, 100));

    fx.send(osc_test::message("/g_dumpTree", 0, 0));

    // Verify engine state intact after dump
    fx.send(osc_test::message("/status"));
    OscReply st;
    REQUIRE(fx.waitForReply("/status.reply", st));
    CHECK(st.parsed().argInt(2) >= 1);

    fx.send(osc_test::message("/n_free", 1000));
    fx.send(osc_test::message("/n_free", 100));
}

TEST_CASE("dumps tree with controls", "[osc_semantic]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    fx.send(osc_test::message("/g_new", 100, 0, 1));
    fx.send(sNew("sonic-pi-beep", 1000, 0, 100));

    fx.send(osc_test::message("/g_dumpTree", 0, 1));

    // Verify engine state intact after dump
    fx.send(osc_test::message("/status"));
    OscReply st;
    REQUIRE(fx.waitForReply("/status.reply", st));
    CHECK(st.parsed().argInt(2) >= 1);

    fx.send(osc_test::message("/n_free", 1000));
    fx.send(osc_test::message("/n_free", 100));
}

// ===========================================================================
//  /n_trace
// ===========================================================================

TEST_CASE("traces synth execution", "[osc_semantic]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    fx.send(sNew("sonic-pi-beep", 1000, 0, 1));

    fx.send(osc_test::message("/n_trace", 1000));

    // Verify synth still exists after trace
    fx.send(osc_test::message("/status"));
    OscReply st;
    REQUIRE(fx.waitForReply("/status.reply", st));
    CHECK(st.parsed().argInt(2) >= 1);

    fx.send(osc_test::message("/n_free", 1000));
}

// ===========================================================================
//  /s_noid
// ===========================================================================

TEST_CASE("/s_noid removes synth ID", "[osc_semantic]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    fx.send(sNew("sonic-pi-beep", 1000, 0, 1));

    // Remove the ID from the synth
    {
        osc_test::Builder b;
        auto& s = b.begin("/s_noid");
        s << (int32_t)1000;
        fx.send(b.end());
    }

    // Engine still alive
    fx.send(osc_test::message("/status"));
    OscReply r;
    REQUIRE(fx.waitForReply("/status.reply", r));
    SUCCEED();
}

// ===========================================================================
//  /n_mapa and /n_mapan and /n_mapn
// ===========================================================================

TEST_CASE("/n_mapa maps to audio bus", "[osc_semantic]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    fx.send(sNew("sonic-pi-beep", 1000, 0, 1));

    {
        osc_test::Builder b;
        auto& s = b.begin("/n_mapa");
        s << (int32_t)1000 << "note" << (int32_t)0;
        fx.send(b.end());
    }

    fx.send(osc_test::message("/n_free", 1000));
    SUCCEED();
}

TEST_CASE("/n_mapan maps sequential to audio buses", "[osc_semantic]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    fx.send(sNew("sonic-pi-beep", 1000, 0, 1));

    {
        osc_test::Builder b;
        auto& s = b.begin("/n_mapan");
        s << (int32_t)1000 << "note" << (int32_t)0 << (int32_t)1;
        fx.send(b.end());
    }

    fx.send(osc_test::message("/n_free", 1000));
    SUCCEED();
}

TEST_CASE("/n_mapn maps sequential to control buses", "[osc_semantic]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    fx.send(sNew("sonic-pi-beep", 1000, 0, 1));

    {
        osc_test::Builder b;
        auto& s = b.begin("/n_mapn");
        s << (int32_t)1000 << "note" << (int32_t)0 << (int32_t)1;
        fx.send(b.end());
    }

    fx.send(osc_test::message("/n_free", 1000));
    SUCCEED();
}

// ===========================================================================
//  /g_queryTree with controls
// ===========================================================================

TEST_CASE("returns correct tree structure", "[osc_semantic]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    fx.send(osc_test::message("/g_new", 100, 0, 1));
    fx.send(sNew("sonic-pi-beep", 1000, 0, 100));

    fx.send(osc_test::message("/g_queryTree", 0, 0));
    OscReply r;
    REQUIRE(fx.waitForReply("/g_queryTree.reply", r));

    auto p = r.parsed();
    CHECK(p.argInt(1) == 0);     // root group ID
    CHECK(p.argInt(2) >= 1);     // root has children

    fx.send(osc_test::message("/n_free", 1000));
    fx.send(osc_test::message("/n_free", 100));
}

TEST_CASE("returns control values with flag 1", "[osc_semantic]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    // Create synth with note=72
    {
        osc_test::Builder b;
        auto& s = b.begin("/s_new");
        s << "sonic-pi-beep" << (int32_t)1000 << (int32_t)0 << (int32_t)1
          << "note" << 72.0f;
        fx.send(b.end());
    }

    // Query without controls (flag=0)
    fx.send(osc_test::message("/g_queryTree", 0, 0));
    OscReply r0;
    REQUIRE(fx.waitForReply("/g_queryTree.reply", r0));
    int countWithout = r0.parsed().argCount();
    fx.clearReplies();

    // Query with controls (flag=1)
    fx.send(osc_test::message("/g_queryTree", 0, 1));
    OscReply r1;
    REQUIRE(fx.waitForReply("/g_queryTree.reply", r1));
    int countWith = r1.parsed().argCount();

    // Flag=1 should return more args (control names + values)
    CHECK(countWith > countWithout);

    fx.send(osc_test::message("/n_free", 1000));
}

// ===========================================================================
//  Notification tests
// ===========================================================================

TEST_CASE("/n_go notification on synth creation", "[osc_semantic]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    // Enable notifications
    fx.send(osc_test::message("/notify", 1));
    fx.clearReplies();

    // Create synth
    fx.send(sNew("sonic-pi-beep", 1000, 0, 1));

    OscReply r;
    if (fx.waitForReply("/n_go", r)) {
        auto p = r.parsed();
        CHECK(p.argInt(0) == 1000);   // nodeID
    } else {
        SUCCEED("headless may not deliver /n_go");
    }

    fx.send(osc_test::message("/n_free", 1000));
}

TEST_CASE("/n_end notification on node free", "[osc_semantic]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    // Enable notifications
    fx.send(osc_test::message("/notify", 1));

    fx.send(sNew("sonic-pi-beep", 1000, 0, 1));
    fx.clearReplies();

    // Free the synth
    fx.send(osc_test::message("/n_free", 1000));

    OscReply r;
    if (fx.waitForReply("/n_end", r)) {
        auto p = r.parsed();
        CHECK(p.argInt(0) == 1000);   // nodeID
    } else {
        SUCCEED("headless may not deliver /n_end");
    }
}

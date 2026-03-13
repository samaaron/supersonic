/*
 * test_synth_commands.cpp — /s_get, /s_getn, /p_new, /s_noid and advanced
 *                            synth operations not covered in other test files.
 */
#include "EngineFixture.h"
#include <catch2/catch_approx.hpp>

static osc_test::Packet sNew(const char* def, int32_t id, int32_t addAction, int32_t target) {
    osc_test::Builder b;
    auto& s = b.begin("/s_new");
    s << def << id << addAction << target;
    return b.end();
}

static osc_test::Packet sNewWithNote(const char* def, int32_t id, int32_t addAction,
                                      int32_t target, float note) {
    osc_test::Builder b;
    auto& s = b.begin("/s_new");
    s << def << id << addAction << target << "note" << note;
    return b.end();
}

// =============================================================================
// /s_get — GET SYNTH CONTROL VALUE
// =============================================================================

TEST_CASE("/s_get returns control value set at creation", "[synth_cmd]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    fx.send(osc_test::message("/notify", 1));
    fx.pump(4);
    fx.clearReplies();

    // Create synth with note=60
    {
        osc_test::Builder b;
        auto& s = b.begin("/s_new");
        s << "sonic-pi-beep" << (int32_t)1000 << (int32_t)0 << (int32_t)1
          << "note" << 60.0f << "release" << 60.0f;
        fx.send(b.end());
    }
    fx.pump(8);
    fx.clearReplies();

    // Query note value
    {
        osc_test::Builder b;
        auto& s = b.begin("/s_get");
        s << (int32_t)1000 << "note";
        fx.send(b.end());
    }

    OscReply r;
    REQUIRE(fx.waitForReply("/n_set", r));
    auto p = r.parsed();
    CHECK(p.argInt(0) == 1000);
    CHECK(p.argString(1) == "note");
    CHECK(p.argFloat(2) == Catch::Approx(60.0f).margin(0.01f));

    fx.send(osc_test::message("/n_free", 1000));
    fx.pump(4);
}

TEST_CASE("/s_get after /n_set reflects new value", "[synth_cmd]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    fx.send(osc_test::message("/notify", 1));
    fx.pump(4);
    fx.clearReplies();

    fx.send(sNewWithNote("sonic-pi-beep", 1000, 0, 1, 60.0f));
    fx.pump(8);

    // Set note to 72
    {
        osc_test::Builder b;
        auto& s = b.begin("/n_set");
        s << (int32_t)1000 << "note" << 72.0f;
        fx.send(b.end());
    }
    fx.pump(8);
    fx.clearReplies();

    // Get note
    {
        osc_test::Builder b;
        auto& s = b.begin("/s_get");
        s << (int32_t)1000 << "note";
        fx.send(b.end());
    }

    OscReply r;
    REQUIRE(fx.waitForReply("/n_set", r));
    CHECK(r.parsed().argFloat(2) == Catch::Approx(72.0f).margin(0.01f));

    fx.send(osc_test::message("/n_free", 1000));
    fx.pump(4);
}

TEST_CASE("/s_get by control index", "[synth_cmd]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    fx.send(osc_test::message("/notify", 1));
    fx.pump(4);
    fx.clearReplies();

    fx.send(sNew("sonic-pi-beep", 1000, 0, 1));
    fx.pump(8);
    fx.clearReplies();

    // Get control at index 0
    {
        osc_test::Builder b;
        auto& s = b.begin("/s_get");
        s << (int32_t)1000 << (int32_t)0;
        fx.send(b.end());
    }

    OscReply r;
    REQUIRE(fx.waitForReply("/n_set", r));
    CHECK(r.parsed().argInt(0) == 1000);

    fx.send(osc_test::message("/n_free", 1000));
    fx.pump(4);
}

// =============================================================================
// /s_getn — GET SEQUENTIAL CONTROL VALUES
// =============================================================================

TEST_CASE("/s_getn returns sequential control values", "[synth_cmd]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    fx.send(osc_test::message("/notify", 1));
    fx.pump(4);
    fx.clearReplies();

    fx.send(sNew("sonic-pi-beep", 1000, 0, 1));
    fx.pump(8);
    fx.clearReplies();

    // Get first 3 controls
    {
        osc_test::Builder b;
        auto& s = b.begin("/s_getn");
        s << (int32_t)1000 << (int32_t)0 << (int32_t)3;
        fx.send(b.end());
    }

    OscReply r;
    REQUIRE(fx.waitForReply("/n_setn", r));
    auto p = r.parsed();
    CHECK(p.argInt(0) == 1000);
    CHECK(p.argInt(1) == 0);
    CHECK(p.argInt(2) == 3);

    fx.send(osc_test::message("/n_free", 1000));
    fx.pump(4);
}

// =============================================================================
// /p_new — PARALLEL GROUP
// =============================================================================

TEST_CASE("/p_new creates parallel group", "[synth_cmd]") {
    EngineFixture fx;

    fx.send(osc_test::message("/p_new", 100, 0, 0));
    fx.pump(8);

    // Query tree — root should have our parallel group
    fx.send(osc_test::message("/g_queryTree", 0, 0));
    OscReply r;
    REQUIRE(fx.waitForReply("/g_queryTree.reply", r));

    auto p = r.parsed();
    CHECK(p.argInt(2) >= 1);  // root has children

    fx.send(osc_test::message("/n_free", 100));
    fx.pump(4);
}

TEST_CASE("/p_new parallel group holds synths", "[synth_cmd]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    // Create parallel group
    fx.send(osc_test::message("/p_new", 100, 0, 0));
    fx.pump(4);

    // Add synths to it
    fx.send(sNew("sonic-pi-beep", 1000, 0, 100));
    fx.send(sNew("sonic-pi-beep", 1001, 0, 100));
    fx.pump(8);

    fx.send(osc_test::message("/status"));
    OscReply r;
    REQUIRE(fx.waitForReply("/status.reply", r));
    CHECK(r.parsed().argInt(2) >= 2);  // numSynths >= 2

    fx.send(osc_test::message("/n_free", 100));
    fx.pump(4);
}

// =============================================================================
// /s_noid — REMOVE SYNTH NODE ID
// =============================================================================

TEST_CASE("/s_noid removes synth node ID", "[synth_cmd]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    fx.send(sNew("sonic-pi-beep", 1000, 0, 1));
    fx.pump(8);

    // Remove the node ID
    {
        osc_test::Builder b;
        auto& s = b.begin("/s_noid");
        s << (int32_t)1000;
        fx.send(b.end());
    }
    fx.pump(8);

    // Synth should still be running (just without a node ID)
    fx.send(osc_test::message("/status"));
    OscReply r;
    REQUIRE(fx.waitForReply("/status.reply", r));
    CHECK(r.parsed().argInt(2) >= 1);  // synth still running
}

// =============================================================================
// /n_setn — SET SEQUENTIAL NODE CONTROLS
// =============================================================================

TEST_CASE("/n_setn sets sequential controls by index", "[synth_cmd]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    fx.send(sNew("sonic-pi-beep", 1000, 0, 1));
    fx.pump(8);

    // Set controls 0,1,2 to specific values
    {
        osc_test::Builder b;
        auto& s = b.begin("/n_setn");
        s << (int32_t)1000 << (int32_t)0 << (int32_t)3
          << 0.111f << 0.222f << 0.333f;
        fx.send(b.end());
    }
    fx.pump(8);

    // Verify with /s_getn
    fx.clearReplies();
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
    fx.pump(4);
}

TEST_CASE("/n_setn sets controls by name", "[synth_cmd]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    fx.send(sNew("sonic-pi-beep", 1000, 0, 1));
    fx.pump(8);

    // Set 2 controls starting from "note"
    {
        osc_test::Builder b;
        auto& s = b.begin("/n_setn");
        s << (int32_t)1000 << "note" << (int32_t)2 << 72.0f << 0.75f;
        fx.send(b.end());
    }
    fx.pump(8);

    // Verify note was set
    fx.clearReplies();
    {
        osc_test::Builder b;
        auto& s = b.begin("/s_get");
        s << (int32_t)1000 << "note";
        fx.send(b.end());
    }

    OscReply r;
    REQUIRE(fx.waitForReply("/n_set", r));
    CHECK(r.parsed().argFloat(2) == Catch::Approx(72.0f).margin(0.01f));

    fx.send(osc_test::message("/n_free", 1000));
    fx.pump(4);
}

// =============================================================================
// /n_fill — FILL NODE CONTROLS
// =============================================================================

TEST_CASE("/n_fill fills range of controls", "[synth_cmd]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    fx.send(sNew("sonic-pi-beep", 1000, 0, 1));
    fx.pump(8);

    // Fill controls 0-2 with 0.5
    {
        osc_test::Builder b;
        auto& s = b.begin("/n_fill");
        s << (int32_t)1000 << (int32_t)0 << (int32_t)3 << 0.5f;
        fx.send(b.end());
    }
    fx.pump(8);

    // Verify with /s_getn
    fx.clearReplies();
    {
        osc_test::Builder b;
        auto& s = b.begin("/s_getn");
        s << (int32_t)1000 << (int32_t)0 << (int32_t)3;
        fx.send(b.end());
    }

    OscReply r;
    REQUIRE(fx.waitForReply("/n_setn", r));
    auto p = r.parsed();
    for (int i = 3; i < 6; i++) {
        CHECK(p.argFloat(i) == Catch::Approx(0.5f).margin(0.01f));
    }

    fx.send(osc_test::message("/n_free", 1000));
    fx.pump(4);
}

// =============================================================================
// /n_mapa, /n_mapan — AUDIO BUS MAPPING
// =============================================================================

TEST_CASE("/n_mapa maps control to audio bus", "[synth_cmd]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    fx.send(sNew("sonic-pi-beep", 1000, 0, 1));
    fx.pump(4);

    {
        osc_test::Builder b;
        auto& s = b.begin("/n_mapa");
        s << (int32_t)1000 << "note" << (int32_t)0;
        fx.send(b.end());
    }
    fx.pump(8);

    // Synth should still be running
    fx.send(osc_test::message("/status"));
    OscReply r;
    REQUIRE(fx.waitForReply("/status.reply", r));
    CHECK(r.parsed().argInt(2) >= 1);

    fx.send(osc_test::message("/n_free", 1000));
    fx.pump(4);
}

TEST_CASE("/n_mapan maps sequential controls to audio buses", "[synth_cmd]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    fx.send(sNew("sonic-pi-beep", 1000, 0, 1));
    fx.pump(4);

    {
        osc_test::Builder b;
        auto& s = b.begin("/n_mapan");
        s << (int32_t)1000 << "note" << (int32_t)0 << (int32_t)1;
        fx.send(b.end());
    }
    fx.pump(8);

    fx.send(osc_test::message("/status"));
    OscReply r;
    REQUIRE(fx.waitForReply("/status.reply", r));

    fx.send(osc_test::message("/n_free", 1000));
    fx.pump(4);
}

TEST_CASE("/n_mapn maps sequential controls to control buses", "[synth_cmd]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    fx.send(sNew("sonic-pi-beep", 1000, 0, 1));
    fx.pump(4);

    {
        osc_test::Builder b;
        auto& s = b.begin("/n_mapn");
        s << (int32_t)1000 << "note" << (int32_t)0 << (int32_t)1;
        fx.send(b.end());
    }
    fx.pump(8);

    fx.send(osc_test::message("/status"));
    OscReply r;
    REQUIRE(fx.waitForReply("/status.reply", r));

    fx.send(osc_test::message("/n_free", 1000));
    fx.pump(4);
}

// =============================================================================
// /g_dumpTree — DUMP NODE TREE
// =============================================================================

TEST_CASE("/g_dumpTree without controls", "[synth_cmd]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    fx.send(osc_test::message("/g_new", 100, 0, 0));
    fx.send(sNew("sonic-pi-beep", 1000, 0, 100));
    fx.pump(8);

    // Dump tree structure (flag 0 = no controls)
    fx.send(osc_test::message("/g_dumpTree", 0, 0));
    fx.pump(16);

    // Verify synth and group still exist after dump
    fx.send(osc_test::message("/status"));
    OscReply st;
    REQUIRE(fx.waitForReply("/status.reply", st));
    CHECK(st.parsed().argInt(2) >= 1);  // synth exists
    CHECK(st.parsed().argInt(3) >= 3);  // root + default + 100

    fx.send(osc_test::message("/n_free", 100));
    fx.pump(4);
}

TEST_CASE("/g_dumpTree with controls", "[synth_cmd]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    fx.send(osc_test::message("/g_new", 100, 0, 0));
    fx.send(sNew("sonic-pi-beep", 1000, 0, 100));
    fx.pump(8);

    // Dump tree with controls (flag 1)
    fx.send(osc_test::message("/g_dumpTree", 0, 1));
    fx.pump(16);

    // Verify synth and group still exist after dump
    fx.send(osc_test::message("/status"));
    OscReply st;
    REQUIRE(fx.waitForReply("/status.reply", st));
    CHECK(st.parsed().argInt(2) >= 1);

    fx.send(osc_test::message("/n_free", 100));
    fx.pump(4);
}

// =============================================================================
// /n_trace — TRACE NODE
// =============================================================================

TEST_CASE("/n_trace traces synth execution", "[synth_cmd]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    fx.send(sNew("sonic-pi-beep", 1000, 0, 1));
    fx.pump(8);

    fx.send(osc_test::message("/n_trace", 1000));
    fx.pump(16);

    // Verify synth still exists after trace
    fx.send(osc_test::message("/status"));
    OscReply st;
    REQUIRE(fx.waitForReply("/status.reply", st));
    CHECK(st.parsed().argInt(2) >= 1);

    fx.send(osc_test::message("/n_free", 1000));
    fx.pump(4);
}

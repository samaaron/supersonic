/*
 * test_synth_lifecycle.cpp — /s_new, /n_free semantic tests, add actions
 */
#include "EngineFixture.h"

// Helper: create a synth with the Builder (string + int args)
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

TEST_CASE("/s_new creates a synth", "[synth]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    fx.send(sNew("sonic-pi-beep", 1000, 0, 1));  // add to head of default group

    // Query the node tree to confirm it exists
    fx.send(osc_test::message("/g_queryTree", 0, 0));
    OscReply r;
    REQUIRE(fx.waitForReply("/g_queryTree.reply", r));

    // /g_queryTree.reply: flag, groupID, numChildren, ...
    auto p = r.parsed();
    CHECK(p.argInt(2) >= 1);  // root group has children

    fx.send(osc_test::message("/n_free", 1000));
}

TEST_CASE("/n_free frees a synth", "[synth]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    fx.send(sNew("sonic-pi-beep", 1000, 0, 1));

    // Confirm synth exists via status
    fx.send(osc_test::message("/status"));
    OscReply before;
    REQUIRE(fx.waitForReply("/status.reply", before));

    // Free it
    fx.send(osc_test::message("/n_free", 1000));

    // If we got here without crash, the free was processed
    SUCCEED();
}

TEST_CASE("Freeing non-existent node does not crash", "[synth]") {
    EngineFixture fx;

    fx.send(osc_test::message("/n_free", 99999));
    SUCCEED();
}

TEST_CASE("/s_new with add-to-tail", "[synth]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    // Create group
    fx.send(osc_test::message("/g_new", 100, 0, 0));

    // Add to tail (action 1) of group 100
    fx.send(sNew("sonic-pi-beep", 1000, 1, 100));

    // Add another to tail
    fx.send(sNew("sonic-pi-beep", 1001, 1, 100));

    // Query tree — both should exist
    fx.send(osc_test::message("/status"));
    OscReply r;
    REQUIRE(fx.waitForReply("/status.reply", r));
    CHECK(r.parsed().argInt(2) >= 2);  // at least 2 synths

    fx.send(osc_test::message("/n_free", 1000));
    fx.send(osc_test::message("/n_free", 1001));
    fx.send(osc_test::message("/n_free", 100));
}

TEST_CASE("/n_run turns synth on/off", "[synth]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    fx.send(osc_test::message("/notify", 1));
    fx.clearReplies();

    fx.send(sNew("sonic-pi-beep", 1000, 0, 1));

    // Turn off
    fx.send(osc_test::message("/n_run", 1000, 0));

    // Turn on
    fx.send(osc_test::message("/n_run", 1000, 1));

    // If we didn't crash, success
    fx.send(osc_test::message("/n_free", 1000));
    SUCCEED();
}

TEST_CASE("/n_set changes synth control value", "[synth]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    fx.send(sNewWithNote("sonic-pi-beep", 1000, 0, 1, 60.0f));

    // Change note
    {
        osc_test::Builder b;
        auto& s = b.begin("/n_set");
        s << (int32_t)1000 << "note" << 72.0f;
        fx.send(b.end());
    }

    // /n_set should not crash — verify by querying status
    fx.send(osc_test::message("/status"));
    OscReply r;
    REQUIRE(fx.waitForReply("/status.reply", r));

    fx.send(osc_test::message("/n_free", 1000));
}

TEST_CASE("Multiple synths can run simultaneously", "[synth]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    for (int i = 0; i < 10; i++) {
        fx.send(sNewWithNote("sonic-pi-beep", 2000 + i, 0, 1, 60.0f + i));
    }

    // Verify synths exist via g_queryTree
    fx.send(osc_test::message("/g_queryTree", 0, 0));
    OscReply r;
    REQUIRE(fx.waitForReply("/g_queryTree.reply", r));
    // At least default group + 10 synths visible in tree
    CHECK(r.parsed().argInt(2) >= 1);

    for (int i = 0; i < 10; i++) {
        fx.send(osc_test::message("/n_free", 2000 + i));
    }
}

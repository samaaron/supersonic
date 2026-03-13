/*
 * test_synthdef.cpp — /d_recv, /d_free, synthdef loading
 */
#include "EngineFixture.h"

TEST_CASE("/d_recv loads synthdef and responds with /done", "[synthdef]") {
    EngineFixture fx;

    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));
}

TEST_CASE("/d_recv increases synthdef count", "[synthdef]") {
    EngineFixture fx;

    // Get count before
    fx.send(osc_test::message("/status"));
    OscReply before;
    REQUIRE(fx.waitForReply("/status.reply", before));
    int defsBefore = before.parsed().argInt(4);  // numSynthDefs

    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    // Get count after
    fx.send(osc_test::message("/status"));
    OscReply after;
    REQUIRE(fx.waitForReply("/status.reply", after));
    int defsAfter = after.parsed().argInt(4);

    CHECK(defsAfter > defsBefore);
}

TEST_CASE("/d_free frees a loaded synthdef", "[synthdef]") {
    EngineFixture fx;

    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    // Get count before free
    fx.send(osc_test::message("/status"));
    OscReply before;
    REQUIRE(fx.waitForReply("/status.reply", before));
    int defsBefore = before.parsed().argInt(4);

    // Free the synthdef
    fx.send(osc_test::message("/d_free", "sonic-pi-beep"));
    fx.pump(8);

    // Get count after free
    fx.send(osc_test::message("/status"));
    OscReply after;
    REQUIRE(fx.waitForReply("/status.reply", after));
    int defsAfter = after.parsed().argInt(4);

    CHECK(defsAfter < defsBefore);
}

TEST_CASE("Loading same synthdef twice is idempotent", "[synthdef]") {
    EngineFixture fx;

    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    // Should still be able to create a synth with it
    osc_test::Builder b;
    auto& s = b.begin("/s_new");
    s << "sonic-pi-beep" << (int32_t)1000 << (int32_t)0 << (int32_t)1;
    auto pkt = b.end();
    fx.send(pkt);
    fx.pump(8);

    // Clean up
    fx.send(osc_test::message("/n_free", 1000));
    fx.pump(4);
    SUCCEED();
}

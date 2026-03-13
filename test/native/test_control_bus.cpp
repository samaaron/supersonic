/*
 * test_control_bus.cpp — Control bus commands: /c_set, /c_get, /c_setn, /c_getn, /c_fill
 */
#include "EngineFixture.h"
#include <catch2/catch_approx.hpp>

// =============================================================================
// /c_set and /c_get
// =============================================================================

TEST_CASE("/c_set and /c_get round-trip single bus", "[control_bus]") {
    EngineFixture fx;

    // Set bus 0 to 440
    {
        osc_test::Builder b;
        auto& s = b.begin("/c_set");
        s << (int32_t)0 << 440.0f;
        fx.send(b.end());
    }
    fx.pump(8);
    fx.clearReplies();

    // Get bus 0
    fx.send(osc_test::message("/c_get", 0));

    OscReply r;
    REQUIRE(fx.waitForReply("/c_set", r));
    auto p = r.parsed();
    CHECK(p.argInt(0) == 0);  // bus index
    CHECK(p.argFloat(1) == Catch::Approx(440.0f).margin(1.0f));
}

TEST_CASE("/c_set multiple buses", "[control_bus]") {
    EngineFixture fx;

    // Set buses 0=440, 1=0.5, 2=880
    {
        osc_test::Builder b;
        auto& s = b.begin("/c_set");
        s << (int32_t)0 << 440.0f
          << (int32_t)1 << 0.5f
          << (int32_t)2 << 880.0f;
        fx.send(b.end());
    }
    fx.pump(8);
    fx.clearReplies();

    // Get all three
    {
        osc_test::Builder b;
        auto& s = b.begin("/c_get");
        s << (int32_t)0 << (int32_t)1 << (int32_t)2;
        fx.send(b.end());
    }

    OscReply r;
    REQUIRE(fx.waitForReply("/c_set", r));
    auto p = r.parsed();
    CHECK(p.argInt(0) == 0);
    CHECK(p.argFloat(1) == Catch::Approx(440.0f).margin(1.0f));
    CHECK(p.argInt(2) == 1);
    CHECK(p.argFloat(3) == Catch::Approx(0.5f).margin(0.01f));
    CHECK(p.argInt(4) == 2);
    CHECK(p.argFloat(5) == Catch::Approx(880.0f).margin(1.0f));
}

// =============================================================================
// /c_setn and /c_getn
// =============================================================================

TEST_CASE("/c_setn and /c_getn round-trip", "[control_bus]") {
    EngineFixture fx;

    // Set buses 0-3 to 100, 200, 300, 400
    {
        osc_test::Builder b;
        auto& s = b.begin("/c_setn");
        s << (int32_t)0 << (int32_t)4
          << 100.0f << 200.0f << 300.0f << 400.0f;
        fx.send(b.end());
    }
    fx.pump(8);
    fx.clearReplies();

    // Get buses 0-3
    fx.send(osc_test::message("/c_getn", 0, 4));

    OscReply r;
    REQUIRE(fx.waitForReply("/c_setn", r));
    auto p = r.parsed();
    CHECK(p.argInt(0) == 0);      // start index
    CHECK(p.argInt(1) == 4);      // count
    CHECK(p.argFloat(2) == Catch::Approx(100.0f).margin(1.0f));
    CHECK(p.argFloat(3) == Catch::Approx(200.0f).margin(1.0f));
    CHECK(p.argFloat(4) == Catch::Approx(300.0f).margin(1.0f));
    CHECK(p.argFloat(5) == Catch::Approx(400.0f).margin(1.0f));
}

// =============================================================================
// /c_fill
// =============================================================================

TEST_CASE("/c_fill fills range of buses", "[control_bus]") {
    EngineFixture fx;

    // Fill buses 0-9 with 0.5
    {
        osc_test::Builder b;
        auto& s = b.begin("/c_fill");
        s << (int32_t)0 << (int32_t)10 << 0.5f;
        fx.send(b.end());
    }
    fx.pump(8);
    fx.clearReplies();

    // Get bus 5
    fx.send(osc_test::message("/c_get", 5));

    OscReply r;
    REQUIRE(fx.waitForReply("/c_set", r));
    CHECK(r.parsed().argFloat(1) == Catch::Approx(0.5f).margin(0.01f));
}

// =============================================================================
// Control bus mapping to synth controls
// =============================================================================

TEST_CASE("/n_map maps synth control to bus", "[control_bus]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    // Set bus 0 to 72
    {
        osc_test::Builder b;
        auto& s = b.begin("/c_set");
        s << (int32_t)0 << 72.0f;
        fx.send(b.end());
    }
    fx.pump(4);

    // Create synth
    {
        osc_test::Builder b;
        auto& s = b.begin("/s_new");
        s << "sonic-pi-beep" << (int32_t)1000 << (int32_t)0 << (int32_t)1;
        fx.send(b.end());
    }
    fx.pump(4);

    // Map note to bus 0
    {
        osc_test::Builder b;
        auto& s = b.begin("/n_map");
        s << (int32_t)1000 << "note" << (int32_t)0;
        fx.send(b.end());
    }
    fx.pump(4);

    // Verify synth still exists
    fx.send(osc_test::message("/status"));
    OscReply r;
    REQUIRE(fx.waitForReply("/status.reply", r));
    CHECK(r.parsed().argInt(2) >= 1);

    // Unmap
    {
        osc_test::Builder b;
        auto& s = b.begin("/n_map");
        s << (int32_t)1000 << "note" << (int32_t)-1;
        fx.send(b.end());
    }
    fx.pump(4);

    fx.send(osc_test::message("/n_free", 1000));
    fx.pump(4);
}

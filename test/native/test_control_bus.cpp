/*
 * test_control_bus.cpp — Control bus commands: /c_set, /c_get, /c_setn, /c_getn, /c_fill
 */
#include "EngineFixture.h"
#include "SynthDefBytes.h"
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

    // Create synth
    {
        osc_test::Builder b;
        auto& s = b.begin("/s_new");
        s << "sonic-pi-beep" << (int32_t)1000 << (int32_t)0 << (int32_t)1;
        fx.send(b.end());
    }

    // Map note to bus 0
    {
        osc_test::Builder b;
        auto& s = b.begin("/n_map");
        s << (int32_t)1000 << "note" << (int32_t)0;
        fx.send(b.end());
    }

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

    fx.send(osc_test::message("/n_free", 1000));
}

// =============================================================================
// /n_set "c<bus>": a control mapped to a control bus by its number
// =============================================================================

namespace {

// One control, "in" (440 to start), copied to control bus 5 every block: the bus says what the control reads.
std::vector<uint8_t> copyToBus5() {
    synthdef_bytes::Def d;
    d.i32(0x53436766).i32(2).i16(1);
    d.str("copy_to_bus5");
    d.i32(1).f32(5.0f);                      // a constant: bus 5
    d.i32(1).f32(440.0f);                    // one control, 440
    d.i32(1).str("in").i32(0);               // named "in"
    d.i32(2);                                // two units
    d.str("Control").i8(1).i32(0).i32(1).i16(0).i8(1);
    d.str("Out").i8(1).i32(2).i32(0).i16(0);
    d.i32(-1).i32(0);                        // the bus, constant 0
    d.i32(0).i32(0);                         // the control
    d.i16(0);                                // no variants
    return d.bytes();
}

osc_test::Packet mapTo(const char* bus) {
    osc_test::Builder b;
    auto& s = b.begin("/n_set");
    s << (int32_t)1000 << "in" << bus;
    return b.end();
}

} // namespace

// A bus number too big for an int is a bus that does not exist, ignored as any other: scsynth's sc_atoi counted it
// on past an int's range (undefined behaviour, found by fuzz_scsynth_commands), and in practice it wrapped, onto a
// bus that does exist or onto -1, which unmaps.
TEST_CASE("/n_set to a bus number too big for an int maps nothing and unmaps nothing", "[control_bus]") {
    EngineFixture fx;
    REQUIRE(synthdef_bytes::loads(fx, copyToBus5()));
    {
        osc_test::Builder b;
        auto& s = b.begin("/c_set");
        s << (int32_t)1 << 777.0f;
        fx.send(b.end());
    }
    {
        osc_test::Builder b;
        auto& s = b.begin("/s_new");
        s << "copy_to_bus5" << (int32_t)1000 << (int32_t)0 << (int32_t)1;
        fx.send(b.end());
    }
    fx.pumpBlock(4);
    CHECK(synthdef_bytes::controlBus(fx, 5) == Catch::Approx(440.0f));

    // 2^32 + 1: wrapped, bus 1
    fx.send(mapTo("c4294967297"));
    fx.pumpBlock(4);
    CHECK(synthdef_bytes::controlBus(fx, 5) == Catch::Approx(440.0f));

    // a bus that exists still maps
    fx.send(mapTo("c1"));
    fx.pumpBlock(4);
    CHECK(synthdef_bytes::controlBus(fx, 5) == Catch::Approx(777.0f));

    // 2^32 - 1: wrapped, -1, which unmaps
    fx.send(mapTo("c4294967295"));
    fx.pumpBlock(4);
    CHECK(synthdef_bytes::controlBus(fx, 5) == Catch::Approx(777.0f));

    fx.send(osc_test::message("/n_free", 1000));
}

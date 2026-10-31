/*
 * test_ugen_controls.cpp - unit generators given control values no client
 * should send: bus numbers past the end of the buses, NaN, infinities.
 *
 * A bus number becomes an index into the World's buses, and the I/O units
 * check the range before using it. The range check must hold for every value
 * a float control can take, so a huge bus number reads and writes nothing,
 * and the engine's memory around the buses is left alone.
 */
#include "SynthDefBytes.h"

#include <limits>
#include <utility>

using namespace synthdef_bytes;

TEST_CASE("UGen controls: a bus number past the end of the buses writes nothing", "[ugen_controls]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));
    const float buses[] = {3e9f, 2.1474835e9f, -3e9f, std::numeric_limits<float>::infinity(),
                           -std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()};
    for (const float bus : buses) {
        osc_test::Builder b;
        b.begin("/s_new") << "sonic-pi-beep" << 1000 << 0 << 0 << "out_bus" << bus << "sustain" << 10.0f;
        fx.send(b.end());
        fx.pumpBlock(16);
        fx.send(osc_test::message("/n_free", 1000));
        fx.pumpBlock(2);
    }
    // Memory the engine allocates around the buses is still sound.
    poolStillSound(fx);
}

TEST_CASE("UGen controls: GVerb with a room of 0, or larger than its maximum, plays", "[ugen_controls]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-fx_gverb"));
    const float nan = std::numeric_limits<float>::quiet_NaN();
    for (const auto& [room, maxRoom] : {std::pair{0.0f, 300.0f}, std::pair{-5.0f, 300.0f},
                                        std::pair{500.0f, 20.0f}, std::pair{nan, 300.0f}}) {
        INFO("room " << room << " max_room " << maxRoom);
        osc_test::Builder b;
        b.begin("/s_new") << "sonic-pi-fx_gverb" << 1000 << 0 << 0 << "room" << room << "max_room" << maxRoom;
        fx.send(b.end());
        fx.pumpBlock(64);
        fx.send(osc_test::message("/n_free", 1000));
        fx.pumpBlock(2);
    }
    poolStillSound(fx);
}

TEST_CASE("UGen controls: GVerb's room grown past where it started plays", "[ugen_controls]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-fx_gverb"));
    osc_test::Builder start;
    start.begin("/s_new") << "sonic-pi-fx_gverb" << 1000 << 0 << 0 << "room" << 2.0f << "max_room" << 300.0f;
    fx.send(start.end());
    fx.pumpBlock(16);
    osc_test::Builder grow;
    grow.begin("/n_set") << 1000 << "room" << 290.0f;
    fx.send(grow.end());
    fx.pumpBlock(256);
    fx.send(osc_test::message("/n_free", 1000));
    fx.pumpBlock(2);
    poolStillSound(fx);
}

namespace {

// GrainBuf.ar(1, trig, blocks * ControlDur, buf, 1, pos, 2, 0, -1, 3), with
// room for three grains, its first sample each block written by A2K and Out.kr
// to control bus `bus`.
std::vector<uint8_t> grainsDef() {
    constexpr int8_t kScalar = 0;
    constexpr int8_t kControl = 1;
    constexpr int8_t kAudio = 2;
    Def d;
    d.i32(0x53436766).i32(2).i16(1);
    d.str("grains");
    d.i32(5);
    for (const float c : {0.0f, 1.0f, 2.0f, -1.0f, 3.0f}) d.f32(c);
    const char* controls[] = {"trig", "blocks", "pos", "buf", "bus"};
    d.i32(5);
    for (int k = 0; k < 5; ++k) d.f32(0.0f);
    d.i32(5);
    for (int32_t k = 0; k < 5; ++k) d.str(controls[k]).i32(k);
    d.i32(6);
    d.str("Control").i8(kControl).i32(0).i32(5).i16(0);
    for (int k = 0; k < 5; ++k) d.i8(kControl);
    d.str("ControlDur").i8(kScalar).i32(0).i32(1).i16(0);
    d.i8(kScalar);
    d.str("BinaryOpUGen").i8(kControl).i32(2).i32(1).i16(2);
    d.i32(0).i32(1).i32(1).i32(0);
    d.i8(kControl);
    d.str("GrainBuf").i8(kAudio).i32(9).i32(1).i16(0);
    d.i32(0).i32(0).i32(2).i32(0).i32(0).i32(3).i32(-1).i32(1).i32(0).i32(2);
    d.i32(-1).i32(2).i32(-1).i32(0).i32(-1).i32(3).i32(-1).i32(4);
    d.i8(kAudio);
    d.str("A2K").i8(kControl).i32(1).i32(1).i16(0);
    d.i32(3).i32(0);
    d.i8(kControl);
    d.str("Out").i8(kControl).i32(2).i32(0).i16(0);
    d.i32(0).i32(4).i32(4).i32(0);
    d.i16(0);
    return d.bytes();
}

void setGrain(EngineFixture& fx, int32_t node, float trig, float blocks, float pos, float buf) {
    osc_test::Builder b;
    b.begin("/n_set") << node << "trig" << trig << "blocks" << blocks << "pos" << pos << "buf" << buf;
    fx.send(b.end());
}

} // namespace

TEST_CASE("UGen controls: a grain GrainBuf refuses to start takes no slot and plays nothing", "[ugen_controls]") {
    EngineFixture fx;
    REQUIRE(fx.sendAndExpectDone(osc_test::message("/b_alloc", 0, 1024, 1)));
    REQUIRE(fx.sendAndExpectDone(osc_test::message("/b_alloc", 1, 1024, 2)));
    osc_test::Builder fill;
    fill.begin("/b_fill") << 0 << 0 << 1024 << 1.0f;
    fx.send(fill.end());
    REQUIRE(loads(fx, grainsDef()));

    // Two synths are sent the same grains, and the first is also sent one it
    // must refuse, for a position that is not a number or for a buffer that
    // is not mono. The refusal comes when the slot past the playing grain
    // still holds a copy of it, moved down when the grain before it ended.
    const float nan = std::numeric_limits<float>::quiet_NaN();
    for (const auto& [pos, buf] : {std::pair{nan, 0.0f}, std::pair{0.0f, 1.0f}}) {
        INFO("pos " << pos << " buf " << buf);
        for (const int32_t node : {1000, 1001}) {
            osc_test::Builder b;
            b.begin("/s_new") << "grains" << node << 0 << 0 << "bus" << float(node - 1000);
            fx.send(b.end());
        }
        fx.pumpBlock(1);
        auto both = [&](float trig, float blocks) {
            setGrain(fx, 1000, trig, blocks, 0.0f, 0.0f);
            setGrain(fx, 1001, trig, blocks, 0.0f, 0.0f);
            fx.pumpBlock(1);
        };
        both(1.0f, 3.5f);
        both(0.0f, 3.5f);
        both(1.0f, 100.0f);
        both(0.0f, 100.0f);
        setGrain(fx, 1000, 1.0f, 100.0f, pos, buf);
        fx.pumpBlock(1);
        both(0.0f, 100.0f);
        both(1.0f, 100.0f);
        both(0.0f, 100.0f);

        // Both buses in one reply, so both from the same block.
        fx.clearReplies();
        fx.send(osc_test::message("/c_getn", 0, 2));
        OscReply reply;
        REQUIRE(fx.waitForReply("/c_setn", reply));
        const auto parsed = reply.parsed();
        REQUIRE(parsed.argCount() == 4);
        const float refused = parsed.argFloat(2);
        const float plain = parsed.argFloat(3);
        CHECK(plain > 0.0f);
        CHECK(refused == plain);
        fx.send(osc_test::message("/n_free", 1000));
        fx.send(osc_test::message("/n_free", 1001));
        fx.pumpBlock(2);
    }
    poolStillSound(fx);
}

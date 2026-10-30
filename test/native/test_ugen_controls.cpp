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

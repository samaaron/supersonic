// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Sam Aaron
/*
 * test_ugen_ports.cpp - units given fewer inputs or outputs than their code
 * uses. A SynthDef gives each unit its inputs and outputs, and nothing says how
 * many a unit type needs, so a SynthDef can give any unit too few. An input
 * the unit does not have reads as the constant 0, and an output it does not
 * have is written nowhere the synth reads.
 */
#include "SynthDefBytes.h"
#include "GuestWorld.h"   // the engine's World, whose registries hold the unit types

#include "HashTable.h"
#include "SC_HiddenWorld.h"
#include "SC_UnitDef.h"

#include <catch2/catch_approx.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <utility>

using namespace synthdef_bytes;

namespace {

constexpr int8_t kScalar = 0;
constexpr int8_t kControl = 1;
constexpr int8_t kAudio = 2;

std::vector<std::string> everyUnitType(EngineFixture& fx) {
    std::vector<std::string> names;
    const auto* lib = guestWorld(fx)->hw->mUnitDefLib;
    for (int32 i = 0; i < lib->TableSize(); ++i)
        if (const UnitDef* def = lib->AtIndex(i))
            names.emplace_back(reinterpret_cast<const char*>(def->mUnitDefName));
    std::sort(names.begin(), names.end());
    return names;
}

// One def holding one `ugen` at `rate`, with no inputs and `outputs` outputs.
std::vector<uint8_t> bareUnit(const char* name, const std::string& ugen, int8_t rate, int32_t outputs) {
    Def d;
    d.i32(0x53436766).i32(2).i16(1);
    d.str(name);
    d.i32(0);                                          // no constants
    d.i32(0);                                          // no controls
    d.i32(0);                                          // no control names
    d.i32(1);
    d.str(ugen.c_str()).i8(rate).i32(0).i32(outputs).i16(0);
    for (int32_t o = 0; o < outputs; ++o) d.i8(rate);
    d.i16(0);                                          // no variants
    return d.bytes();
}

// One def holding one `ugen` at `rate`, with `inputs` inputs all the
// constant `value`, and `outputs` outputs.
std::vector<uint8_t> unitGiven(const char* name, const std::string& ugen, int8_t rate, int32_t inputs, float value,
                               int32_t outputs) {
    Def d;
    d.i32(0x53436766).i32(2).i16(1);
    d.str(name);
    d.i32(1).f32(value);
    d.i32(0);                                          // no controls
    d.i32(0);                                          // no control names
    d.i32(1);
    d.str(ugen.c_str()).i8(rate).i32(inputs).i32(outputs).i16(0);
    for (int32_t i = 0; i < inputs; ++i) d.i32(-1).i32(0);
    for (int32_t o = 0; o < outputs; ++o) d.i8(rate);
    d.i16(0);                                          // no variants
    return d.bytes();
}

// One def holding a control and one `ugen` at `rate`, with `inputs` inputs all
// the control's output, and `outputs` outputs. A unit at audio rate given a
// control-rate input reads one value held in its wire.
std::vector<uint8_t> unitFedByControl(const char* name, const std::string& ugen, int8_t rate, int32_t inputs,
                                      int32_t outputs) {
    Def d;
    d.i32(0x53436766).i32(2).i16(1);
    d.str(name);
    d.i32(0);                                          // no constants
    d.i32(1).f32(0.5f);                                // one control
    d.i32(1).str("level").i32(0);
    d.i32(2);
    d.str("Control").i8(kControl).i32(0).i32(1).i16(0);
    d.i8(kControl);
    d.str(ugen.c_str()).i8(rate).i32(inputs).i32(outputs).i16(0);
    for (int32_t i = 0; i < inputs; ++i) d.i32(0).i32(0);
    for (int32_t o = 0; o < outputs; ++o) d.i8(rate);
    d.i16(0);                                          // no variants
    return d.bytes();
}

void startNode(EngineFixture& fx, const char* def) {
    osc_test::Builder b;
    b.begin("/s_new") << def << 2000 << 0 << 0;
    fx.send(b.end());
}

// The k-th constant as an input at `rate`: the constant itself at scalar rate,
// otherwise a DC unit added to `units`.
std::pair<int32_t, int32_t> constantAt(std::vector<UnitSpec>& units, int8_t rate, int32_t k) {
    if (rate == kScalar) return {-1, k};
    units.push_back({"DC", rate, {{-1, k}}, 1});
    return {static_cast<int32_t>(units.size()) - 1, 0};
}

} // namespace

TEST_CASE("UGen ports: every unit type given no inputs plays and frees", "[ugen_ports]") {
    EngineFixture fx;
    // Buffer 0 exists, so a missing buffer number finds a buffer and the
    // units that read and write buffers run.
    REQUIRE(fx.sendAndExpectDone(osc_test::message("/b_alloc", 0, 1024, 1)));

    const auto types = everyUnitType(fx);
    REQUIRE(types.size() > 300);
    for (const auto& ugen : types) {
        for (const int8_t rate : {kScalar, kControl, kAudio}) {
            for (const int32_t outputs : {0, 1}) {
                INFO(ugen << " rate " << int(rate) << " outputs " << outputs);
                const auto def = bareUnit("bare", ugen, rate, outputs);
                REQUIRE(loads(fx, def));
                startNode(fx, "bare");
                fx.pumpBlock(4);
                fx.send(osc_test::message("/n_free", 2000));
                fx.pumpBlock(1);
            }
        }
    }
    poolStillSound(fx);
}

TEST_CASE("UGen ports: every unit type given extreme inputs plays and frees", "[ugen_ports]") {
    EngineFixture fx;
    REQUIRE(fx.sendAndExpectDone(osc_test::message("/b_alloc", 0, 1024, 1)));

    const float inf = std::numeric_limits<float>::infinity();
    const float values[] = {std::numeric_limits<float>::quiet_NaN(), inf, -inf, 1e30f, -1e30f, 3e9f, -3e9f, -1.0f,
                            // Small and positive: a rate or frequency this low makes a period or a
                            // buffer that an int cannot hold twice over.
                            1e-30f, 1e-9f, 2.5e-7f, 3.2e-5f, 1e-3f};
    for (const auto& ugen : everyUnitType(fx)) {
        for (const int8_t rate : {kScalar, kControl, kAudio}) {
            for (const float value : values) {
                INFO(ugen << " rate " << int(rate) << " inputs " << value);
                REQUIRE(loads(fx, unitGiven("extreme", ugen, rate, 16, value, 2)));
                startNode(fx, "extreme");
                fx.pumpBlock(4);
                fx.send(osc_test::message("/n_free", 2000));
                fx.pumpBlock(1);
            }
        }
    }
    poolStillSound(fx);
}

// BinaryOpUGen and UnaryOpUGen hold every operator behind one unit name, and
// the special index picks the operator. Each operator has its own code for each
// rate of its inputs, and the sweep above only reaches operator 0. The loops run
// one past the last operator, which has no code of its own.
TEST_CASE("UGen ports: every operator given extreme inputs at every input rate plays and frees", "[ugen_ports]") {
    constexpr int16_t kBinaryOperators = 49;   // opNumBinarySelectors
    constexpr int16_t kUnaryOperators = 54;    // opNumUnarySelectors
    const float inf = std::numeric_limits<float>::infinity();
    const float nan = std::numeric_limits<float>::quiet_NaN();
    EngineFixture fx;

    const auto play = [&](const std::vector<float>& consts, const std::vector<UnitSpec>& units) {
        REQUIRE(loads(fx, graphDef("operator", consts, units)));
        startNode(fx, "operator");
        fx.pumpBlock(4);
        fx.send(osc_test::message("/n_free", 2000));
        fx.pumpBlock(1);
    };

    const std::vector<float> lefts = {1.0f, -1.0f, nan, 1e30f, -1e30f};
    const std::vector<float> rights = {33.0f, -1.0f, nan, inf, 1e30f, -1e30f};
    std::vector<float> pairs = lefts;
    pairs.insert(pairs.end(), rights.begin(), rights.end());
    for (int16_t op = 0; op <= kBinaryOperators; ++op) {
        for (const int8_t rateA : {kScalar, kControl, kAudio}) {
            for (const int8_t rateB : {kScalar, kControl, kAudio}) {
                INFO("binary operator " << op << " input rates " << int(rateA) << " " << int(rateB));
                std::vector<UnitSpec> units;
                for (size_t i = 0; i < lefts.size(); ++i) {
                    for (size_t j = 0; j < rights.size(); ++j) {
                        const auto a = constantAt(units, rateA, static_cast<int32_t>(i));
                        const auto b = constantAt(units, rateB, static_cast<int32_t>(lefts.size() + j));
                        units.push_back({"BinaryOpUGen", std::max(rateA, rateB), {a, b}, 1, op});
                    }
                }
                play(pairs, units);
            }
        }
    }

    const std::vector<float> values = {nan, inf, -inf, 1e30f, -1e30f, 3e9f, -3e9f, -1.0f, 1e-30f};
    for (int16_t op = 0; op <= kUnaryOperators; ++op) {
        for (const int8_t rate : {kScalar, kControl, kAudio}) {
            INFO("unary operator " << op << " input rate " << int(rate));
            std::vector<UnitSpec> units;
            for (size_t k = 0; k < values.size(); ++k) {
                const auto in = constantAt(units, rate, static_cast<int32_t>(k));
                units.push_back({"UnaryOpUGen", rate, {in}, 1, op});
            }
            play(values, units);
        }
    }
    poolStillSound(fx);
}

// A shift count outside 0 to 31 is undefined in C++, and a float input can hold
// any count. The operators take the count modulo 32, on every target.
TEST_CASE("UGen ports: the shift operators take their count modulo 32", "[ugen_ports]") {
    constexpr int16_t kShiftLeft = 26;    // opShiftLeft
    constexpr int16_t kShiftRight = 27;   // opShiftRight
    struct Shift {
        int16_t op;
        float value, count, expected;
    };
    const Shift shifts[] = {
        {kShiftRight, 64.0f, 2.0f, 16.0f},
        {kShiftRight, 64.0f, 33.0f, 32.0f},
        {kShiftRight, 64.0f, -31.0f, 32.0f},
        {kShiftRight, -8.0f, 1.0f, -4.0f},
        {kShiftRight, -8.0f, 33.0f, -4.0f},
        {kShiftLeft, 1.0f, 3.0f, 8.0f},
        {kShiftLeft, 1.0f, 33.0f, 2.0f},
        {kShiftLeft, 1.0f, -1.0f, -2147483648.0f},
    };
    EngineFixture fx;
    for (const auto& s : shifts) {
        INFO("operator " << s.op << " value " << s.value << " count " << s.count);
        REQUIRE(loads(fx, graphDef("shift", {s.value, s.count, 0.0f},
                                   {{"BinaryOpUGen", kControl, {{-1, 0}, {-1, 1}}, 1, s.op},
                                    {"Out", kControl, {{-1, 2}, {0, 0}}, 0}})));
        startNode(fx, "shift");
        fx.pumpBlock(2);
        CHECK(controlBus(fx, 0) == s.expected);
        fx.send(osc_test::message("/n_free", 2000));
        fx.pumpBlock(1);
    }
    poolStillSound(fx);
}

// Pitch sizes its analysis buffer as twice its longest period, and that
// period is the sample rate over the minimum frequency it is given. A
// frequency low enough makes a period an int cannot hold twice over; the
// buffer must still hold every sample the analysis reads. Inputs: in,
// initFreq, minFreq, maxFreq, execFreq, maxBinsPerOctave, median,
// ampThreshold, peakThreshold, downSample, clar.
TEST_CASE("UGen ports: Pitch given a minimum frequency its period cannot double plays and frees", "[ugen_ports]") {
    EngineFixture fx;
    SECTION("control-rate input: one sample a block, a period of 1.5e9 of them") {
        REQUIRE(loads(fx, graphDef("pitchk", {0.0f, 440.0f, 2.5e-7f, 4000.0f, 1000.0f, 16.0f, 1.0f, 0.01f, 0.5f, 1.0f, 0.0f},
                                   {{"Pitch", kControl, constants(0, 11), 2}})));
        startNode(fx, "pitchk");
        fx.pumpBlock(260);   // past the first full buffer, so the analysis runs
    }
    SECTION("audio-rate input: a period of 1.5e9 samples") {
        REQUIRE(loads(fx, graphDef("pitcha", {440.0f, 3.2e-5f, 4000.0f, 1000.0f, 16.0f, 1.0f, 0.01f, 0.5f, 1.0f, 0.0f},
                                   {{"WhiteNoise", kAudio, {}, 1},
                                    {"Pitch", kControl, [] {
                                         std::vector<std::pair<int32_t, int32_t>> in = {{0, 0}};
                                         for (int32_t k = 0; k < 10; ++k) in.emplace_back(-1, k);
                                         return in;
                                     }(), 2}})));
        startNode(fx, "pitcha");
        fx.pumpBlock(8);
    }
    fx.send(osc_test::message("/n_free", 2000));
    fx.pumpBlock(2);
    stillRunning(fx);
}

TEST_CASE("UGen ports: every unit type given control-rate inputs plays and frees", "[ugen_ports]") {
    EngineFixture fx;
    REQUIRE(fx.sendAndExpectDone(osc_test::message("/b_alloc", 0, 1024, 1)));

    for (const auto& ugen : everyUnitType(fx)) {
        for (const int8_t rate : {kScalar, kControl, kAudio}) {
            INFO(ugen << " rate " << int(rate));
            REQUIRE(loads(fx, unitFedByControl("fed", ugen, rate, 16, 2)));
            startNode(fx, "fed");
            fx.pumpBlock(4);
            fx.send(osc_test::message("/n_free", 2000));
            fx.pumpBlock(1);
        }
    }
    poolStillSound(fx);
}

TEST_CASE("UGen ports: Pan2 given one output still pans into it", "[ugen_ports]") {
    EngineFixture fx;
    // Pan2.kr(0.5, 0, 1) with one output, written by Out.kr to control bus 0,
    // and a DC.kr(0.25) after it writing control bus 1.
    Def d;
    d.i32(0x53436766).i32(2).i16(1);
    d.str("pan_one_out");
    d.i32(6);
    for (const float c : {0.5f, 0.0f, 1.0f, 0.0f, 0.25f, 1.0f}) d.f32(c);
    d.i32(0).i32(0);
    d.i32(4);
    d.str("Pan2").i8(kControl).i32(3).i32(1).i16(0);
    d.i32(-1).i32(0).i32(-1).i32(1).i32(-1).i32(2);
    d.i8(kControl);
    d.str("Out").i8(kControl).i32(2).i32(0).i16(0);
    d.i32(-1).i32(3).i32(0).i32(0);
    d.str("DC").i8(kControl).i32(1).i32(1).i16(0);
    d.i32(-1).i32(4);
    d.i8(kControl);
    d.str("Out").i8(kControl).i32(2).i32(0).i16(0);
    d.i32(-1).i32(5).i32(2).i32(0);
    d.i16(0);
    REQUIRE(loads(fx, d.bytes()));

    startNode(fx, "pan_one_out");
    fx.pumpBlock(4);
    // Centre panning puts the signal at -3 dB in each channel.
    CHECK(controlBus(fx, 0) == Catch::Approx(0.5f * std::sqrt(0.5f)).epsilon(0.01));
    CHECK(controlBus(fx, 1) == 0.25f);
    fx.send(osc_test::message("/n_free", 2000));
    fx.pumpBlock(1);
    poolStillSound(fx);
}

TEST_CASE("UGen ports: a unit given a rate it has no calculation for outputs zeros", "[ugen_ports]") {
    EngineFixture fx;
    // BlockSize computes its one value when built, at scalar rate. Given
    // control rate, written by Out.kr to control bus 0.
    Def d;
    d.i32(0x53436766).i32(2).i16(1);
    d.str("blocksize_kr");
    d.i32(1).f32(0.0f);
    d.i32(0).i32(0);
    d.i32(2);
    d.str("BlockSize").i8(kControl).i32(0).i32(1).i16(0);
    d.i8(kControl);
    d.str("Out").i8(kControl).i32(2).i32(0).i16(0);
    d.i32(-1).i32(0).i32(0).i32(0);
    d.i16(0);
    REQUIRE(loads(fx, d.bytes()));

    osc_test::Builder set;
    set.begin("/c_set") << 0 << 0.75f;
    fx.send(set.end());
    startNode(fx, "blocksize_kr");
    fx.pumpBlock(4);
    CHECK(controlBus(fx, 0) == 0.0f);
    fx.send(osc_test::message("/n_free", 2000));
    fx.pumpBlock(1);
    poolStillSound(fx);
}

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

std::vector<std::string> everyUnitType() {
    std::vector<std::string> names;
    for (int32 i = 0; i < gUnitDefLib->TableSize(); ++i)
        if (const UnitDef* def = gUnitDefLib->AtIndex(i))
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

} // namespace

TEST_CASE("UGen ports: every unit type given no inputs plays and frees", "[ugen_ports]") {
    EngineFixture fx;
    // Buffer 0 exists, so a missing buffer number finds a buffer and the
    // units that read and write buffers run.
    REQUIRE(fx.sendAndExpectDone(osc_test::message("/b_alloc", 0, 1024, 1)));

    const auto types = everyUnitType();
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
    const float values[] = {std::numeric_limits<float>::quiet_NaN(), inf, -inf, 1e30f, -1e30f, 3e9f, -3e9f, -1.0f};
    for (const auto& ugen : everyUnitType()) {
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

TEST_CASE("UGen ports: every unit type given control-rate inputs plays and frees", "[ugen_ports]") {
    EngineFixture fx;
    REQUIRE(fx.sendAndExpectDone(osc_test::message("/b_alloc", 0, 1024, 1)));

    for (const auto& ugen : everyUnitType()) {
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

/*
 * test_synthdef_inputs.cpp - a SynthDef whose unit inputs name a source that
 * does not exist is answered with /fail, and the engine keeps running.
 *
 * Each input names its source as (unit index, output index) for an earlier
 * unit's output, or (-1, constant index) for a constant. Each case sends defs
 * that differ from a valid one only in that source.
 */
#include "SynthDefBytes.h"

using namespace synthdef_bytes;

namespace {

// One def named `name`: one control `x` (a Control unit, one output) and an
// Out.kr whose bus is constant 0 and whose signal is (fromUnit, fromIndex).
// Valid when fromUnit is 0 and fromIndex 0, or fromUnit -1 and fromIndex 0.
std::vector<uint8_t> defWithSignalFrom(const char* name, int32_t fromUnit, int32_t fromIndex) {
    Def d;
    d.i32(0x53436766).i32(2).i16(1);              // "SCgf", version 2, one def
    d.str(name);
    d.i32(1).f32(0.0f);                           // constants: 0
    d.i32(1).f32(0.0f);                           // one control, initially 0
    d.i32(1).str("x").i32(0);                     // named x, control 0
    d.i32(2);                                     // two units
    d.str("Control").i8(1).i32(0).i32(1).i16(0);  // 0: Control.kr, no inputs, one output
    d.i8(1);                                      //    its output, control rate
    d.str("Out").i8(1).i32(2).i32(0).i16(0);      // 1: Out.kr, two inputs, no outputs
    d.i32(-1).i32(0);                             //    bus: constant 0
    d.i32(fromUnit).i32(fromIndex);               //    signal: the source under test
    d.i16(0);                                     // no variants
    return d.bytes();
}

// The same def with its two units swapped: Out.kr first, reading the output
// of the Control unit that follows it.
std::vector<uint8_t> defReadingLaterUnit(const char* name) {
    Def d;
    d.i32(0x53436766).i32(2).i16(1);
    d.str(name);
    d.i32(1).f32(0.0f);
    d.i32(1).f32(0.0f);
    d.i32(1).str("x").i32(0);
    d.i32(2);
    d.str("Out").i8(1).i32(2).i32(0).i16(0);      // 0: Out.kr
    d.i32(-1).i32(0);                             //    bus: constant 0
    d.i32(1).i32(0);                              //    signal: unit 1, output 0
    d.str("Control").i8(1).i32(0).i32(1).i16(0);  // 1: Control.kr
    d.i8(1);
    d.i16(0);
    return d.bytes();
}

} // namespace

TEST_CASE("SynthDef inputs: a signal from an output the unit does not have is refused",
          "[synthdef][inputs]") {
    EngineFixture fx;
    CHECK(loads(fx, defWithSignalFrom("in_ok", 0, 0)));
    CHECK_FALSE(loads(fx, defWithSignalFrom("in_bad_output", 0, 24)));
    CHECK_FALSE(loads(fx, defWithSignalFrom("in_negative_output", 0, -1)));
    stillRunning(fx);
}

TEST_CASE("SynthDef inputs: a signal from a unit that does not exist or comes later is refused",
          "[synthdef][inputs]") {
    EngineFixture fx;
    CHECK_FALSE(loads(fx, defWithSignalFrom("in_bad_unit", 7, 0)));
    CHECK_FALSE(loads(fx, defWithSignalFrom("in_self", 1, 0)));   // Out has no outputs
    CHECK_FALSE(loads(fx, defReadingLaterUnit("in_later_unit")));
    stillRunning(fx);
}

TEST_CASE("SynthDef inputs: a constant that does not exist is refused",
          "[synthdef][inputs]") {
    EngineFixture fx;
    CHECK(loads(fx, defWithSignalFrom("in_const_ok", -1, 0)));
    CHECK_FALSE(loads(fx, defWithSignalFrom("in_bad_const", -1, 5)));
    CHECK_FALSE(loads(fx, defWithSignalFrom("in_negative_const", -1, -3)));
    stillRunning(fx);
}

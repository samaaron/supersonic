/*
 * test_synthdef_counts.cpp - a SynthDef whose counts claim more elements than
 * any engine can hold is answered with /fail, and the engine keeps running.
 *
 * A count is read before the array it sizes. Where size_t is 32 bits, as in
 * the web engine, a large enough count wraps the array's size in bytes.
 */
#include "SynthDefBytes.h"

using namespace synthdef_bytes;

// "SCgf", version 2, one def named "huge" claiming 0x3FFFFFFD constants and
// carrying none. 0x3FFFFFFD four-byte constants with the array's header come
// to 2^32 + 4 bytes, which wraps to 4 where size_t is 32 bits.
TEST_CASE("SynthDef counts: a constant count larger than memory is refused",
          "[synthdef][counts]") {
    EngineFixture fx;
    Def d;
    d.i32(0x53436766).i32(2).i16(1);
    d.str("huge");
    d.i32(0x3FFFFFFD);
    CHECK_FALSE(loads(fx, d.bytes()));
    stillRunning(fx);
}


namespace {

// "SCgf", version 2, one def named "huge".
Def header() {
    Def d;
    d.i32(0x53436766).i32(2).i16(1);
    d.str("huge");
    return d;
}

} // namespace

TEST_CASE("SynthDef counts: each count the def cannot hold is refused", "[synthdef][counts]") {
    EngineFixture fx;

    SECTION("a negative constant count") {
        Def d = header();
        d.i32(-1);
        CHECK_FALSE(loads(fx, d.bytes()));
    }
    SECTION("more controls than the def holds") {
        Def d = header();
        d.i32(0).i32(0x3FFFFFFD);
        CHECK_FALSE(loads(fx, d.bytes()));
    }
    SECTION("more control names than the def holds") {
        Def d = header();
        d.i32(0).i32(0).i32(int32_t(0xFD000001));
        CHECK_FALSE(loads(fx, d.bytes()));
    }
    SECTION("more units than the def holds") {
        Def d = header();
        d.i32(0).i32(0).i32(0).i32(0x7FFFFFFF);
        CHECK_FALSE(loads(fx, d.bytes()));
    }
    SECTION("more unit inputs than the def holds") {
        Def d = header();
        d.i32(0).i32(0).i32(0).i32(1);
        d.str("SinOsc").i8(2).i32(0x7FFFFFFF).i32(1).i16(0);
        CHECK_FALSE(loads(fx, d.bytes()));
    }
    SECTION("more unit outputs than the def holds") {
        Def d = header();
        d.i32(0).i32(0).i32(0).i32(1);
        d.str("SinOsc").i8(2).i32(0).i32(0x7FFFFFFF).i16(0);
        CHECK_FALSE(loads(fx, d.bytes()));
    }
    stillRunning(fx);
}

// In version 3 each def starts with its size in bytes, counting the size
// field itself. A size the bytes sent cannot hold is refused before the def's
// end is worked out from it.
TEST_CASE("SynthDef sizes: a version 3 def whose size does not fit what was sent is refused",
          "[synthdef][counts]") {
    EngineFixture fx;
    for (const int32_t size : {int32_t(-1), int32_t(0x80000000), 2, 0x7FFFFFFF}) {
        Def d;
        d.i32(0x53436766).i32(3).i16(1);
        d.i32(size);
        d.str("huge").i32(0).i32(0).i32(0).i32(0).i16(0);
        CHECK_FALSE(loads(fx, d.bytes()));
    }
    stillRunning(fx);
}

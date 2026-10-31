// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Sam Aaron
/*
 * test_unit_buffer_alignment.cpp - every buffer a unit is handed starts on the
 * widest vector load nova-simd makes (kSCVectorAlign). A unit's calc function
 * may load a block from any input with an aligned SIMD load, and a SynthDef
 * can wire any input from a single value: a constant, or the output of a
 * control-rate unit, held in its wire inside the synth's memory.
 *
 * A probe unit records the address of each buffer it is given. Where a wire
 * lands depends on how many units come before the wires, so the synths here
 * have from two to five units.
 */
#include "SynthDefBytes.h"

#include "SC_Prototypes.h"
#include "SC_Unit.h"
#include "SC_UnitDef.h"
#include "SC_Wire.h"

#include <algorithm>
#include <array>
#include <cstdint>

using namespace synthdef_bytes;

namespace {

constexpr int8_t kControl = 1;
constexpr int8_t kAudio = 2;

struct AlignmentProbe : public Unit {};

std::array<std::uintptr_t, 16> gSeen{};
std::size_t gSeenCount = 0;

void record(const float* buffer) {
    if (gSeenCount < gSeen.size())
        gSeen[gSeenCount++] = reinterpret_cast<std::uintptr_t>(buffer);
}

void AlignmentProbe_next(AlignmentProbe* unit, int inNumSamples) {
    for (uint32 i = 0; i < unit->mNumOutputs; ++i)
        std::fill_n(unit->mOutBuf[i], inNumSamples, 0.f);
}

void AlignmentProbe_Ctor(AlignmentProbe* unit) {
    for (uint32 i = 0; i < unit->mNumInputs; ++i)
        record(unit->mInBuf[i]);
    for (uint32 i = 0; i < unit->mNumOutputs; ++i)
        record(unit->mOutBuf[i]);
    SETCALC(AlignmentProbe_next);
    AlignmentProbe_next(unit, 1);
}

// The probe joins the unit table the way a plugin's DefineUnit does.
void defineProbe() {
    UnitDef_Create("AlignmentProbe", sizeof(AlignmentProbe), (UnitCtorFunc)&AlignmentProbe_Ctor, nullptr, 0);
    int32 key[kSCNameLen];
    str4cpy(key, "AlignmentProbe");
    REQUIRE(GetUnitDef(key) != nullptr);
}

// A control and `fillers` DC.kr units, then the probe at `rate`, given a
// constant, the control and each DC's output, with one output of its own.
std::vector<uint8_t> probeDef(const char* name, int8_t rate, int32_t fillers) {
    Def d;
    d.i32(0x53436766).i32(2).i16(1);
    d.str(name);
    d.i32(1).f32(0.5f);                                 // one constant
    d.i32(1).f32(0.25f);                                // one control
    d.i32(1).str("level").i32(0);
    d.i32(2 + fillers);
    d.str("Control").i8(kControl).i32(0).i32(1).i16(0);
    d.i8(kControl);
    for (int32_t f = 0; f < fillers; ++f) {
        d.str("DC").i8(kControl).i32(1).i32(1).i16(0);
        d.i32(-1).i32(0);
        d.i8(kControl);
    }
    d.str("AlignmentProbe").i8(rate).i32(2 + fillers).i32(1).i16(0);
    d.i32(-1).i32(0);
    d.i32(0).i32(0);
    for (int32_t f = 0; f < fillers; ++f)
        d.i32(1 + f).i32(0);
    d.i8(rate);
    d.i16(0);                                           // no variants
    return d.bytes();
}

} // namespace

TEST_CASE("Unit buffers: every input and output a unit is handed starts on the widest vector load",
          "[unit_buffers]") {
    EngineFixture fx;
    defineProbe();

    for (const int8_t rate : {kControl, kAudio}) {
        for (int32_t fillers = 0; fillers < 4; ++fillers) {
            INFO("probe rate " << int(rate) << ", " << (2 + fillers) << " units");
            REQUIRE(loads(fx, probeDef("aligned", rate, fillers)));
            gSeenCount = 0;
            osc_test::Builder b;
            b.begin("/s_new") << "aligned" << 2000 << 0 << 0;
            fx.send(b.end());
            fx.pumpBlock(2);
            REQUIRE(gSeenCount == static_cast<std::size_t>(2 + fillers + 1));
            for (std::size_t i = 0; i < gSeenCount; ++i) {
                INFO("buffer " << i << " at " << gSeen[i]);
                CHECK(gSeen[i] % kSCVectorAlign == 0);
            }
            fx.send(osc_test::message("/n_free", 2000));
            fx.pumpBlock(1);
        }
    }
}

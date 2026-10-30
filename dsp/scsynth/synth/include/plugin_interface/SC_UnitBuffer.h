// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Sam Aaron

#pragma once

#include "SC_FloatToInt.h"
#include "SC_Graph.h"
#include "SC_SndBuf.h"
#include "SC_Unit.h"
#include "SC_World.h"

// The buffer `bufnum` names for `unit`: one of the World's buffers, or,
// numbered after them, one of the local buffers its synth has made. nullptr
// for any other number. A unit's buffer number is an input, so a SynthDef or a
// control can give any value.
inline SndBuf* sc_unit_buffer_at(const Unit* unit, int64 bufnum) {
    const World* world = unit->mWorld;
    if (bufnum < 0)
        return nullptr;
    if (bufnum < (int64)world->mNumSndBufs)
        return world->mSndBufs + bufnum;
    const int64 local = bufnum - (int64)world->mNumSndBufs;
    const Graph* parent = unit->mParent;
    if (local < (int64)parent->localBufNum)
        return parent->mLocalSndBufs + local;
    return nullptr;
}

// The same for a buffer number as a float, NaN and infinities included.
inline SndBuf* sc_unit_buffer(const Unit* unit, float fbufnum) {
    if (!(fbufnum >= 0.f))
        return nullptr;
    return sc_unit_buffer_at(unit, sc_float_to<int64>(fbufnum));
}

/*
    SuperCollider real time audio synthesis system
    Copyright (c) 2002 James McCartney. All rights reserved.
    http://www.audiosynth.com

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
*/


#pragma once

#include "SC_Types.h"

#include <cstddef>

// SuperSonic: the widest load nova-simd makes from a unit's buffer, 32 bytes
// with AVX and 16 with SSE, NEON or wasm SIMD. Every buffer a unit is handed
// starts on it, a wire's single value included.
#if defined(__AVX__)
constexpr std::size_t kSCVectorAlign = 32;
#else
constexpr std::size_t kSCVectorAlign = 16;
#endif

// SuperSonic: mScalarValue first and the wire aligned to kSCVectorAlign, so a
// single value held in its wire starts on a vector load as well.
struct alignas(kSCVectorAlign) Wire {
    float32 mScalarValue;
    int32 mCalcRate;
    struct Unit* mFromUnit;
    float32* mBuffer;
};
typedef struct Wire Wire;

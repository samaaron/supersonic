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

#include <stddef.h>
#include <stdint.h>

// Platform capabilities (SC_HAS_*) and the derived SC_LEAN_TARGET live in
// SC_Platform.h. Included here, in the universal types leaf, so every TU sees the
// same answer (SC_Reply.h, SC_HiddenWorld.h, SC_MiscCmds.cpp, ... all key their
// feature guards off SC_LEAN_TARGET).
#include "SC_Platform.h"

#if __cplusplus
#    define SC_INLINE inline
#else
#    define SC_INLINE static inline
#endif

#ifdef __cplusplus
// unfortunately, we have to use 'bool' for source compatibility with
// existing  C++ plugins, but we make sure that it is indeed 1 byte long.
typedef bool SCBool;
static_assert(sizeof(SCBool) == 1, "unexpected size of 'bool'");
#else
// let's take the chance and use a well-defined type for SCBool.
typedef uint8_t SCBool;
#endif // __cplusplus

enum { kSCTrue = 1, kSCFalse = 0 };

// Use plain int/unsigned, not int32_t/uint32_t. On Xtensa (ESP32) newlib
// int32_t is `long` — still 32-bit, but a *distinct* type from int, which breaks
// the many scsynth signatures written with plain `int`/`SCErr`. int == int32_t
// on desktop/WASM, so this is a no-op there and a portability fix on Xtensa.
typedef int SCErr;

typedef int64_t int64;
typedef uint64_t uint64;

typedef int int32;
typedef unsigned int uint32;

typedef int16_t int16;
typedef uint16_t uint16;

typedef int8_t int8;
typedef uint8_t uint8;

typedef float float32;
typedef double float64;

typedef union {
    uint32 u;
    int32 i;
    float32 f;
} elem32;

typedef union {
    uint64 u;
    int64 i;
    float64 f;
} elem64;

#ifdef __cplusplus
const unsigned int kSCNameLen = 8;
const unsigned int kSCNameByteLen = 8 * sizeof(int32);

// Round n up to the next multiple of a (a must be a power of two). Used by
// the bump allocators in SC_UnitDef / SC_GraphDef / SC_Node / SendReply_Ctor
// to keep 8-byte pointers and 4-byte floats from landing at sub-aligned
// offsets within packed-buffer layouts.
constexpr size_t sc_align_up(size_t n, size_t a) { return (n + a - 1) & ~(a - 1); }
#else
#    define kSCNameLen 8
#    define kSCNameByteLen (8 * sizeof(int32))
#endif

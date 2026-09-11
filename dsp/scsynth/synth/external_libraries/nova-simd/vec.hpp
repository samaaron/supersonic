//  generic vector class
//
//  Copyright (C) 2010 Tim Blechmann
//
//  This program is free software; you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation; either version 2 of the License, or
//  (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program; see the file COPYING.  If not, write to
//  the Free Software Foundation, Inc., 51 Franklin Street - Fifth Floor,
//  Boston, MA 02110-1301, USA.

#ifndef VEC_HPP
#define VEC_HPP

#include "vec/vec_generic.hpp"

#if defined(__wasm_simd128__)
// WASM SIMD128: native 128-bit SIMD for WebAssembly (emscripten)
// This specializes vec<float> with wasm_simd128.h intrinsics,
// replacing the generic scalar fallback.
#  include "vec/vec_wasm128.hpp"
#elif defined(__ARM_NEON__) || defined(__ARM_NEON) || defined(_M_ARM64)
#  include "vec/vec_neon.hpp"
#elif defined(__AVX__)
#  include "vec/vec_avx_float.hpp"
#  include "vec/vec_avx_double.hpp"
#elif defined(__SSE__)
#  include "vec/vec_sse.hpp"
#  ifdef __SSE2__
#    include "vec/vec_sse2.hpp"
#  endif
#endif

#ifdef __ALTIVEC__
#  include "vec/vec_altivec.hpp"
#endif

namespace nova
{

template <typename T>
T max_(T const & left, T const & right)
{
    return std::max(left, right);
}

template <typename T>
T min_(T const & left, T const & right)
{
    return std::min(left, right);
}

}

#endif /* VEC_HPP */

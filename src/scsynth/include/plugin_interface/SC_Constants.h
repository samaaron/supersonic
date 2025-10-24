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

#include <cmath>

// CRITICAL FIX for --no-entry WASM builds:
// In standalone WASM builds without an entry point, static constructors don't run.
// This means const globals that depend on function calls like std::acos(), std::log(), etc.
// remain zero instead of being initialized. We must use constexpr or #define for constants.

#ifdef __EMSCRIPTEN__
// For WASM builds, use compile-time constexpr values
#ifndef __FP__
constexpr double pi = 3.141592653589793238462643383279502884;
#else
constexpr double sc_pi = 3.141592653589793238462643383279502884;
#    define pi sc_pi
#endif

/// pi / 2
constexpr double pi2 = 1.570796326794896619231321691639751442;

/// 3pi / 2
constexpr double pi32 = 4.712388980384689857693965074919254327;

/// 2pi
constexpr double twopi = 6.283185307179586476925286766559005768;

/// 1/2pi
constexpr double rtwopi = 0.159154943091895335768883763372514362;

/// log(0.001)
constexpr double log001 = -6.907755278982137052053974364242476164;

/// log(0.01)
constexpr double log01 = -4.605170185988091368035982548330541771;

/// log(0.1)
constexpr double log1 = -2.302585092994045684017991454684364207;

/// 1/log(2)
constexpr double rlog2 = 1.442695040888963407359924681001892137;

/// sqrt(2)
constexpr double sqrt2 = 1.414213562373095048801688724209698079;

/// 1/sqrt(2)
constexpr double rsqrt2 = 0.707106781186547524400844362104849039;

/// pi as float
constexpr float pi_f = 3.141592653589793238462643383279502884f;

/// pi / 2
constexpr float pi2_f = 1.570796326794896619231321691639751442f;

/// 3pi / 2
constexpr float pi32_f = 4.712388980384689857693965074919254327f;

/// 2pi
constexpr float twopi_f = 6.283185307179586476925286766559005768f;

/// sqrt(2)
constexpr float sqrt2_f = 1.414213562373095048801688724209698079f;

/// 1/sqrt(2)
constexpr float rsqrt2_f = 0.707106781186547524400844362104849039f;

/// used to truncate precision
constexpr float truncFloat = 12582912.0f;  // 3 * 2^22
constexpr double truncDouble = 6755399441055744.0;  // 3 * 2^51

#else
// For native builds, use runtime computation (original code)
#ifndef __FP__
const double pi = std::acos(-1.);
#else
const double sc_pi = std::acos(-1.);
#    define pi sc_pi // hack to avoid osx warning about deprecated pi
#endif

/// pi / 2
const double pi2 = pi * .5;

/// 3pi / 2
const double pi32 = pi * 1.5;

/// 2pi
const double twopi = pi * 2.;

/// 1/2pi
const double rtwopi = 1. / twopi;

/// log(0.001)
const double log001 = std::log(0.001);

/// log(0.01)
const double log01 = std::log(0.01);

/// log(0.1)
const double log1 = std::log(0.1);

/// 1/log(2)
const double rlog2 = 1. / std::log(2.);

/// sqrt(2)
const double sqrt2 = std::sqrt(2.);

/// 1/sqrt(2)
const double rsqrt2 = 1. / sqrt2;

/// pi as float
const float pi_f = std::acos(-1.f);

/// pi / 2
const float pi2_f = pi_f * 0.5f;

/// 3pi / 2
const float pi32_f = pi_f * 1.5f;

/// 2pi
const float twopi_f = pi_f * 2.f;

/// sqrt(2)
const float sqrt2_f = std::sqrt(2.f);

/// 1/sqrt(2)
const float rsqrt2_f = 1.f / std::sqrt(2.f);

/// used to truncate precision
const float truncFloat = (float)(3. * std::pow(2.0, 22));
const double truncDouble = 3. * std::pow(2.0, 51);

#endif // __EMSCRIPTEN__

/// used in the secant table for values very close to 1/0
const float kBadValue = 1e20f;

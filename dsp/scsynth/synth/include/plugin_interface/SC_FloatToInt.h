// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Sam Aaron

#pragma once

#include "SC_Types.h"

#include <limits>
#include <type_traits>

#if defined(__aarch64__) || defined(_M_ARM64)
#    include <arm_neon.h>
#endif

// A float as an integer of type Int, truncated toward zero as a cast is, held
// to Int's range, and NaN as 0. Converting a float that Int cannot hold is
// undefined, and a control, an input or an OSC argument can hold any float, so
// every float that becomes an integer goes through this.
template <typename Int, typename Float> inline Int sc_float_to(Float f) {
    static_assert(std::is_integral<Int>::value && std::is_floating_point<Float>::value, "float to integer");
    // 2^digits: one past Int's largest value, exact in any float type.
    constexpr Float past = static_cast<Float>(std::numeric_limits<Int>::max() / 2 + 1) * 2;
    if (!(f == f))
        return 0;
    if (f >= past)
        return std::numeric_limits<Int>::max();
    if (std::is_signed<Int>::value ? f <= -past : f <= 0)
        return std::numeric_limits<Int>::min();
    return static_cast<Int>(f);
}

// The same for int32 from a float, in one instruction where the hardware
// conversion already saturates and takes NaN to 0.
inline int32 sc_float_to_int32(float f) {
#if defined(__aarch64__) || defined(_M_ARM64)
    return vcvts_s32_f32(f);
#elif defined(__wasm_nontrapping_fptoint__)
    return __builtin_wasm_trunc_saturate_s_i32_f32(f);
#else
    return sc_float_to<int32>(f);
#endif
}

inline int32 sc_float_to_int32(double f) { return sc_float_to<int32>(f); }

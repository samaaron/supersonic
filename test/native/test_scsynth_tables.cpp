// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Sam Aaron
/*
 * test_scsynth_tables.cpp — scsynth's constant lookup tables are what upstream
 * computes at load.
 *
 * scsynth_tables.cpp is generated (scripts/gen-scsynth-tables.sh) from the
 * formulas upstream fills its tables with: Samp.cpp's SynthLibInit,
 * SC_Complex.h's initTables, SC_fftlib.cpp's windows and cosine tables. This
 * computes each here, with this platform's libm, and holds the data to it
 * within one float ulp: a libm may differ from the generating one in the last
 * bit of a double, which reaches the float's rounding only at the edge.
 */
#include <catch2/catch_test_macros.hpp>

#include "scsynth_tables.h"
#include "SC_Constants.h"

#include <cmath>
#include <cstdint>
#include <cstring>

namespace {

bool withinOneUlp(float a, float b) {
    if (a == b) return true;
    if (std::isnan(a) || std::isnan(b)) return false;
    return std::nextafter(a, b) == b;
}

// Checks every entry, and names the first that is off.
#define CHECK_TABLE(table, count, expected)                                                                          \
    do {                                                                                                               \
        size_t off = 0, firstOff = SIZE_MAX;                                                                           \
        for (size_t i = 0; i < (count); ++i)                                                                           \
            if (!withinOneUlp((table)[i], (expected)(i))) {                                                            \
                if (firstOff == SIZE_MAX) firstOff = i;                                                                \
                ++off;                                                                                                 \
            }                                                                                                          \
        INFO(#table << ": " << off << " entries off, the first at " << firstOff);                                      \
        CHECK(off == 0);                                                                                               \
    } while (0)

// SC_fftlib.cpp's own constants: float literals, promoted in the arithmetic.
constexpr float kFftPi = 3.1415926535898f;
constexpr float kFftTwoPi = 6.28318530717952646f;

} // namespace

TEST_CASE("scsynth tables: the sine tables are Samp.cpp's", "[tables]") {
    const double twopi = std::acos(-1.0) * 2.0;
    const double sineIndexToPhase = twopi / kSineSize;
    const double pmf = (1L << 29) / twopi;
    auto sine = [&](size_t i) { return (float)std::sin(i * sineIndexToPhase); };
    CHECK_TABLE(gSine, kSineSize + 1, sine);
    CHECK_TABLE(gPMSine, kSineSize + 1, [&](size_t i) { return (float)(sine(i) * pmf); });

    auto invSine = [&](size_t i) {
        const long sz = kSineSize, sz2 = sz >> 1;
        const long n = (long)i;
        const bool bad = n == 0 || n == sz2 || n == sz || (n >= 1 && n <= 8) || (n >= sz - 8 && n <= sz - 1)
            || (n >= sz2 - 8 && n <= sz2 - 1) || (n >= sz2 + 1 && n <= sz2 + 8);
        return bad ? kBadValue : (float)(1. / sine(i));
    };
    CHECK_TABLE(gInvSine, kSineSize + 1, invSine);

    // SignalAsWavetable over gSine: pairs of (2a - b, b - a), the last wrapping to the first.
    auto wavetable = [&](size_t i) {
        const size_t k = i / 2;
        const float a = gSine[k], b = gSine[k + 1 < (size_t)kSineSize ? k + 1 : 0];
        return i % 2 == 0 ? 2.f * a - b : b - a;
    };
    CHECK_TABLE(gSineWavetable, 2 * kSineSize, wavetable);
}

TEST_CASE("scsynth tables: the polar lookup tables are SC_Complex.h's", "[tables]") {
    const double rPolarLUTSize2 = 1. / detail::kPolarLUTSize2;
    auto angle = [&](size_t i) { return std::atan(((double)i - detail::kPolarLUTSize2) * rPolarLUTSize2); };
    CHECK_TABLE(detail::gPhaseLUT, detail::kPolarLUTSize, [&](size_t i) { return (float)angle(i); });
    CHECK_TABLE(detail::gMagLUT, detail::kPolarLUTSize, [&](size_t i) { return (float)(1.f / std::cos(angle(i))); });
    CHECK(static_cast<const void*>(detail::gSine) == static_cast<const void*>(gSine));
}

TEST_CASE("scsynth tables: the FFT windows and cosine tables are SC_fftlib.cpp's, every size", "[tables]") {
    for (int log2n = SC_FFT_LOG2_MINSIZE; log2n <= SC_FFT_LOG2_MAXSIZE; ++log2n) {
        INFO("size 2^" << log2n);
        const int size = 1 << log2n;
        const float* sineWin = gFftSineWindows + scfft_window_offset(log2n);
        const float* hannWin = gFftHannWindows + scfft_window_offset(log2n);
        const float* cosTable = gFftCosTables + scfft_cos_offset(log2n);
        const double sineInc = kFftPi / size;
        const double hannInc = kFftTwoPi / size;
        CHECK_TABLE(sineWin, (size_t)size, [&](size_t i) { return (float)std::sin(i * sineInc); });
        CHECK_TABLE(hannWin, (size_t)size, [&](size_t i) { return (float)(0.5 - 0.5 * std::cos(i * hannInc)); });
        CHECK_TABLE(cosTable, (size_t)(size / 4 + 1), [&](size_t i) { return (float)std::cos(i * hannInc); });
    }
    // The layout closes: the last window ends the store, the last table its.
    CHECK(scfft_window_offset(SC_FFT_LOG2_MAXSIZE) + (size_t(1) << SC_FFT_LOG2_MAXSIZE) == scfft_window_floats());
    CHECK(scfft_cos_offset(SC_FFT_LOG2_MAXSIZE) + scfft_cos_floats(SC_FFT_LOG2_MAXSIZE)
          == scfft_cos_offset(SC_FFT_LOG2_MAXSIZE + 1));
}

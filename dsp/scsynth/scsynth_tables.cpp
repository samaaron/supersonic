// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
#include "scsynth_tables.h"
#include "scsynth_options.h"
#include "Samp.hpp"
#include "SC_Complex.h"
#include "SC_fftlib.h"

#include <cmath>
#include <cstdint>

namespace detail {
// The polar LUTs and the sine table SC_Complex.h's inline functions read: one set, placed below.
float* gMagLUT = nullptr;
float* gPhaseLUT = nullptr;
float* gSine = nullptr;

// SC_Complex.h's initTables, over the placed tables.
static size_t complex_tables_place(float* base) {
    constexpr size_t kSineFloats = (kSineSize + 1 + 3) & ~3;           // 8193, rounded to keep the next 16-aligned
    constexpr size_t kLutFloats = (kPolarLUTSize + 3) & ~3;             // 2049, likewise
    gSine = base;
    gMagLUT = base + kSineFloats;
    gPhaseLUT = gMagLUT + kLutFloats;
    const double sineIndexToPhase = twopi / kSineSize;
    for (int i = 0; i <= kSineSize; ++i) gSine[i] = (float)std::sin(i * sineIndexToPhase);
    const double rPolarLUTSize2 = 1. / kPolarLUTSize2;
    for (int i = 0; i < kPolarLUTSize; ++i) {
        const double angle = std::atan((i - kPolarLUTSize2) * rPolarLUTSize2);
        gPhaseLUT[i] = (float)angle;
        gMagLUT[i] = (float)(1.f / std::cos(angle));
    }
    return kSineFloats + 2 * kLutFloats;
}
} // namespace detail

namespace {
// Each part's floats, as the parts place them (every part keeps 16-byte alignment on its own).
constexpr size_t kSampFloats = 3 * ((kSineSize + 1 + 3) & ~3) + 2 * kSineSize;          // gSine, gPMSine, gInvSine, gSineWavetable
constexpr size_t kComplexFloats = ((detail::kSineSize + 1 + 3) & ~3) + 2 * ((detail::kPolarLUTSize + 3) & ~3);
constexpr size_t kTotalFloats = kSampFloats + kComplexFloats + scfft_tables_floats();
static_assert(kTotalFloats * sizeof(float) <= SCSYNTH_TABLES_BYTES, "SCSYNTH_TABLES_BYTES (scsynth_options.h) is less than the tables take");
}

size_t scsynth_tables_bytes(void) { return kTotalFloats * sizeof(float); }

void scsynth_tables_place(void* base) {
    float* at = static_cast<float*>(base);
    at += samp_tables_place(at);
    at += detail::complex_tables_place(at);
    at += scfft_tables_place(at);
}

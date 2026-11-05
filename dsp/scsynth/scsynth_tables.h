// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//
// scsynth's lookup tables, as constants.
//
// Upstream computes them at load: the sine tables in Samp.cpp, the polar
// LUTs in SC_Complex.h (one copy per translation unit) and the FFT windows
// and cosine tables in SC_fftlib.cpp, each into a global or a malloc. A guest
// any number of which can run in one process keeps nothing process-wide
// (clockwork's docs/BOUNDARY.md), and a table filled at load is a mutable
// static. These are the same values as constants: computed once by
// scripts/gen-scsynth-tables.cpp with upstream's formulas, written to
// scsynth_tables.cpp by scripts/gen-scsynth-tables.sh, and held to the
// formulas by test/native/test_scsynth_tables.cpp. Every instance reads the
// one copy, which is the program's.
//
// The sine tables are declared in Samp.hpp and the polar LUTs in
// SC_Complex.h, under their upstream names; the FFT stores are declared here
// and laid out as SC_fftlib.h says.
#pragma once

#include "Samp.hpp"
#include "SC_Complex.h"
#include "SC_fftlib.h"

// The sine (Welch) and Hann windows for every size from 2^SC_FFT_LOG2_MINSIZE to
// 2^SC_FFT_LOG2_MAXSIZE, end to end (scfft_window_offset), and the Green FFT's
// cosine quarter-tables likewise (scfft_cos_offset).
extern const float gFftSineWindows[scfft_window_floats()];
extern const float gFftHannWindows[scfft_window_floats()];
extern const float gFftCosTables[scfft_cos_offset(SC_FFT_LOG2_MAXSIZE + 1)];

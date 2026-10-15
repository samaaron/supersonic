/*
SC_fftlib.h
An interface to abstract over different FFT libraries, for SuperCollider 3.
Copyright (c) 2008 Dan Stowell. All rights reserved.

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

// These specify the min & max FFT sizes expected (used when creating windows, also allocating some other arrays).
// SC_FFT_LOG2_MAXSIZE bounds the window + twiddle tables that
// scfft_global_initialization() precomputes eagerly at static-init (~0.6 MB at
// the default 15 / 32768-point transform). It is overridable so a memory-
// constrained build can cap it (e.g. -DSC_FFT_LOG2_MAXSIZE=11 for 2048-point,
// ~40 KB of tables); larger transforms are still created lazily on first use.
#define SC_FFT_MINSIZE 8
#define SC_FFT_LOG2_MINSIZE 3
#ifndef SC_FFT_LOG2_MAXSIZE
#define SC_FFT_LOG2_MAXSIZE 15
#endif
#define SC_FFT_MAXSIZE (1 << (SC_FFT_LOG2_MAXSIZE))


// Note that things like *fftWindow actually allow for other sizes, to be created on user request.
#define SC_FFT_ABSOLUTE_MAXSIZE 262144
#define SC_FFT_LOG2_ABSOLUTE_MAXSIZE 18

#ifdef CLOCKWORK_GUEST
#include <stddef.h>
// The layout of the windows and cosine tables in the guest's working memory (scsynth_tables.h): a window of
// 2^k is 2^k floats, size 2^n's starts after the smaller ones (2^n − 2^MIN); a cosine table of 2^k is
// 2^k/4 + 1 floats, rounded up to four so each starts 16-aligned.
constexpr size_t scfft_window_offset(int log2n) { return (size_t(1) << log2n) - (size_t(1) << SC_FFT_LOG2_MINSIZE); }
constexpr size_t scfft_window_floats() { return scfft_window_offset(SC_FFT_LOG2_MAXSIZE + 1); }
constexpr size_t scfft_cos_floats(int log2k) { return ((size_t(1) << log2k) / 4 + 1 + 3) & ~size_t(3); }
constexpr size_t scfft_cos_offset(int log2n) { size_t at = 0; for (int k = SC_FFT_LOG2_MINSIZE; k < log2n; ++k) at += scfft_cos_floats(k); return at; }
// Floats the whole store takes: two window types, then the cosine tables.
constexpr size_t scfft_tables_floats() { return 2 * scfft_window_floats() + scfft_cos_offset(SC_FFT_LOG2_MAXSIZE + 1); }
// Point the store at `base` and build every table; the floats taken (scfft_tables_floats()).
size_t scfft_tables_place(float* base);
#endif
#define SC_FFT_LOG2_ABSOLUTE_MAXSIZE_PLUS1 19

// The eager-init loops and the fftWindow/cosTable arrays are sized to the
// absolute max, so a raised SC_FFT_LOG2_MAXSIZE must not exceed it.
#if SC_FFT_LOG2_MAXSIZE > SC_FFT_LOG2_ABSOLUTE_MAXSIZE
#error "SC_FFT_LOG2_MAXSIZE exceeds SC_FFT_LOG2_ABSOLUTE_MAXSIZE"
#endif

struct scfft;

typedef void* (*SCFFT_AllocFunc)(void* user, size_t size);
typedef void (*SCFFT_FreeFunc)(void* user, void* ptr);

struct SCFFT_Allocator {
    SCFFT_AllocFunc mAlloc;
    SCFFT_FreeFunc mFree;
    void* mUser;
};

enum SCFFT_Direction { kForward = 1, kBackward = 0 };

// These values are referred to from SC lang as well as in the following code - do not rearrange!
enum SCFFT_WindowFunction { kRectWindow = -1, kSineWindow = 0, kHannWindow = 1 };

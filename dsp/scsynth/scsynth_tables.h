// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//
// scsynth's lookup tables, in its working memory.
//
// Upstream keeps them as globals (the sine tables in Samp.cpp, the polar LUTs
// in SC_Complex.h, one copy per translation unit) and mallocs the FFT windows
// at load. Under clockwork a guest's fixed state is its working memory
// (DspConfig::memory): clockwork reserves it, tiers it and names it as the
// guest's; a global lives wherever the linker put the program and a malloc
// wherever the allocator did, and neither can be moved or named. So the
// tables are built here, at the start of that memory, on every World_New
// (the memory is zeroed for each), and SCSYNTH_TABLES_BYTES in
// scsynth_options.h is what dsp_memory_bytes adds for them.
#pragma once
#include <stddef.h>

// Bytes the tables take; SCSYNTH_TABLES_BYTES covers it, and the .cpp checks that at compile time.
size_t scsynth_tables_bytes(void);

// Place every table at `base` (16-byte aligned, scsynth_tables_bytes() long) and fill it: the sine
// tables, the polar LUTs, the FFT windows and cosine tables. Idempotent per base; every World_New.
void scsynth_tables_place(void* base);

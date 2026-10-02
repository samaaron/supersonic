// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Sam Aaron
/*
 * AudioThreadWitness.h — file work done on the audio thread, counted.
 *
 * The engine has no files. The front reads, decodes and encodes on threads of
 * its own and hands the engine bytes, so no block carries a file. clockwork
 * marks the audio thread while it renders (rt_alloc::g_in_rt, set around every
 * block), and each place the front touches a file asks whether it is on that
 * thread, counting it if so. Nothing should ever count.
 *
 * The tests that say "no block carries the read" read this count. They used to
 * time blocks instead, and a block's wall-clock time on a shared runner
 * measures the runner: a Windows one took 6.8 ms for blocks that carried
 * nothing (0.89.0's release). Where the work ran is the claim; this says it
 * exactly, however fast the machine.
 */
#pragma once

#include "rt_alloc.h"

#include <atomic>
#include <cstdint>

namespace audio_thread_witness {

inline std::atomic<uint64_t> g_fileWork{0};

// Called wherever the front reads, decodes, encodes or writes a file.
inline void fileWork() {
    if (rt_alloc::g_in_rt) g_fileWork.fetch_add(1, std::memory_order_relaxed);
}

// File work the audio thread has done since the process began.
inline uint64_t fileWorkCount() { return g_fileWork.load(std::memory_order_relaxed); }

} // namespace audio_thread_witness

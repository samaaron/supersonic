/*
 * rt_alloc.h — RT-thread allocation detector
 *
 * A guard set inside process_audio() (and any other RT-only scope) that,
 * paired with `operator new`/`delete` overrides in the test binary, counts
 * allocations made on the audio thread. Production builds just write the flag —
 * nothing reads it (the new/delete overrides only exist in the test binary), so
 * the cost is two flag stores per callback, immeasurable against the audio
 * budget.
 *
 * The guard saves and restores the previous flag value so explicit outer
 * guards in tests don't get clobbered by nested process_audio() calls.
 */
#pragma once

#include "SC_Platform.h"  // SC_HAS_HOSTED_OS

#include <atomic>
#include <cstdint>

namespace rt_alloc {

// Only the test binary reads this flag (via its new/delete overrides), and only
// a hosted, multi-threaded build does — there it must be per-thread. Lean self-
// driven targets have no reader, and a bare-metal one (e.g. Teensy) has no TLS
// runtime at all, so a plain flag is both sufficient and portable.
#if SC_HAS_HOSTED_OS
inline thread_local bool g_in_rt = false;
#else
inline bool g_in_rt = false;
#endif
inline std::atomic<int64_t> g_allocs{0};
inline std::atomic<int64_t> g_frees{0};

struct Guard {
    bool prev;
    Guard()  : prev(g_in_rt) { g_in_rt = true; }
    ~Guard() { g_in_rt = prev; }
};

inline void reset() {
    g_allocs.store(0, std::memory_order_relaxed);
    g_frees.store(0, std::memory_order_relaxed);
}

} // namespace rt_alloc

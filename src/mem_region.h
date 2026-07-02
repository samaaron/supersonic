/*
 * mem_region.h — tiered memory placement for SuperSonic
 *
 * SuperSonic runs on devices with one kind of RAM (desktop, WASM) and on
 * devices with two (ESP32-S3: fast internal SRAM + slower bulk PSRAM). This lets
 * allocation sites express *intent* — "this is real-time hot" vs "this is
 * cold/large" — and leaves the mapping of intent to physical memory to a
 * per-platform backend.
 *
 *   Tier::Fast — low-latency memory for the audio-thread-hot data (the RT pool's
 *                initial area, audio buses, inter-UGen wire scratch).
 *                ESP32 → internal SRAM.
 *   Tier::Bulk — large, latency-tolerant memory for cold or late data (sample
 *                buffers, RT-pool overflow). ESP32 → PSRAM.
 *
 * On single-region platforms (desktop, WASM, NIF) both tiers compile straight to
 * std::malloc / std::free with no wrapper — zero overhead and byte-for-byte the
 * behaviour these allocations had before. The tier is then purely documentation
 * of intent that an embedded build acts on.
 */
#pragma once
#include <cstddef>

namespace supersonic::mem {

enum class Tier { Fast = 0, Bulk = 1 };

// Allocate at least 16-byte aligned (SC_MEMORY_ALIGNMENT) memory, or nullptr on
// failure.
//
// allow_spill=true (default): a request the named tier cannot satisfy falls back
// to the other region rather than failing outright. A Fast request landing in
// Bulk also increments spill_count().
//
// allow_spill=false: the request is served from the named tier only, returning
// nullptr if it does not fit. Callers that need Fast placement specifically (the
// RT pool and heap initial areas) use this to notice the failure and pick their
// own fallback, rather than having a too-large request land wholesale in slow
// RAM with nothing reported.
void* alloc(Tier tier, size_t bytes, bool allow_spill = true);

// Release memory returned by alloc(). Tier-agnostic.
void free(void* ptr);

// Bytes currently handed out for a tier, for on-device SRAM budgeting / metrics.
// Single-region platforms don't track this (they have nothing to budget) and
// return 0.
size_t in_use(Tier tier);

// Largest contiguous free block in a tier's backing region, for sizing Fast areas
// at boot from the real (fragmented) SRAM map instead of a compile-time constant
// that can drift from the firmware's memory layout. Returns SIZE_MAX on
// single-region platforms, where any compile-time cap is lower and so wins.
size_t largest_free(Tier tier);

// Number of Fast allocations that transparently fell back to Bulk because Fast
// was exhausted. Non-zero means something meant for internal SRAM is in bulk RAM;
// hosts should surface it, as the symptom is usually render overruns rather than
// an error. Always 0 on single-region platforms.
size_t spill_count();

} // namespace supersonic::mem

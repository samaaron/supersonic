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
// failure. On a backend with a distinct Fast region, a Fast request that cannot
// be satisfied transparently spills to Bulk — graceful degradation rather than a
// hard out-of-memory on the audio path.
void* alloc(Tier tier, size_t bytes);

// Release memory returned by alloc(). Tier-agnostic.
void free(void* ptr);

// Bytes currently handed out for a tier, for on-device SRAM budgeting / metrics.
// Single-region platforms don't track this (they have nothing to budget) and
// return 0.
size_t in_use(Tier tier);

} // namespace supersonic::mem

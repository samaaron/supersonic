/*
 * SC_Platform.h — capability profile for the active build target.
 *
 * The one place that turns raw platform macros (__EMSCRIPTEN__, ESP_PLATFORM, …)
 * into a small vocabulary of CAPABILITIES. Engine code keys off the capabilities,
 * never the platform macro — so adding a target is "add a profile row" (in
 * SC_PlatformProfile.inc) plus a selection branch here, not "hunt #ifdefs".
 *
 * Public capabilities (each 1 or 0):
 *   SC_HAS_HOSTED_OS      full OS: boost::asio sockets, host filesystem, IPC shm
 *   SC_HAS_BYTE_ATOMICS   std::atomic_flag / __atomic_test_and_set usable
 *   SC_HAS_TIERED_MEMORY  distinct fast/bulk RAM regions (placement allocator)
 * Derived:
 *   SC_LEAN_TARGET        defined iff !SC_HAS_HOSTED_OS (self-driven build)
 *   SC_COLD_BSS           attribute placing large cold static tables in bulk RAM;
 *                         empty where there is a single RAM tier
 */
#pragma once

// ── Target selection ──────────────────────────────────────────────────────────
// SUPERSONIC_FREESTANDING is an explicit build opt-in (the freestanding CI
// guard), checked first so it overrides the desktop auto-detection: it compiles
// the lean/self-driven profile natively. The rest auto-detect from the toolchain.
#if defined(SUPERSONIC_FREESTANDING)
#    define SCP_TARGET_FREESTANDING 1
#elif defined(__EMSCRIPTEN__)
#    define SCP_TARGET_WASM 1
#elif defined(ESP_PLATFORM)
#    define SCP_TARGET_ESP32 1
#else
#    define SCP_TARGET_DESKTOP 1
#endif

// ── Capability values (single source of truth) ────────────────────────────────
#include "SC_PlatformProfile.inc"

#define SC_HAS_HOSTED_OS      SCP_HOSTED_OS
#define SC_HAS_BYTE_ATOMICS   SCP_BYTE_ATOMICS
#define SC_HAS_TIERED_MEMORY  SCP_TIERED_MEMORY

// ── Derived ───────────────────────────────────────────────────────────────────
// Self-driven targets (no hosted OS) build lean: no boost::asio, no cross-process
// shared memory, no host-filesystem synthdef loading. The engine's existing
// feature guards already key off SC_LEAN_TARGET.
#if !SC_HAS_HOSTED_OS
#    define SC_LEAN_TARGET 1
#endif

// Cold-BSS attribute: push large, rarely-touched static tables (FFT/polar LUTs,
// the cold sine wavetables) into bulk RAM so the audio path keeps fast RAM.
// Empty (no-op) on single-tier targets.
#if defined(SCP_TARGET_ESP32)
#    include "esp_attr.h"
#    define SC_COLD_BSS EXT_RAM_BSS_ATTR
#else
#    define SC_COLD_BSS
#endif

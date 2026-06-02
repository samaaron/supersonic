/*
 * supersonic_config.h — Shared definitions for all SuperSonic builds (WASM + native)
 *
 * Both the WASM AudioWorklet build and the native JUCE build share SuperSonic's
 * architecture: NRT mode, ring buffer communication, pre-allocated memory,
 * no malloc on the audio thread. This header provides common declarations
 * used across both platforms.
 */

#pragma once

// ─── Version ─────────────────────────────────────────────────────────────────
// Single source of truth for all builds (WASM, native exe, NIF).
// Updated by scripts/bump-version.sh.
#define SUPERSONIC_VERSION_MAJOR 0
#define SUPERSONIC_VERSION_MINOR 68
#define SUPERSONIC_VERSION_PATCH 0

// String form for CLI / banners (e.g. "0.64.0")
#define SUPERSONIC_STRINGIFY2(x) #x
#define SUPERSONIC_STRINGIFY(x) SUPERSONIC_STRINGIFY2(x)
#define SUPERSONIC_VERSION_STRING \
    SUPERSONIC_STRINGIFY(SUPERSONIC_VERSION_MAJOR) "." \
    SUPERSONIC_STRINGIFY(SUPERSONIC_VERSION_MINOR) "." \
    SUPERSONIC_STRINGIFY(SUPERSONIC_VERSION_PATCH)

// Pre-allocated heap size for RT-safe allocations (used by supersonic_heap).
// Default 64MB for native builds (unused in WASM where emscripten manages
// memory); sized per device via memory_profile.h.
#include "memory_profile.h"

#ifdef __cplusplus
extern "C" {
#endif

// Debug logging — writes to the debug ring buffer (DEBUG_BUFFER) on both WASM
// and native; the consumer drains it via the debug channel. No-ops until
// memory is initialised. printf-compatible signature. Note: this does NOT go
// to stderr — native host logging uses fprintf(stderr) directly.
int ss_log(const char* fmt, ...);

// Table initialization — must be called explicitly before World_New.
// In WASM --no-entry builds, static constructors don't run.
// On native, calling these is harmless (idempotent double-init).
void InitializeSynthTables(void);
void InitializeFFTTables(void);

#ifdef __cplusplus
}
#endif

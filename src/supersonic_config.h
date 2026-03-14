/*
 * supersonic_config.h — Shared definitions for all SuperSonic builds (WASM + native)
 *
 * Both the WASM AudioWorklet build and the native JUCE build share SuperSonic's
 * architecture: NRT mode, ring buffer communication, pre-allocated memory,
 * no malloc on the audio thread. This header provides common declarations
 * used across both platforms.
 */

#pragma once

// Pre-allocated heap size for RT-safe allocations (used by supersonic_heap).
// 64MB matches the WASM linear memory buffer pool region.
#ifndef SUPERSONIC_HEAP_SIZE
#define SUPERSONIC_HEAP_SIZE (64 * 1024 * 1024)
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Debug logging — available on both WASM (writes to debug ring buffer) and
// native (forwards to JUCE logger / stderr). printf-compatible signature.
int worklet_debug(const char* fmt, ...);

// Table initialization — must be called explicitly before World_New.
// In WASM --no-entry builds, static constructors don't run.
// On native, calling these is harmless (idempotent double-init).
void InitializeSynthTables(void);
void InitializeFFTTables(void);

#ifdef __cplusplus
}
#endif

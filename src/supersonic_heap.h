/*
 * supersonic_heap.h — Growable heap for RT-safe allocations
 *
 * On WASM, emscripten's malloc already operates on pre-allocated linear memory,
 * so these are inline passthroughs with zero overhead.
 *
 * On native, a global AllocPool instance provides a heap from a system-malloc'd
 * block, with automatic growth when exhausted. All allocations are RT-safe once
 * the memory is allocated (growth happens on the SampleLoader thread, not RT).
 */

#pragma once
#include <cstddef>

#ifdef __EMSCRIPTEN__
// WASM: emscripten's malloc already operates on pre-allocated linear memory
#include "scsynth/common/malloc_aligned.hpp"
inline void   supersonic_heap_init(size_t) {}
inline void*  supersonic_heap_alloc(size_t size) { return nova::malloc_aligned(size); }
inline void   supersonic_heap_free(void* ptr) { nova::free_aligned(ptr); }
inline void   supersonic_heap_destroy() {}
inline size_t supersonic_heap_total_allocated() { return 0; }
inline size_t supersonic_heap_growth_count() { return 0; }
#else
// Native: growable pool (implemented in supersonic_heap.cpp)
void   supersonic_heap_init(size_t bytes);
void*  supersonic_heap_alloc(size_t bytes);
void   supersonic_heap_free(void* ptr);
void   supersonic_heap_destroy();
size_t supersonic_heap_total_allocated();
size_t supersonic_heap_growth_count();
#endif

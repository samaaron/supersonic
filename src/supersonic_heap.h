/*
 * supersonic_heap.h — Pre-allocated heap for RT-safe allocations
 *
 * On WASM, emscripten's malloc already operates on pre-allocated linear memory,
 * so these are inline passthroughs with zero overhead.
 *
 * On native, a global AllocPool instance provides a fixed-size heap from a
 * single system-malloc'd block, making all allocations RT-safe.
 */

#pragma once
#include <cstddef>

#ifdef __EMSCRIPTEN__
// WASM: emscripten's malloc already operates on pre-allocated linear memory
#include "scsynth/common/malloc_aligned.hpp"
inline void  supersonic_heap_init(size_t) {}
inline void* supersonic_heap_alloc(size_t size) { return nova::malloc_aligned(size); }
inline void  supersonic_heap_free(void* ptr) { nova::free_aligned(ptr); }
inline void  supersonic_heap_destroy() {}
#else
// Native: pre-allocated pool (implemented in supersonic_heap.cpp)
void  supersonic_heap_init(size_t bytes);
void* supersonic_heap_alloc(size_t bytes);
void  supersonic_heap_free(void* ptr);
void  supersonic_heap_destroy();
#endif

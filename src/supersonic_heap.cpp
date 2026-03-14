/*
 * supersonic_heap.cpp — Native pre-allocated heap using AllocPool
 *
 * Creates a single fixed-size AllocPool from a system-malloc'd block at init
 * time. All subsequent allocations via supersonic_heap_alloc/free use this pool
 * instead of system malloc, making them RT-safe.
 *
 * Thread safety: a std::atomic_flag spinlock protects AllocPool operations.
 * Contention is minimal — only buffer commands (/b_alloc, /b_allocRead) and
 * SampleLoader touch this, both infrequently.
 */

#include "supersonic_heap.h"
#include "SC_AllocPool.h"
#include <atomic>
#include <cstdlib>
#include <cstdio>

static AllocPool* g_heap_pool = nullptr;
static void*      g_heap_backing = nullptr;
static std::atomic_flag g_heap_lock = ATOMIC_FLAG_INIT;

// AllocPool callbacks — NewAreaFunc / FreeAreaFunc
// The pool is fixed-size (areaMoreSize=0), so these are only called once at init
// and once at destroy. The NewAreaFunc receives the backing block we already allocated.

static void* g_pending_area = nullptr;

static void* heap_new_area(size_t size) {
    // Return the pre-allocated backing block (only called once during AllocPool init)
    void* ptr = g_pending_area;
    g_pending_area = nullptr;
    return ptr;
}

static void heap_free_area(void* ptr) {
    // No-op — we free the backing block in supersonic_heap_destroy()
}

void supersonic_heap_init(size_t bytes) {
    if (g_heap_pool) {
        // Pool exists — reset it (all old allocations are abandoned since
        // a fresh World_New is about to run). No need to reallocate.
        g_heap_pool->FreeAllInternal();
        return;
    }

    // AllocPool::NewArea requests areaInitSize + kAreaOverhead from the callback,
    // so we must allocate enough to cover both the usable pool and the overhead.
    size_t total = bytes + kAreaOverhead;
    g_heap_backing = std::malloc(total);
    if (!g_heap_backing) {
        fprintf(stderr, "supersonic_heap_init: failed to allocate %zu bytes\n", total);
        return;
    }

    // AllocPool::NewArea will call heap_new_area, which returns this block
    g_pending_area = g_heap_backing;

    // areaMoreSize=0 means fixed-size, non-growable pool
    g_heap_pool = new AllocPool(heap_new_area, heap_free_area, bytes, 0);
}

void* supersonic_heap_alloc(size_t bytes) {
    if (!g_heap_pool)
        return nullptr;

    // Spinlock — contention is minimal (buffer ops only)
    while (g_heap_lock.test_and_set(std::memory_order_acquire)) {
        // spin
    }

    void* ptr = g_heap_pool->Alloc(bytes);

    g_heap_lock.clear(std::memory_order_release);

    return ptr;
}

void supersonic_heap_free(void* ptr) {
    if (!ptr || !g_heap_pool)
        return;

    while (g_heap_lock.test_and_set(std::memory_order_acquire)) {
        // spin
    }

    g_heap_pool->Free(ptr);

    g_heap_lock.clear(std::memory_order_release);
}

void supersonic_heap_destroy() {
    if (g_heap_pool) {
        delete g_heap_pool;
        g_heap_pool = nullptr;
    }
    if (g_heap_backing) {
        std::free(g_heap_backing);
        g_heap_backing = nullptr;
    }
}

/*
 * supersonic_heap.cpp — Native growable heap using AllocPool
 *
 * Creates an AllocPool whose backing memory comes from the tiered placement
 * allocator (mem_region.h): the initial area from Tier::Fast, growth areas from
 * Tier::Bulk. On desktop/WASM/NIF both tiers are plain std::malloc, so the pool
 * is one region and behaviour is unchanged; an embedded build maps Fast to
 * internal SRAM (the boot-time hot set) and grows into Bulk (PSRAM).
 *
 * All allocations via supersonic_heap_alloc/free use this pool instead of
 * system malloc, making them RT-safe once allocated.
 *
 * Thread safety: a std::atomic_flag spinlock protects AllocPool operations.
 * Contention is minimal — only buffer commands (/b_alloc, /b_allocRead) and
 * SampleLoader touch this, both infrequently.
 */

#include "supersonic_heap.h"
#include "SC_AllocPool.h"
#include "memory_profile.h"
#include "mem_region.h"
#include "SC_Platform.h"
#include <atomic>
#include <cstdlib>
#include <cstdio>
#include <vector>

// Growth increment when the pool is exhausted. Default 16MB; sized per device
// via memory_profile.h (the ESP32-S3 profile shrinks it to 256KB).
static constexpr size_t HEAP_GROWTH_SIZE = SUPERSONIC_HEAP_GROWTH_SIZE;

static AllocPool* g_heap_pool = nullptr;
static void*      g_heap_backing = nullptr;

// Spinlock protecting AllocPool operations. The Xtensa toolchain provides
// 32-bit atomics (S32C1I) but not std::atomic_flag's byte test-and-set
// (__atomic_test_and_set), so embedded uses a 4-byte atomic. Desktop/WASM keep
// std::atomic_flag, byte-for-byte as before.
#if !SC_HAS_BYTE_ATOMICS
static std::atomic<uint32_t> g_heap_lock{0};
static inline void heap_lock() {
    uint32_t expected = 0;
    while (!g_heap_lock.compare_exchange_weak(expected, 1u, std::memory_order_acquire,
                                              std::memory_order_relaxed))
        expected = 0;
}
static inline void heap_unlock() { g_heap_lock.store(0, std::memory_order_release); }
#else
static std::atomic_flag g_heap_lock = ATOMIC_FLAG_INIT;
static inline void heap_lock() {
    while (g_heap_lock.test_and_set(std::memory_order_acquire)) { /* spin */ }
}
static inline void heap_unlock() { g_heap_lock.clear(std::memory_order_release); }
#endif

// Track dynamically allocated growth areas for cleanup
static std::vector<void*> g_extra_areas;
static size_t g_initial_size = 0;
static size_t g_total_allocated = 0;
static size_t g_growth_count = 0;

// AllocPool callbacks — NewAreaFunc / FreeAreaFunc
// First call returns the pre-allocated backing block. Subsequent calls (when
// areaMoreSize > 0 and the pool is exhausted) malloc new areas on demand.

static void* g_pending_area = nullptr;

static void* heap_new_area(size_t size) {
    if (g_pending_area) {
        // Initial allocation — return the pre-allocated backing block
        void* ptr = g_pending_area;
        g_pending_area = nullptr;
        return ptr;
    }

    // Growth allocation — Bulk tier (PSRAM on embedded, malloc on desktop)
    void* ptr = supersonic::mem::alloc(supersonic::mem::Tier::Bulk, size);
    if (ptr) {
        g_extra_areas.push_back(ptr);
        g_total_allocated += size;
        g_growth_count++;
    }
    return ptr;
}

static void heap_free_area(void* ptr) {
    // AllocPool calls this when an entire area becomes empty.
    // Don't free the initial backing block (freed in supersonic_heap_destroy).
    if (ptr == g_heap_backing)
        return;

    // Free growth areas and remove from tracking
    for (auto it = g_extra_areas.begin(); it != g_extra_areas.end(); ++it) {
        if (*it == ptr) {
            supersonic::mem::free(ptr);
            g_extra_areas.erase(it);
            return;
        }
    }
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
    // Initial area — Fast tier (internal SRAM on embedded, malloc on desktop)
    g_heap_backing = supersonic::mem::alloc(supersonic::mem::Tier::Fast, total);
    if (!g_heap_backing) {
        fprintf(stderr, "supersonic_heap_init: failed to allocate %zu bytes\n", total);
        return;
    }

    g_initial_size = total;
    g_total_allocated = total;
    g_growth_count = 0;

    // AllocPool::NewArea will call heap_new_area, which returns this block
    g_pending_area = g_heap_backing;

    // areaMoreSize > 0 enables automatic growth when the pool is exhausted
    g_heap_pool = new AllocPool(heap_new_area, heap_free_area, bytes, HEAP_GROWTH_SIZE);
}

void* supersonic_heap_alloc(size_t bytes) {
    if (!g_heap_pool)
        return nullptr;

    // Spinlock — contention is minimal (buffer ops only)
    heap_lock();

    void* ptr = g_heap_pool->Alloc(bytes);

    heap_unlock();

    return ptr;
}

void supersonic_heap_free(void* ptr) {
    if (!ptr || !g_heap_pool)
        return;

    heap_lock();

    g_heap_pool->Free(ptr);

    heap_unlock();
}

void supersonic_heap_destroy() {
    if (g_heap_pool) {
        delete g_heap_pool;
        g_heap_pool = nullptr;
    }
    // Free growth areas
    for (void* area : g_extra_areas) {
        supersonic::mem::free(area);
    }
    g_extra_areas.clear();
    // Free initial backing block
    if (g_heap_backing) {
        supersonic::mem::free(g_heap_backing);
        g_heap_backing = nullptr;
    }
    g_total_allocated = 0;
    g_growth_count = 0;
}

size_t supersonic_heap_total_allocated() {
    return g_total_allocated;
}

size_t supersonic_heap_growth_count() {
    return g_growth_count;
}

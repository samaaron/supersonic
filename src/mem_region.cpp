/*
 * mem_region.cpp — tiered memory placement backends. See mem_region.h.
 *
 * Two implementations selected at compile time:
 *   - ESP_PLATFORM: a real two-tier backend (internal SRAM / PSRAM) with a small
 *     per-allocation header so free() is tier-agnostic and usage is accounted.
 *   - everything else (desktop / WASM / NIF): a thin pass-through to
 *     std::malloc / std::free with no header, no accounting, no indirection —
 *     identical to what these allocations did before tiers existed.
 *
 * Native-only translation unit. WASM never reaches here (its supersonic_heap is
 * an inline pass-through and its RT pool uses the SAB pool callbacks).
 */
#include "mem_region.h"
#include "SC_Platform.h"

#include <cstdlib>

#if SC_HAS_TIERED_MEMORY

#include <atomic>
#include <cstdint>
#include "esp_heap_caps.h"

namespace supersonic::mem {
namespace {

// Per-tier bytes currently handed out (requested sizes). Atomic because areas
// are (re)allocated from control/loader threads, never the audio thread.
std::atomic<size_t> g_in_use[2] = {{0}, {0}};

// Every allocation carries a 16-byte header so free() is tier-agnostic and
// accounting stays exact. 16 bytes == one alignment slot, so given a 16-aligned
// base the user pointer (base + 16) is also 16-aligned (SC_MEMORY_ALIGNMENT).
struct alignas(16) Header {
    uint32_t tier;   // Tier value (0 Fast, 1 Bulk)
    uint32_t magic;  // guards free() against foreign pointers
    size_t   bytes;  // requested size, for in_use() accounting
};
static_assert(sizeof(Header) == 16, "Header must occupy exactly one 16-byte slot");
constexpr uint32_t kMagic = 0x4D454D52; // 'MEMR'

// Allocate `total` bytes, >= 16-byte aligned, from the physical memory that
// backs `tier`. Spills to the other region rather than failing outright.
void* backend_alloc(Tier tier, size_t total) {
    const uint32_t caps_fast = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    const uint32_t caps_bulk = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    const uint32_t first = (tier == Tier::Fast) ? caps_fast : caps_bulk;
    const uint32_t second = (tier == Tier::Fast) ? caps_bulk : caps_fast;
    void* p = heap_caps_aligned_alloc(16, total, first);
    if (!p)
        p = heap_caps_aligned_alloc(16, total, second); // graceful spill
    return p;
}

} // namespace

void* alloc(Tier tier, size_t bytes) {
    if (bytes == 0)
        return nullptr;
    void* base = backend_alloc(tier, bytes + sizeof(Header));
    if (!base)
        return nullptr;
    auto* h = static_cast<Header*>(base);
    h->tier = static_cast<uint32_t>(tier);
    h->magic = kMagic;
    h->bytes = bytes;
    g_in_use[h->tier & 1].fetch_add(bytes, std::memory_order_relaxed);
    return static_cast<char*>(base) + sizeof(Header);
}

void free(void* ptr) {
    if (!ptr)
        return;
    auto* h = reinterpret_cast<Header*>(static_cast<char*>(ptr) - sizeof(Header));
    if (h->magic == kMagic) {
        g_in_use[h->tier & 1].fetch_sub(h->bytes, std::memory_order_relaxed);
        heap_caps_free(h);
    } else {
        heap_caps_free(ptr); // not ours (shouldn't happen) — release without underflow
    }
}

size_t in_use(Tier tier) {
    return g_in_use[static_cast<int>(tier) & 1].load(std::memory_order_relaxed);
}

} // namespace supersonic::mem

#else  // single-region platforms (desktop / WASM / NIF)

namespace supersonic::mem {

// std::malloc already returns max_align_t-aligned memory (16 on 64-bit targets),
// satisfying the alignment contract; both tiers map to it with no wrapper.
void* alloc(Tier, size_t bytes) { return bytes ? std::malloc(bytes) : nullptr; }
void free(void* ptr) { std::free(ptr); }
size_t in_use(Tier) { return 0; }

} // namespace supersonic::mem

#endif

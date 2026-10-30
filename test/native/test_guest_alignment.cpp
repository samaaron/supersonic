/*
 * test_guest_alignment.cpp - every block from the guest's heap is 64-byte
 * aligned.
 *
 * scsynth's audio buses, wire buffer space and buffer data come from
 * supersonic_guest_alloc_bytes, and nova-simd reads and writes them with
 * aligned vector loads: 16 bytes with SSE and NEON, 32 with AVX. Upstream
 * allocates them with nova::malloc_aligned, 64-byte aligned.
 */
#include "EngineFixture.h"
#include "buffer_commands.h"

#include <cstddef>
#include <cstdint>

TEST_CASE("Guest memory: every block from the guest's heap is 64-byte aligned", "[memory]") {
    EngineFixture fx;
    for (const size_t bytes : {size_t(1), size_t(16), size_t(17), size_t(63), size_t(64), size_t(100),
                               size_t(1000), size_t(4096), size_t(65536)}) {
        void* p = supersonic_guest_alloc_bytes(bytes);
        REQUIRE(p != nullptr);
        CHECK(reinterpret_cast<uintptr_t>(p) % 64 == 0);
        supersonic_guest_free_bytes(p);
    }
}

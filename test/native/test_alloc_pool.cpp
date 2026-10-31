// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Sam Aaron
/*
 * test_alloc_pool.cpp - the real-time pool refuses a request it cannot hold,
 * however large, and stays sound after. Units ask it for sizes they compute
 * from their inputs, so a SynthDef chooses the size.
 */
#include <catch2/catch_test_macros.hpp>

#include "SC_AllocPool.h"
#include "SC_Wire.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>

namespace {

constexpr size_t kPoolSize = size_t(8) << 20;

void* newArea(size_t size) { return std::malloc(size); }
void freeArea(void* area) { std::free(area); }

} // namespace

TEST_CASE("AllocPool: a request larger than the pool is refused", "[alloc_pool]") {
    AllocPool pool(newArea, freeArea, kPoolSize, 0);
    void* first = pool.Alloc(64);
    REQUIRE(first != nullptr);

    // Past the pool at either width, the half-range limit among them; and on a
    // 64-bit size_t, about 16 GB, whose remainder against a small chunk does not
    // fit an int.
    constexpr size_t kMax = std::numeric_limits<size_t>::max();
    std::vector<size_t> sizes = {kPoolSize * 2, kMax / 2, kMax / 2 + 1, kMax - 8, kMax};
    if (sizeof(size_t) == 8)
        sizes.insert(sizes.end(), {size_t(uint64_t(0x3FFFFFFFC)), size_t(uint64_t(1) << 34)});
    for (const size_t size : sizes) {
        INFO("request " << size);
        CHECK(pool.Alloc(size) == nullptr);
    }

    // Still sound: half the pool can be taken, written and given back.
    void* half = pool.Alloc(kPoolSize / 2);
    REQUIRE(half != nullptr);
    std::memset(half, 1, kPoolSize / 2);
    pool.Free(half);
    pool.Free(first);
    CHECK(pool.LargestFreeChunk() >= kPoolSize / 2);
}

// A synth's memory comes from the pool, and its wires start on a vector load
// only if the block does (SC_Wire.h). The pool's blocks start on 64 bytes with
// 64-bit pointers and 32 bytes past them with 32-bit ones; with the pool off
// they are the C library's, on 64 bytes.
TEST_CASE("AllocPool: every block starts on the widest vector load", "[alloc_pool]") {
    AllocPool pool(newArea, freeArea, kPoolSize, 0);
    std::vector<void*> held;
    for (size_t i = 0; i < 200; ++i) {
        void* block = pool.Alloc(1 + (i * 37) % 3000);
        REQUIRE(block != nullptr);
        INFO("block " << i << " at " << block);
        CHECK(reinterpret_cast<uintptr_t>(block) % kSCVectorAlign == 0);
        held.push_back(block);
    }
    for (void* block : held)
        pool.Free(block);
}

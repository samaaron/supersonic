// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Sam Aaron
/*
 * test_alloc_pool.cpp - the real-time pool refuses a request it cannot hold,
 * however large, and stays sound after. Units ask it for sizes they compute
 * from their inputs, so a SynthDef chooses the size.
 */
#include <catch2/catch_test_macros.hpp>

#include "SC_AllocPool.h"

#include <cstdlib>
#include <cstring>
#include <limits>

namespace {

constexpr size_t kPoolSize = size_t(8) << 20;

void* newArea(size_t size) { return std::malloc(size); }
void freeArea(void* area) { std::free(area); }

} // namespace

TEST_CASE("AllocPool: a request larger than the pool is refused", "[alloc_pool]") {
    AllocPool pool(newArea, freeArea, kPoolSize, 0);
    void* first = pool.Alloc(64);
    REQUIRE(first != nullptr);

    for (const size_t size : {kPoolSize * 2, size_t(0x3FFFFFFFC), size_t(1) << 34,
                              std::numeric_limits<size_t>::max() - 8, std::numeric_limits<size_t>::max()}) {
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

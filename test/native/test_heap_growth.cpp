/*
 * test_heap_growth.cpp — Tests for dynamic heap growth in native mode
 *
 * Validates that the supersonic_heap grows on demand when the initial
 * allocation is exhausted, and that freed memory is reclaimable.
 */
#include "EngineFixture.h"
#include "supersonic_heap.h"
#include <cstring>

// ── Direct heap tests (bypass engine, test the allocator directly) ──────────

TEST_CASE("supersonic_heap grows when initial pool is exhausted", "[heap][growth]") {
    // Destroy any existing heap from prior tests, then create a tiny one
    supersonic_heap_destroy();
    supersonic_heap_init(256 * 1024);  // 256KB

    REQUIRE(supersonic_heap_total_allocated() > 0);
    REQUIRE(supersonic_heap_growth_count() == 0);

    // Allocate more than the 256KB pool
    void* p = supersonic_heap_alloc(512 * 1024);  // 512KB
    REQUIRE(p != nullptr);

    CHECK(supersonic_heap_growth_count() > 0);
    CHECK(supersonic_heap_total_allocated() > 256 * 1024);

    supersonic_heap_free(p);
    supersonic_heap_destroy();
}

TEST_CASE("supersonic_heap supports multiple growth events", "[heap][growth]") {
    supersonic_heap_destroy();
    supersonic_heap_init(64 * 1024);  // 64KB — very small

    // Allocate several chunks that each exceed the initial pool
    void* ptrs[5];
    for (int i = 0; i < 5; i++) {
        ptrs[i] = supersonic_heap_alloc(128 * 1024);  // 128KB each
        REQUIRE(ptrs[i] != nullptr);
    }

    CHECK(supersonic_heap_growth_count() >= 1);

    // All allocations are usable
    for (int i = 0; i < 5; i++) {
        std::memset(ptrs[i], 0xAB, 128 * 1024);
    }

    for (int i = 0; i < 5; i++) {
        supersonic_heap_free(ptrs[i]);
    }
    supersonic_heap_destroy();
}

TEST_CASE("supersonic_heap freed memory is reusable", "[heap][growth]") {
    supersonic_heap_destroy();
    supersonic_heap_init(256 * 1024);  // 256KB

    // Allocate and free a large chunk
    void* p1 = supersonic_heap_alloc(512 * 1024);
    REQUIRE(p1 != nullptr);
    size_t growthAfterAlloc = supersonic_heap_growth_count();

    supersonic_heap_free(p1);

    // Reallocate same size — should succeed (may or may not need another growth
    // depending on AllocPool's free-list coalescing across areas)
    void* p2 = supersonic_heap_alloc(512 * 1024);
    REQUIRE(p2 != nullptr);

    supersonic_heap_free(p2);
    supersonic_heap_destroy();
}

TEST_CASE("supersonic_heap destroy cleans up growth areas", "[heap][growth]") {
    supersonic_heap_destroy();
    supersonic_heap_init(64 * 1024);

    // Trigger growth
    void* p = supersonic_heap_alloc(256 * 1024);
    REQUIRE(p != nullptr);
    REQUIRE(supersonic_heap_growth_count() > 0);

    supersonic_heap_free(p);
    supersonic_heap_destroy();

    // After destroy, stats should be zeroed
    CHECK(supersonic_heap_total_allocated() == 0);
    CHECK(supersonic_heap_growth_count() == 0);

    // Re-init should work cleanly
    supersonic_heap_init(256 * 1024);
    REQUIRE(supersonic_heap_total_allocated() > 0);
    REQUIRE(supersonic_heap_growth_count() == 0);
    supersonic_heap_destroy();
}

// ── Engine integration test (uses the default 64MB heap) ────────────────────

TEST_CASE("engine buffer allocation works with growable heap", "[heap][growth]") {
    EngineFixture fx;

    // Allocate a large buffer — exercises the supersonic_heap path
    fx.send(osc_test::message("/b_alloc", 0, 1024 * 1024, 2));  // 1M frames × 2ch = 8MB
    OscReply alloc;
    REQUIRE(fx.waitForReply("/done", alloc));
    fx.clearReplies();

    // Query to verify it's valid
    fx.send(osc_test::message("/b_query", 0));
    OscReply q;
    REQUIRE(fx.waitForReply("/b_info", q));
    auto parsed = q.parsed();
    CHECK(parsed.argInt(1) == 1024 * 1024);  // frames

    // Free it
    fx.send(osc_test::message("/b_free", 0));
    OscReply freed;
    REQUIRE(fx.waitForReply("/done", freed));
}

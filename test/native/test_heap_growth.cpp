/*
 * test_heap_growth.cpp — the guest's pool never grows.
 *
 * clockwork's allocator over the guest's bulk memory (clockwork_pool.h): a
 * span that fills answers NULL rather than growing or reaching malloc, so
 * what a guest declared is what it has. Exercised directly here, then
 * through the engine with a buffer well inside the profile's default span.
 * (Until 2026-10-15 the pool could grow once into a spare block; these were
 * its tests.)
 */
#include "EngineFixture.h"
#include "clockwork_pool.h"
#include "memory_profile.h"
#include <cstdlib>
#include <cstring>
#include <vector>

// ── Direct pool tests (bypass engine, test the allocator directly) ──────────

namespace {
// A span of the guest's: 16-aligned, as the engine's would be.
struct Span {
    std::vector<unsigned char> bytes;
    void* base;
    explicit Span(size_t n) : bytes(n + 16) {
        base = reinterpret_cast<void*>((reinterpret_cast<uintptr_t>(bytes.data()) + 15) & ~uintptr_t(15));
    }
};
}

TEST_CASE("the guest's pool serves from its span and refuses what the span cannot hold", "[heap][growth]") {
    clockwork_pool_teardown();
    Span span(256 * 1024);
    clockwork_pool_boot(span.base, 256 * 1024);
    REQUIRE(clockwork_pool_holds(span.base));

    void* p = clockwork_pool_alloc(64 * 1024);
    REQUIRE(p != nullptr);
    CHECK(clockwork_pool_holds(p));
    // More than the span: refused, never grown, never malloc'd.
    CHECK(clockwork_pool_alloc(512 * 1024) == nullptr);
    CHECK(clockwork_pool_used_bytes() >= 64 * 1024);

    clockwork_pool_free(p);
    clockwork_pool_teardown();
}

TEST_CASE("the guest's pool fills to its span and then refuses", "[heap][growth]") {
    clockwork_pool_teardown();
    Span span(1024 * 1024);
    clockwork_pool_boot(span.base, 1024 * 1024);

    std::vector<void*> taken;
    for (;;) {
        void* p = clockwork_pool_alloc(64 * 1024);
        if (!p) break;
        std::memset(p, 0xAB, 64 * 1024);
        taken.push_back(p);
        REQUIRE(taken.size() < 64);   // the span is finite
    }
    CHECK(taken.size() >= 14);
    CHECK(clockwork_pool_alloc(64 * 1024) == nullptr);

    for (void* p : taken) clockwork_pool_free(p);
    // Everything back: the span holds its full count again.
    void* again = clockwork_pool_alloc(64 * 1024);
    CHECK(again != nullptr);
    clockwork_pool_free(again);
    clockwork_pool_teardown();
}

TEST_CASE("the guest's pool reuses freed memory", "[heap][growth]") {
    clockwork_pool_teardown();
    Span span(256 * 1024);
    clockwork_pool_boot(span.base, 256 * 1024);

    void* p1 = clockwork_pool_alloc(128 * 1024);
    REQUIRE(p1 != nullptr);
    const size_t used = clockwork_pool_used_bytes();
    clockwork_pool_free(p1);
    CHECK(clockwork_pool_used_bytes() < used);

    void* p2 = clockwork_pool_alloc(128 * 1024);
    REQUIRE(p2 != nullptr);
    CHECK(p2 == p1);   // the freed block, back at the head of its size class
    clockwork_pool_free(p2);
    clockwork_pool_teardown();
}

TEST_CASE("teardown forgets the pool", "[heap][growth]") {
    clockwork_pool_teardown();
    Span span(64 * 1024);
    clockwork_pool_boot(span.base, 64 * 1024);

    void* p = clockwork_pool_alloc(16 * 1024);
    REQUIRE(p != nullptr);
    clockwork_pool_free(p);
    clockwork_pool_teardown();

    CHECK(clockwork_pool_used_bytes() == 0);
    CHECK(!clockwork_pool_holds(span.base));
    CHECK(clockwork_pool_alloc(16) == nullptr);   // no pool

    // Boot again cleanly.
    clockwork_pool_boot(span.base, 64 * 1024);
    REQUIRE(clockwork_pool_holds(span.base));
    clockwork_pool_teardown();
}

// ── Engine integration test (the profile's default bulk memory) ─────────────

TEST_CASE("engine buffer allocation works from the guest's bulk memory", "[heap][growth]") {
    EngineFixture fx;

    // Allocate a large buffer — exercises the pool path
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

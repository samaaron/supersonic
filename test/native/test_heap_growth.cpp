/*
 * test_heap_growth.cpp — the guest's pool never grows.
 *
 * clockwork's allocator over the guest's bulk memory: a span that fills
 * answers NULL rather than growing or reaching malloc, so what a guest
 * declared is what it has. The pool itself is clockwork's, and clockwork
 * holds it to that (clockwork-heap's core_heap tests); here, scsynth through
 * the engine, with a buffer well inside the profile's default span.
 */
#include "EngineFixture.h"

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

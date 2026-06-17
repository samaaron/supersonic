/*
 * test_scheduler.cpp — Unit tests for the generic Scheduler core (Scheduler.h):
 * time ordering, tag-selective flush, and the bump data-pool accounting.
 * Pure data structure — no engine, no audio driver.
 */
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstring>

#include "scheduler/Scheduler.h"

namespace {
struct TestMeta { uint32_t v = 0; };

const uint32_t TAG_KEEP  = sched_tag_hash("keep", 4);
const uint32_t TAG_FLUSH = sched_tag_hash("flushme", 7);

const uint8_t kData[8] = {1, 2, 3, 4, 5, 6, 7, 8};
}  // namespace

// Regression: flushing buried (not-yet-due) events must not leak heap capacity.
// A flushed event whose slot is freed but whose time is later than a live event
// never sits at the heap top, so a tombstone-only design would never reclaim it
// and add() would falsely report the pool full.
TEST_CASE("Scheduler - flush of buried future events does not leak capacity",
          "[scheduler][flush]") {
    Scheduler<TestMeta, 4, 8192> s;

    // One early, live event pins the heap top so flushed later events are buried.
    REQUIRE(s.add(/*when*/ 10, TAG_KEEP, {}, kData, sizeof kData));

    for (int i = 0; i < 1000; ++i) {
        REQUIRE(s.add(/*when*/ 1000 + i, TAG_FLUSH, {}, kData, sizeof kData));
        s.flush(TAG_FLUSH);
        REQUIRE(s.size() == 1);   // only the keeper remains live
    }

    // Capacity must still be available — the buried flushes did not leak.
    REQUIRE(s.add(/*when*/ 20, TAG_KEEP, {}, kData, sizeof kData));
    CHECK(s.size() == 2);
}

// The data pool can be exhausted while slots remain free (the ESP32 profile sizes
// the pool well below slots*maxPayload). add() then fails even though full() — which
// reports slot-count only — is false. Callers that back-pressure on full() must also
// treat an add() failure as backpressure, not a silent drop. Guards the invariant
// the drain's Retain path relies on.
TEST_CASE("Scheduler - data pool fills while slots stay free; full() misses it",
          "[scheduler]") {
    Scheduler<TestMeta, 64, 256> s;   // 64 slots, 256-byte pool
    const uint8_t blob[64] = {0};

    int added = 0;
    while (s.add(/*when*/ 1000 + added, TAG_KEEP, {}, blob, sizeof blob)) ++added;

    CHECK(added < 64);        // ran out of DATA pool (256/64 = 4 chunks), not slots
    CHECK_FALSE(s.full());    // full() is slot-count only — slots are still free
    const uint8_t tiny[1] = {0};
    CHECK_FALSE(s.add(2000, TAG_KEEP, {}, tiny, sizeof tiny));  // even 1 byte won't fit
}

TEST_CASE("Scheduler - pops due events in time order (FIFO for equal times)",
          "[scheduler]") {
    Scheduler<TestMeta, 16, 8192> s;
    s.add(300, TAG_KEEP, {3}, kData, 4);
    s.add(100, TAG_KEEP, {1}, kData, 4);
    s.add(100, TAG_KEEP, {2}, kData, 4);   // same time as {1}, added later → after it
    s.add(200, TAG_KEEP, {4}, kData, 4);

    int64_t last = -1;
    int order[4]; int n = 0;
    for (;;) {
        auto e = s.popDue(INT64_MAX);
        if (!e.valid()) break;
        CHECK(e.when >= last); last = e.when;
        order[n++] = static_cast<int>(e.meta->v);
        s.release(e);
    }
    REQUIRE(n == 4);
    CHECK(order[0] == 1);   // t=100 first-added
    CHECK(order[1] == 2);   // t=100 second-added (FIFO)
    CHECK(order[2] == 4);   // t=200
    CHECK(order[3] == 3);   // t=300
}

TEST_CASE("Scheduler - flush cancels only the matching tag; flush(0) clears all",
          "[scheduler][flush]") {
    Scheduler<TestMeta, 16, 8192> s;
    s.add(100, TAG_KEEP, {}, kData, 4);
    s.add(200, TAG_FLUSH, {}, kData, 4);
    s.add(300, TAG_KEEP, {}, kData, 4);
    REQUIRE(s.size() == 3);

    s.flush(TAG_FLUSH);
    CHECK(s.size() == 2);
    CHECK(s.nextTime() == 100);          // earliest survivor
    // The flushed event must never surface.
    int popped = 0;
    for (;;) { auto e = s.popDue(INT64_MAX); if (!e.valid()) break; ++popped; s.release(e); }
    CHECK(popped == 2);

    s.add(100, TAG_KEEP, {}, kData, 4);
    s.add(200, TAG_FLUSH, {}, kData, 4);
    s.flush(0);                          // wildcard
    CHECK(s.size() == 0);
    CHECK(s.nextTime() == INT64_MAX);
}

TEST_CASE("Scheduler - data pool reclaims released bytes under continuous use",
          "[scheduler]") {
    Scheduler<TestMeta, 512, 524288> s;
    // Pin the queue at >=1 so the pool never fully drains (the only free path is
    // then compaction).
    uint8_t keeper[64] = {};
    REQUIRE(s.add(INT64_MAX, TAG_KEEP, {}, keeper, sizeof keeper));

    uint8_t busy[152];
    std::memset(busy, 0xAB, sizeof busy);
    int failedAt = -1;
    for (int i = 0; i < 10000; ++i) {
        if (!s.add(i, TAG_KEEP, {}, busy, sizeof busy)) { failedAt = i; break; }
        auto e = s.popDue(INT64_MAX);   // pops the keeper or busy? earliest = busy(i) (< INT64_MAX)
        REQUIRE(e.valid());
        s.release(e);
    }
    CHECK(failedAt == -1);
}

TEST_CASE("Scheduler - drained queue resets the data-pool head", "[scheduler]") {
    Scheduler<TestMeta, 16, 8192> s;
    for (int i = 0; i < 5; ++i) REQUIRE(s.add(i, TAG_KEEP, {}, kData, 8));
    CHECK(s.dataUsed() > 0);
    while (s.size() > 0) { auto e = s.popDue(INT64_MAX); REQUIRE(e.valid()); s.release(e); }
    CHECK(s.dataUsed() == 0);
}

TEST_CASE("Scheduler - payload bytes survive pool churn intact", "[scheduler]") {
    Scheduler<TestMeta, 16, 8192> s;
    uint8_t keeper[32] = {};
    REQUIRE(s.add(INT64_MAX, TAG_KEEP, {}, keeper, sizeof keeper));
    for (int i = 0; i < 100; ++i) {
        uint8_t marker[64];
        for (int b = 0; b < 64; ++b) marker[b] = static_cast<uint8_t>((i * 7 + b) & 0xFF);
        REQUIRE(s.add(i, TAG_KEEP, {}, marker, sizeof marker));
        auto e = s.popDue(INT64_MAX);
        REQUIRE(e.valid());
        REQUIRE(e.size == sizeof marker);
        CHECK(std::memcmp(e.data, marker, sizeof marker) == 0);
        s.release(e);
    }
}

/*
 * test_event_scheduler.cpp — EngineScheduler in isolation: events in
 * (addScheduled) → stored → out on time (popDue returns each due OSC packet).
 * flush() cancels pending events by tag. No engine or hardware needed.
 */
#include <catch2/catch_test_macros.hpp>

#include "scheduler/EngineScheduler.h"
#include "RingTestUtils.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {
constexpr uint32_t kTagA = sched_tag_hash("a", 1);
constexpr uint32_t kTagB = sched_tag_hash("b", 1);
} // namespace

TEST_CASE("EngineScheduler holds events until due, then returns the OSC payload",
          "[event_scheduler]") {
    EngineScheduler es;
    const uint8_t osc[] = {0x90, 60, 100};   // stand-in inner payload
    REQUIRE(es.addScheduled(/*when*/ 1000, kTagA, /*origin*/ 0, osc, sizeof(osc)));

    // Not due yet → nothing fires.
    CHECK(ring_test::drainDue(es, /*now*/ 500).empty());

    // Due → exactly one event, carrying the scheduled payload verbatim.
    auto fired = ring_test::drainDue(es, /*now*/ 2000);
    REQUIRE(fired.size() == 1);
    REQUIRE(fired[0].data.size() == sizeof(osc));
    CHECK(std::memcmp(fired[0].data.data(), osc, sizeof(osc)) == 0);

    // A second drain with nothing pending returns nothing more.
    CHECK(ring_test::drainDue(es, 3000).empty());
}

TEST_CASE("EngineScheduler emits each due event once", "[event_scheduler]") {
    EngineScheduler es;
    const uint8_t a[] = {0xB0, 7, 1};
    const uint8_t b[] = {0xB0, 7, 2};
    es.addScheduled(100, kTagA, /*origin*/ 0, a, sizeof(a));
    es.addScheduled(200, kTagA, /*origin*/ 0, b, sizeof(b));

    auto first = ring_test::drainDue(es, 150);    // only `a` due
    REQUIRE(first.size() == 1);
    CHECK(std::memcmp(first[0].data.data(), a, sizeof(a)) == 0);

    CHECK(ring_test::drainDue(es, 150).empty());  // `a` already fired, `b` not due

    auto second = ring_test::drainDue(es, 250);   // now `b` due
    REQUIRE(second.size() == 1);
    CHECK(std::memcmp(second[0].data.data(), b, sizeof(b)) == 0);
}

TEST_CASE("EngineScheduler fires in the first tick at/after target — never early",
          "[event_scheduler]") {
    EngineScheduler es;
    const uint8_t osc[] = {0xF8};
    const int64_t block  = 1000;     // OSC-time units advanced per audio tick
    const int64_t target = 10'500;   // mid-block target
    es.addScheduled(target, kTagA, /*origin*/ 0, osc, sizeof(osc));

    int64_t t = 0;
    int64_t firedAtNextOsc = -1;
    for (int i = 0; i < 20 && firedAtNextOsc < 0; ++i) {
        const int64_t nextOscTime = t + block;
        if (!ring_test::drainDue(es, nextOscTime).empty()) firedAtNextOsc = nextOscTime;
        t = nextOscTime;
    }
    REQUIRE(firedAtNextOsc >= 0);
    CHECK(firedAtNextOsc >= target);            // never early
    CHECK(firedAtNextOsc - target < block);     // within one block (block-granular)
}

TEST_CASE("EngineScheduler carries the origin token to the due event",
          "[event_scheduler]") {
    EngineScheduler es;
    const uint8_t osc[] = {0x90, 60, 100};
    REQUIRE(es.addScheduled(/*when*/ 1000, kTagA, /*origin*/ 4242, osc, sizeof(osc)));

    auto e = es.popDue(2000);
    REQUIRE(e.valid());
    CHECK(e.meta->origin == 4242u);   // the scheduling caller's token survives to fire time
    es.release(e);
}

TEST_CASE("EngineScheduler rejects oversized payloads", "[event_scheduler]") {
    EngineScheduler es;
    // kMaxPayload is the scheduler's own data pool — a payload that can never fit
    // is dropped+counted (not back-pressured). One byte over is enough.
    std::vector<uint8_t> huge(EngineScheduler::kMaxPayload + 1, 0);
    CHECK_FALSE(es.addScheduled(0, kTagA, /*origin*/ 0, huge.data(),
                           static_cast<uint32_t>(huge.size())));
    CHECK(es.dropped() == 1);
}

TEST_CASE("EngineScheduler flush(tag) cancels only matching pending events",
          "[event_scheduler]") {
    EngineScheduler es;
    const uint8_t oscA[] = {0x90, 60, 100};   // kTagA payload (flushed)
    const uint8_t oscB[] = {0xB0, 7, 42};     // kTagB payload (survives)
    es.addScheduled(1000, kTagA, /*origin*/ 0, oscA, sizeof(oscA));
    es.addScheduled(1000, kTagB, /*origin*/ 0, oscB, sizeof(oscB));

    es.flush(kTagA);                  // drop the kTagA event only

    // Both would be due — but kTagA was flushed, so exactly one fires, and it is
    // the kTagB one (its payload survives intact).
    auto fired = ring_test::drainDue(es, 2000);
    REQUIRE(fired.size() == 1);
    REQUIRE(fired[0].data.size() == sizeof(oscB));
    CHECK(std::memcmp(fired[0].data.data(), oscB, sizeof(oscB)) == 0);

    // Nothing else should be queued.
    CHECK(ring_test::drainDue(es, 3000).empty());
}

TEST_CASE("EngineScheduler flush(0) cancels all pending events", "[event_scheduler]") {
    EngineScheduler es;
    const uint8_t osc[] = {0x90, 60, 100};
    es.addScheduled(1000, kTagA, /*origin*/ 0, osc, sizeof(osc));
    es.addScheduled(1000, kTagB, /*origin*/ 0, osc, sizeof(osc));

    es.flush(0);                      // wildcard: drop everything

    CHECK(ring_test::drainDue(es, 5000).empty());
}

TEST_CASE("EngineScheduler flush only affects pending events, not fired ones",
          "[event_scheduler]") {
    EngineScheduler es;
    const uint8_t osc[] = {0xF8};
    es.addScheduled(100, kTagA, /*origin*/ 0, osc, sizeof(osc));

    auto fired = ring_test::drainDue(es, 200);   // fire it
    REQUIRE(fired.size() == 1);

    // The event already fired; flushing its tag now is a no-op and never
    // resurrects or re-fires it.
    es.flush(kTagA);
    CHECK(ring_test::drainDue(es, 5000).empty());
}

// Scheduling accuracy: this is what scheduled OSC (and MIDI) inherits by riding
// the EngineScheduler instead of BEAM's millisecond timers — each event fires in
// the first block at/after its timetag (never early, within one block), and a
// regular request cadence comes out block-quantized but drift-free. Deterministic
// (no sockets/threads/wall-clock): the block grid is the only error source.
TEST_CASE("scheduled events fire block-accurately, never early, no drift",
          "[event_scheduler][accuracy]") {
    EngineScheduler es;
    const uint8_t osc[] = {0x2F, 0x6F, 0x00, 0x00};   // dummy outbound OSC payload

    constexpr int64_t block    = 128;                 // OSC-time units per audio block
    constexpr int     N        = 12;
    constexpr int64_t interval = block * 5 + 37;       // NOT a block multiple → exercises quantization
    constexpr int64_t base     = block * 3 + 11;       // first target, off the grid too

    for (int i = 0; i < N; ++i)
        es.addScheduled(base + i * interval, kTagA, /*origin*/ 0, osc, sizeof(osc));

    // Advance one block at a time; record the nextOscTime each event fires at.
    // interval > block guarantees at most one fires per block, so a non-empty
    // drain pins exactly one event's fire time.
    std::vector<int64_t> fireAt;
    int64_t t = 0;
    const int64_t lastTarget = base + (N - 1) * interval;
    while (t < lastTarget + block * 2 && static_cast<int>(fireAt.size()) < N) {
        const int64_t nextOscTime = t + block;
        auto fired = ring_test::drainDue(es, nextOscTime);
        REQUIRE(fired.size() <= 1);                 // never more than one per block
        if (!fired.empty()) fireAt.push_back(nextOscTime);
        t = nextOscTime;
    }
    REQUIRE(fireAt.size() == static_cast<size_t>(N));

    // Absolute accuracy: fired in the first block at/after the target, never early.
    int64_t maxLatency = 0;
    for (int i = 0; i < N; ++i) {
        const int64_t target = base + i * interval;
        CHECK(fireAt[i] >= target);                 // never early
        CHECK(fireAt[i] - target < block);          // within one block
        maxLatency = std::max(maxLatency, fireAt[i] - target);
    }
    CHECK(maxLatency < block);

    // Relative accuracy: consecutive spacing tracks the request (block-quantized,
    // so |error| < one block) and does NOT accumulate — fireAt[i]-base stays
    // within a block of i*interval across the whole run.
    for (int i = 1; i < N; ++i) {
        const int64_t spacing = fireAt[i] - fireAt[i - 1];
        CHECK(std::llabs(spacing - interval) < block);
    }
    CHECK(std::llabs((fireAt[N - 1] - fireAt[0]) - (N - 1) * interval) < block);  // no drift
}

/*
 * test_prescheduler.cpp — native host (Prescheduler) behavioural tests.
 *
 * test_prescheduler_core.cpp covers the portable algorithm against the shared
 * vectors + a fuzz; this file covers the native wrapper's own logic, which the
 * core has no part in: ring-buffer dispatch, the retry queue on a full buffer,
 * backpressure, payload-buffer ownership (alloc on schedule, free on dispatch /
 * drain / cancel), and the dispatch metrics.
 *
 * Timing flows through SuperClock: every bundle's timetag is anchored to the
 * injected clock's wallNow() (the same clock the OSC-ingress classifier and the
 * JS worker dispatch against), offset into the past (due) or future (pending).
 * The dispatch cycle is driven synchronously via PreschedulerTestAccess — no
 * thread, no sleeps — so due-ness is decided by the offset, not by wall-clock
 * progression, and the tests are deterministic.
 */
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>

#include "src/workers/Prescheduler.h"
#include "src/SuperClock.h"
#include "src/shared_memory.h"

// Friend seam (declared in Prescheduler.h): run the dispatch cycle directly and
// peek queue depths / shrink the backpressure cap for the overflow test.
struct PreschedulerTestAccess {
    static void     dispatch(Prescheduler& p)   { p.checkAndDispatch(); }
    static void     drainRetry(Prescheduler& p) { p.processRetryQueue(); }
    static uint32_t heapSize(Prescheduler& p)   { return p.mCore.size(); }
    static size_t   retrySize(Prescheduler& p)  { return p.mRetryQueue.size(); }
    static void     setMaxPending(Prescheduler& p, uint32_t n) {
        auto cfg = p.mCore.config();
        cfg.maxPending = n;
        p.mCore.setConfig(cfg);
    }
};

namespace {

constexpr uint32_t kRingSize = 4096;
const uint8_t kPayload[12] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };

// One self-contained native prescheduler: ring buffer + control atomics +
// metrics + clock, all owned here so each test starts from a clean slate.
struct Harness {
    std::vector<uint8_t> ring;
    std::atomic<int32_t> head{0}, tail{0}, seq{0}, lock{0};
    PerformanceMetrics   metrics;
    SuperClock           clock;
    Prescheduler         ps;

    explicit Harness(double lookaheadS = 0.5) : ring(kRingSize, 0) {
        // metrics lives in zeroed shared memory in production; match that here.
        std::memset(static_cast<void*>(&metrics), 0, sizeof(metrics));
        ps.setSuperClock(&clock);
        ps.initialise(ring.data(), kRingSize, &head, &tail, &seq, &lock, &metrics, lookaheadS);
    }

    // Schedule a bundle whose timetag is `offsetS` seconds from the engine
    // clock's current wall NTP: negative = already due, positive = future.
    void scheduleRel(double offsetS) {
        ps.schedule(kPayload, sizeof(kPayload), clock.wallNow() + offsetS);
    }

    uint32_t met(const std::atomic<uint32_t>& a) const { return a.load(std::memory_order_relaxed); }
    int32_t  met(const std::atomic<int32_t>& a)  const { return a.load(std::memory_order_relaxed); }

    // Wedge the ring buffer so writes fail (avail ≈ 1), or clear it so they pass.
    void fillRing()  { head.store(int32_t(kRingSize - 2)); tail.store(0); }
    void emptyRing() { head.store(0); tail.store(0); }
};

} // namespace

TEST_CASE("Prescheduler dispatches a due bundle to the ring buffer", "[prescheduler][native]") {
    Harness h;
    h.scheduleRel(-1.0);  // 1 s in the past → due now
    REQUIRE(PreschedulerTestAccess::heapSize(h.ps) == 1);

    PreschedulerTestAccess::dispatch(h.ps);

    CHECK(PreschedulerTestAccess::heapSize(h.ps) == 0);
    CHECK(h.met(h.metrics.prescheduler_dispatched) == 1);
    CHECK(h.met(h.metrics.prescheduler_total_dispatches) == 1);
    CHECK(h.met(h.metrics.prescheduler_pending) == 0);

    // The payload bytes actually landed in the ring buffer.
    uint32_t magic = 0;
    std::memcpy(&magic, h.ring.data(), sizeof(magic));
    CHECK(magic == MESSAGE_MAGIC);

    // Dispatched ~1 s after its timetag → counts as late.
    CHECK(h.met(h.metrics.prescheduler_lates) == 1);
    CHECK(h.met(h.metrics.prescheduler_max_late_ms) >= 900);
}

TEST_CASE("Prescheduler leaves a not-yet-due bundle queued", "[prescheduler][native]") {
    Harness h;
    h.scheduleRel(100.0);  // far future, outside the lookahead window

    PreschedulerTestAccess::dispatch(h.ps);

    CHECK(PreschedulerTestAccess::heapSize(h.ps) == 1);
    CHECK(h.met(h.metrics.prescheduler_dispatched) == 0);
    CHECK(h.met(h.metrics.prescheduler_pending) == 1);
}

TEST_CASE("Prescheduler records headroom for an on-time bundle", "[prescheduler][native]") {
    Harness h;            // lookahead 0.5 s
    h.scheduleRel(0.2);   // inside lookahead → due, and dispatched ahead of time

    PreschedulerTestAccess::dispatch(h.ps);

    CHECK(h.met(h.metrics.prescheduler_dispatched) == 1);
    CHECK(h.met(h.metrics.prescheduler_lates) == 0);
    // ~200 ms headroom; allow generous slack for the µs of real time elapsed.
    uint32_t headroom = h.met(h.metrics.prescheduler_min_headroom_ms);
    CHECK(headroom >= 150);
    CHECK(headroom <= 200);
}

TEST_CASE("Prescheduler retries a due bundle when the ring buffer is full, then drains",
          "[prescheduler][native]") {
    Harness h;
    h.fillRing();         // writes fail
    h.scheduleRel(-1.0);  // due

    PreschedulerTestAccess::dispatch(h.ps);

    CHECK(PreschedulerTestAccess::heapSize(h.ps) == 0);
    CHECK(PreschedulerTestAccess::retrySize(h.ps) == 1);
    CHECK(h.met(h.metrics.prescheduler_messages_retried) == 1);
    CHECK(h.met(h.metrics.prescheduler_retry_queue_size) == 1);
    CHECK(h.met(h.metrics.prescheduler_dispatched) == 0);
    CHECK(h.met(h.metrics.prescheduler_total_dispatches) == 1);

    h.emptyRing();        // space frees up
    PreschedulerTestAccess::drainRetry(h.ps);

    CHECK(PreschedulerTestAccess::retrySize(h.ps) == 0);
    CHECK(h.met(h.metrics.prescheduler_retries_succeeded) == 1);
    CHECK(h.met(h.metrics.prescheduler_retry_queue_size) == 0);
}

TEST_CASE("Prescheduler enforces the pending cap (backpressure drop)", "[prescheduler][native]") {
    Harness h;
    PreschedulerTestAccess::setMaxPending(h.ps, 3);

    for (int i = 0; i < 5; ++i) h.scheduleRel(100.0);  // future → stay queued

    CHECK(PreschedulerTestAccess::heapSize(h.ps) == 3);          // only 3 accepted
    CHECK(h.met(h.metrics.prescheduler_bundles_scheduled) == 3); // only accepted count
    CHECK(h.met(h.metrics.prescheduler_pending) == 3);
    // The 2 rejected bundles' buffers were freed (leak/double-free caught under
    // an ASan build, SUPERSONIC_SANITIZER=address).
}

TEST_CASE("Prescheduler cancelAll clears heap + retry queue and frees payloads",
          "[prescheduler][native]") {
    Harness h;
    h.scheduleRel(100.0);  // heap
    h.scheduleRel(100.0);  // heap

    // Force a third bundle into the retry queue.
    h.fillRing();
    h.scheduleRel(-1.0);   // due → dispatch fails → retry
    PreschedulerTestAccess::dispatch(h.ps);
    REQUIRE(PreschedulerTestAccess::heapSize(h.ps) == 2);
    REQUIRE(PreschedulerTestAccess::retrySize(h.ps) == 1);

    h.ps.cancelAll();

    CHECK(PreschedulerTestAccess::heapSize(h.ps) == 0);
    CHECK(PreschedulerTestAccess::retrySize(h.ps) == 0);
    CHECK(h.met(h.metrics.prescheduler_pending) == 0);
    CHECK(h.met(h.metrics.prescheduler_retry_queue_size) == 0);
    CHECK(h.met(h.metrics.prescheduler_events_cancelled) == 3);  // 2 heap + 1 retry

    // State is intact: a fresh due bundle still dispatches.
    h.emptyRing();
    h.scheduleRel(-1.0);
    PreschedulerTestAccess::dispatch(h.ps);
    CHECK(h.met(h.metrics.prescheduler_dispatched) == 1);
}

TEST_CASE("Prescheduler survives many schedule/cancel cycles", "[prescheduler][native]") {
    Harness h;
    for (int c = 0; c < 500; ++c) {
        for (int i = 0; i < 10; ++i) h.scheduleRel(100.0);
        h.ps.cancelAll();
    }
    CHECK(PreschedulerTestAccess::heapSize(h.ps) == 0);
    CHECK(h.met(h.metrics.prescheduler_events_cancelled) == 5000);
    // 5000 alloc/free pairs across the cycle — a target for the ASan build.
}

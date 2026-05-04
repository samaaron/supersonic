/*
 * test_priority_queue.cpp — Unit tests for PriorityQueueT and BundleScheduler
 *
 * Pure data-structure tests — no engine or audio driver needed.
 * Covers ordering, stability (FIFO for same-timestamp), capacity,
 * and interleaved add/remove patterns.
 */
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <climits>
#include <vector>
#include <algorithm>
#include <numeric>

// ---------- Minimal event type for PriorityQueueT tests ----------

struct TestEvent {
    int64_t mTime = 0;
    int64_t mStabilityCount = 0;
    int id = 0;  // payload for tracking identity

    struct key_t {
        int64_t time, stabilityCount;
        bool operator<(const key_t& rhs) const {
            if (time < rhs.time) return true;
            if (time > rhs.time) return false;
            return stabilityCount < rhs.stabilityCount;
        }
        bool operator>(const key_t& rhs) const {
            if (time > rhs.time) return true;
            if (time < rhs.time) return false;
            return stabilityCount > rhs.stabilityCount;
        }
    };

    int64_t Time() const { return mTime; }
    key_t key() const { return { mTime, mStabilityCount }; }
};

// Include the scheduler's PriorityQueueT (the one we changed to a heap)
#include "src/scheduler/PriorityQueue.h"

// ======================== PriorityQueueT ========================

TEST_CASE("PriorityQueueT - empty queue", "[scheduler][priority-queue]") {
    PriorityQueueT<TestEvent, 16> q;
    CHECK(q.Size() == 0);
    CHECK(q.NextTime() == INT64_MAX);
}

TEST_CASE("PriorityQueueT - single element", "[scheduler][priority-queue]") {
    PriorityQueueT<TestEvent, 16> q;
    TestEvent e;
    e.mTime = 100;
    e.mStabilityCount = 0;
    e.id = 1;

    CHECK(q.Add(e));
    CHECK(q.Size() == 1);
    CHECK(q.NextTime() == 100);

    TestEvent out = q.Remove();
    CHECK(out.mTime == 100);
    CHECK(out.id == 1);
    CHECK(q.Size() == 0);
}

TEST_CASE("PriorityQueueT - ordered insertion", "[scheduler][priority-queue]") {
    PriorityQueueT<TestEvent, 16> q;

    for (int i = 0; i < 5; i++) {
        TestEvent e;
        e.mTime = (i + 1) * 100;
        e.mStabilityCount = i;
        e.id = i;
        q.Add(e);
    }

    for (int i = 0; i < 5; i++) {
        CHECK(q.NextTime() == (i + 1) * 100);
        TestEvent out = q.Remove();
        CHECK(out.id == i);
    }
}

TEST_CASE("PriorityQueueT - reverse insertion", "[scheduler][priority-queue]") {
    PriorityQueueT<TestEvent, 16> q;

    for (int i = 4; i >= 0; i--) {
        TestEvent e;
        e.mTime = (i + 1) * 100;
        e.mStabilityCount = i;
        e.id = i;
        q.Add(e);
    }

    // Should come out in time order regardless of insertion order
    for (int i = 0; i < 5; i++) {
        TestEvent out = q.Remove();
        CHECK(out.mTime == (i + 1) * 100);
        CHECK(out.id == i);
    }
}

TEST_CASE("PriorityQueueT - random insertion order", "[scheduler][priority-queue]") {
    PriorityQueueT<TestEvent, 64> q;

    // Insert times in scrambled order
    int times[] = {500, 100, 300, 200, 400, 50, 350, 250, 150, 450};
    for (int i = 0; i < 10; i++) {
        TestEvent e;
        e.mTime = times[i];
        e.mStabilityCount = i;
        e.id = i;
        q.Add(e);
    }

    // Must come out sorted by time
    int64_t prev = 0;
    for (int i = 0; i < 10; i++) {
        TestEvent out = q.Remove();
        CHECK(out.mTime >= prev);
        prev = out.mTime;
    }
}

TEST_CASE("PriorityQueueT - stability (FIFO for same timestamp)", "[scheduler][priority-queue]") {
    PriorityQueueT<TestEvent, 16> q;

    // All same timestamp, different stability counts
    for (int i = 0; i < 5; i++) {
        TestEvent e;
        e.mTime = 100;
        e.mStabilityCount = i;  // ascending stability = insertion order
        e.id = i;
        q.Add(e);
    }

    // Should come out in insertion order (FIFO)
    for (int i = 0; i < 5; i++) {
        TestEvent out = q.Remove();
        CHECK(out.id == i);
    }
}

TEST_CASE("PriorityQueueT - capacity limit", "[scheduler][priority-queue]") {
    PriorityQueueT<TestEvent, 4> q;

    for (int i = 0; i < 4; i++) {
        TestEvent e;
        e.mTime = i * 100;
        e.mStabilityCount = i;
        CHECK(q.Add(e));
    }

    CHECK(q.IsFull());

    TestEvent overflow;
    overflow.mTime = 999;
    overflow.mStabilityCount = 99;
    CHECK_FALSE(q.Add(overflow));
    CHECK(q.Size() == 4);
}

TEST_CASE("PriorityQueueT - interleaved add and remove", "[scheduler][priority-queue]") {
    PriorityQueueT<TestEvent, 16> q;

    // Add 3 events
    for (int i = 0; i < 3; i++) {
        TestEvent e;
        e.mTime = (i + 1) * 100;
        e.mStabilityCount = i;
        e.id = i;
        q.Add(e);
    }

    // Remove earliest (100)
    TestEvent out = q.Remove();
    CHECK(out.mTime == 100);

    // Add something earlier than remaining
    TestEvent early;
    early.mTime = 150;
    early.mStabilityCount = 10;
    early.id = 10;
    q.Add(early);

    // Should get 150 next
    out = q.Remove();
    CHECK(out.mTime == 150);
    CHECK(out.id == 10);

    // Then 200, 300
    out = q.Remove();
    CHECK(out.mTime == 200);
    out = q.Remove();
    CHECK(out.mTime == 300);
    CHECK(q.Size() == 0);
}

TEST_CASE("PriorityQueueT - large fill and drain", "[scheduler][priority-queue]") {
    constexpr int CAP = 2048;
    PriorityQueueT<TestEvent, CAP> q;

    // Fill to capacity in reverse order (worst case for old insertion-sort)
    for (int i = CAP - 1; i >= 0; i--) {
        TestEvent e;
        e.mTime = i;
        e.mStabilityCount = CAP - 1 - i;
        e.id = i;
        CHECK(q.Add(e));
    }

    CHECK(q.IsFull());

    // Drain — must come out in ascending time order
    for (int i = 0; i < CAP; i++) {
        TestEvent out = q.Remove();
        CHECK(out.mTime == i);
    }

    CHECK(q.Size() == 0);
}

TEST_CASE("PriorityQueueT - remove from empty returns default", "[scheduler][priority-queue]") {
    PriorityQueueT<TestEvent, 4> q;
    TestEvent out = q.Remove();
    CHECK(out.mTime == 0);  // default-constructed
}

// ======================== BundleScheduler ========================
//
// These tests target BundleScheduler's data-pool accounting — separate
// from the inner PriorityQueueT. The pool is a bump allocator that only
// resets when the queue is fully empty. That's correct for batch
// patterns (fill → drain → fill) but wrong for steady-state streams
// (Sonic Pi's live loops never let the queue reach zero), which exhaust
// the pool monotonically and start dropping bundles. The tests here
// exercise the steady-state pattern explicitly.

#include "src/scheduler/BundleScheduler.h"

namespace {
    // BundleScheduler::Add takes a World*; its Perform() method is the
    // only thing that dereferences it. We never call Perform here, so
    // a null World pointer is safe.
    World* const kNoWorld = nullptr;

    // Construct a ReplyAddress that's safe to pass through Add — its
    // mReplyFunc is only invoked by reply-emitting code paths we don't
    // exercise.
    ReplyAddress make_null_reply() {
        ReplyAddress addr;
        addr.mReplyFunc = nullptr;
        addr.mReplyData = nullptr;
        addr.mProtocol = kUDP;
        addr.mPort = 0;
        addr.mSocket = 0;
        return addr;
    }
}

TEST_CASE("BundleScheduler - data pool reclaims released bytes "
          "(no leak under continuous use)",
          "[scheduler][bundle-scheduler]") {
    BundleScheduler sched;
    auto addr = make_null_reply();

    // Pin the queue at >=1 by holding one bundle in for the duration.
    // The pool only resets on full drain, so without the keeper bundle
    // the bug would self-heal between iterations.
    char keeperData[64] = {};
    REQUIRE(sched.Add(kNoWorld, INT64_MAX, keeperData, sizeof(keeperData), addr));

    // Cycle add-then-pop-then-release. Each iteration consumes 152
    // pool bytes (typical OSC bundle in Sonic Pi). With pool size
    // 524288 bytes (512 KB default) and ~152 bytes per cycle, the
    // un-fixed bump allocator runs out around iteration 3,448. We
    // run 10,000 to give plenty of headroom for the fix to prove
    // it's actually compacting, not just delaying.
    constexpr int  kCycles      = 10000;
    constexpr int  kBundleBytes = 152;
    char busyData[kBundleBytes];
    std::memset(busyData, 0xAB, sizeof(busyData));

    int  failedAt    = -1;
    uint32_t peakPool = 0;
    for (int i = 0; i < kCycles; ++i) {
        if (!sched.Add(kNoWorld, i, busyData, sizeof(busyData), addr)) {
            failedAt = i;
            break;
        }
        if (sched.DataPoolUsed() > peakPool) peakPool = sched.DataPoolUsed();

        // Pop the freshly-added bundle (it's the highest time so far,
        // but the keeper has time INT64_MAX so the new bundle pops first).
        ScheduledBundle* b = sched.Remove();
        REQUIRE(b != nullptr);
        sched.ReleaseSlot(b);
    }

    INFO("Add failed at iteration " << failedAt
         << " of " << kCycles
         << ", peak pool used = " << peakPool
         << "/" << sched.DataPoolCapacity()
         << ", queue size at exit = " << sched.Size());
    CHECK(failedAt == -1);
}

TEST_CASE("BundleScheduler - drained queue resets pool head",
          "[scheduler][bundle-scheduler]") {
    // Existing batch-pattern guarantee — verify the pre-existing reset
    // still works after any compaction work we add. Add a few bundles,
    // pop+release them all, expect mDataPoolHead back to zero.
    BundleScheduler sched;
    auto addr = make_null_reply();
    char data[100] = {};

    for (int i = 0; i < 5; ++i) {
        REQUIRE(sched.Add(kNoWorld, i, data, sizeof(data), addr));
    }
    CHECK(sched.DataPoolUsed() > 0);

    while (sched.Size() > 0) {
        ScheduledBundle* b = sched.Remove();
        REQUIRE(b != nullptr);
        sched.ReleaseSlot(b);
    }
    CHECK(sched.DataPoolUsed() == 0);
}

TEST_CASE("BundleScheduler - bundle data integrity preserved across pool churn",
          "[scheduler][bundle-scheduler]") {
    // Whatever pool-management strategy we use (compaction or not),
    // bundles popped from the queue must return data that exactly
    // matches what was passed to Add. This catches accidental data
    // corruption from in-place memmove / wrong-offset bugs.
    BundleScheduler sched;
    auto addr = make_null_reply();

    char keeperData[32] = {};
    REQUIRE(sched.Add(kNoWorld, INT64_MAX, keeperData, sizeof(keeperData), addr));

    // Add a marker bundle, pop it, verify the data is intact.
    // Repeat enough times that the pool head will have moved well
    // past the keeper's offset (forcing any compaction to re-place
    // both the keeper and the marker correctly).
    for (int i = 0; i < 100; ++i) {
        char marker[64];
        for (int b = 0; b < 64; ++b) marker[b] = static_cast<char>((i * 7 + b) & 0xFF);

        REQUIRE(sched.Add(kNoWorld, i, marker, sizeof(marker), addr));
        ScheduledBundle* popped = sched.Remove();
        REQUIRE(popped != nullptr);
        REQUIRE(popped->mSize == sizeof(marker));

        // mDataOffset points into sched's data pool; recover bytes via
        // the public DataPool() accessor.
        const uint8_t* poolData = sched.DataPool() + popped->mDataOffset;
        for (int b = 0; b < 64; ++b) {
            CHECK(static_cast<char>(poolData[b]) == marker[b]);
        }

        sched.ReleaseSlot(popped);
    }
}

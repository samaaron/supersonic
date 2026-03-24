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

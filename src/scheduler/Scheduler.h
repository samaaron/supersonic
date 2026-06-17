/*
    SuperSonic
    Copyright (c) 2025 Sam Aaron

    Dual-licensed MIT OR GPL-3.0-or-later (see repo LICENSE).

    Generic real-time timed-event scheduler.

    Stores opaque event payloads keyed by an int64 timetag and releases them in
    time order. Each event carries a caller-defined Meta value and a tag used for
    selective cancellation. The structure is RT-safe: a fixed metadata pool, a
    shared bump-allocated data pool with in-place compaction, and a binary
    min-heap of (timetag, slot index) entries. No allocation, no locks.

    Payload bytes and Meta are opaque to the scheduler; what an event means and
    how it is delivered when due is entirely the caller's concern.
*/

#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>

// Stable 32-bit hash of a tag string (FNV-1a). Shared by producers and the
// flush side so the same string keys the same events. Never returns 0 — 0 is
// reserved as the flush wildcard ("cancel everything").
constexpr uint32_t sched_tag_hash(const char* s, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; ++i) { h ^= static_cast<uint8_t>(s[i]); h *= 16777619u; }
    return h ? h : 1u;
}

// Default tag for user-scheduled events; cancelled on a run stop.
constexpr uint32_t SCHED_TAG_DEFAULT = sched_tag_hash("default", 7);
// Engine-generated periodic events (e.g. a clock) — a distinct tag so a
// default-tag flush never silences them. Only the wildcard flush(0) drops them.
constexpr uint32_t SCHED_TAG_CLOCK = sched_tag_hash("clock", 5);
// Graph (synth) events — a distinct tag so an outbound default-tag flush leaves
// them untouched; they are cleared via the wildcard flush(0) / clear().
constexpr uint32_t SCHED_TAG_SYNTH = sched_tag_hash("synth", 5);

template <typename Meta, int SlotCount, int DataPoolSize>
class Scheduler {
    static_assert(SlotCount > 0 && SlotCount <= 32767,
                  "SlotCount must be positive and fit the int16_t slot index");
    static_assert(DataPoolSize > 0, "DataPoolSize must be positive");

public:
    // A due event handed to the caller. Valid until release() is called for it.
    struct Event {
        int64_t        when = 0;
        uint32_t       tag  = 0;
        const Meta*    meta = nullptr;
        const uint8_t* data = nullptr;
        uint32_t       size = 0;
        int            slot = -1;   // opaque; pass back to release()
        bool valid() const { return slot >= 0; }
    };

    Scheduler() { reset(); }

    // Store `data`/`size` to fire at timetag `when`, carrying `meta` and keyed by
    // `tag`. Returns false (no state change) if the slot pool or data pool is
    // full. RT-safe; no allocation.
    bool add(int64_t when, uint32_t tag, const Meta& meta,
             const uint8_t* data, uint32_t size) {
        if (mQueueSize >= SlotCount) return false;
        if (size > static_cast<uint32_t>(DataPoolSize)) return false;  // never fits; also guards the +3 align below

        uint32_t aligned = (size + 3u) & ~3u;
        if (mDataHead + aligned > static_cast<uint32_t>(DataPoolSize)) {
            compact();
            if (mDataHead + aligned > static_cast<uint32_t>(DataPoolSize)) return false;
        }

        int slot = allocSlot();
        if (slot < 0) return false;

        uint32_t offset = mDataHead;
        std::memcpy(mData + offset, data, size);
        mDataHead += aligned;

        Slot& s = mPool[slot];
        s.when      = when;
        s.tag       = tag;
        s.meta      = meta;
        s.offset    = offset;
        s.size      = size;
        s.stability = mStability++;
        s.inUse     = true;

        heapPush(when, s.stability, static_cast<int16_t>(slot));
        return true;
    }

    // Timetag of the earliest live event, or INT64_MAX if none.
    int64_t nextTime() const {
        return mQueueSize > 0 ? mQueue[0].time : INT64_MAX;
    }

    // Pop the earliest event if it is due at/through `now`. The returned Event
    // borrows the data pool; call release(event) once the caller is done with it.
    // An invalid Event (valid() == false) means nothing is due.
    Event popDue(int64_t now) {
        if (mQueueSize == 0 || mQueue[0].time > now) return Event{};
        int slot = mQueue[0].poolIndex;
        heapPop();
        Slot& s = mPool[slot];
        return Event{ s.when, s.tag, &s.meta, mData + s.offset, s.size, slot };
    }

    // Return a popped event's slot to the pool. When the queue empties, the data
    // pool resets to zero (zero-cost compaction).
    void release(const Event& e) {
        if (e.slot < 0) return;
        freeSlot(e.slot);
    }

    // Cancel every live event whose tag matches `tag` (tag 0 = all). Matching
    // slots are freed and the heap is rebuilt from the survivors, so the heap
    // never carries dead entries. RT-safe (no allocation); O(n) over the pool.
    void flush(uint32_t tag) {
        if (tag == 0) { reset(); return; }
        bool freedAny = false;
        for (int i = 0; i < SlotCount; ++i) {
            if (mPool[i].inUse && mPool[i].tag == tag) { freeSlot(i); freedAny = true; }
        }
        if (freedAny) rebuildHeap();
    }

    void clear() { reset(); }

    int      size() const { return mLive; }
    bool     full() const { return mQueueSize >= SlotCount; }
    uint32_t dataUsed() const { return mDataHead; }
    uint32_t dataCapacity() const { return static_cast<uint32_t>(DataPoolSize); }

    // Cross-thread clear handshake: clear() is not safe to call concurrently with
    // the time-ordered operations on the audio thread. A control thread calls
    // requestClear() (lock-free); the audio thread calls drainPendingClear() at a
    // safe point. Release/acquire pairs the request with the drain.
    void requestClear() { mClearPending.store(true, std::memory_order_release); }
    bool drainPendingClear() {
        if (mClearPending.exchange(false, std::memory_order_acquire)) {
            reset();
            return true;
        }
        return false;
    }

private:
    struct Slot {
        int64_t  when      = 0;
        int64_t  stability = 0;
        uint32_t tag       = 0;
        uint32_t offset    = 0;
        uint32_t size      = 0;
        Meta     meta{};
        bool     inUse     = false;
        int16_t  nextFree  = -1;
    };

    struct QueueEntry {
        int64_t time      = 0;
        int64_t stability = 0;
        int16_t poolIndex = -1;
        bool earlierThan(const QueueEntry& o) const {
            if (time != o.time) return time < o.time;
            return stability < o.stability;   // FIFO for equal timetags
        }
    };

    // Scratch for compact(): a live chunk's (slot, data offset, size).
    struct Live { int16_t slot; uint32_t off; uint32_t size; };

    Slot              mPool[SlotCount];
    QueueEntry        mQueue[SlotCount];
    uint8_t           mData[DataPoolSize];
    Live              mCompactScratch[SlotCount];   // compact()'s working set; off the RT stack
    uint32_t          mDataHead  = 0;
    int               mQueueSize = 0;   // heap entries (kept equal to live slots)
    int               mLive      = 0;   // slots currently in use
    int64_t           mStability = 0;
    int16_t           mFreeHead  = -1;
    std::atomic<bool> mClearPending{false};

    void reset() {
        mDataHead = 0;
        mQueueSize = 0;
        mLive = 0;
        for (int i = 0; i < SlotCount - 1; ++i) {
            mPool[i].inUse = false;
            mPool[i].nextFree = static_cast<int16_t>(i + 1);
        }
        mPool[SlotCount - 1].inUse = false;
        mPool[SlotCount - 1].nextFree = -1;
        mFreeHead = 0;
    }

    int allocSlot() {
        if (mFreeHead < 0) return -1;
        int slot = mFreeHead;
        mFreeHead = mPool[slot].nextFree;
        ++mLive;
        return slot;
    }

    void freeSlot(int slot) {
        if (!mPool[slot].inUse) return;
        mPool[slot].inUse = false;
        mPool[slot].size = 0;
        mPool[slot].nextFree = mFreeHead;
        mFreeHead = static_cast<int16_t>(slot);
        --mLive;
        if (mLive == 0) mDataHead = 0;   // pool empty — reclaim everything
    }

    // Comparator for the std heap algorithms below: they keep the "greatest"
    // element at the front, so treating "later" as greater puts the earliest
    // event at mQueue[0]. Equal timetags fall back to the stability counter
    // (FIFO). All heap ops are in-place — RT-safe, no allocation.
    static bool laterFirst(const QueueEntry& a, const QueueEntry& b) {
        return b.earlierThan(a);
    }

    // Rebuild the heap from the live slots (used after flush frees a subset).
    void rebuildHeap() {
        mQueueSize = 0;
        for (int i = 0; i < SlotCount; ++i) {
            if (mPool[i].inUse) {
                mQueue[mQueueSize++] =
                    QueueEntry{ mPool[i].when, mPool[i].stability, static_cast<int16_t>(i) };
            }
        }
        std::make_heap(mQueue, mQueue + mQueueSize, laterFirst);
    }

    void heapPush(int64_t time, int64_t stability, int16_t poolIndex) {
        mQueue[mQueueSize++] = QueueEntry{ time, stability, poolIndex };
        std::push_heap(mQueue, mQueue + mQueueSize, laterFirst);
    }

    // Remove the earliest event (mQueue[0]); callers read it before calling.
    void heapPop() {
        if (mQueueSize <= 0) return;
        std::pop_heap(mQueue, mQueue + mQueueSize, laterFirst);
        --mQueueSize;
    }

    // Slide live data chunks down to remove gaps left by freed slots, without
    // requiring the queue to drain. No allocation; sort live chunks by offset
    // (member scratch, off the RT stack) then slide each down in-place.
    void compact() {
        if (mLive == 0) { mDataHead = 0; return; }

        Live* lives = mCompactScratch;
        int n = 0;
        for (int i = 0; i < SlotCount; ++i) {
            if (mPool[i].inUse && mPool[i].size > 0)
                lives[n++] = { static_cast<int16_t>(i), mPool[i].offset, mPool[i].size };
        }

        std::sort(lives, lives + n,
                  [](const Live& a, const Live& b) { return a.off < b.off; });

        uint32_t head = 0;
        for (int i = 0; i < n; ++i) {
            uint32_t aligned = (lives[i].size + 3u) & ~3u;
            if (head != lives[i].off) {
                std::memmove(mData + head, mData + lives[i].off, lives[i].size);
                mPool[lives[i].slot].offset = head;
            }
            head += aligned;
        }
        mDataHead = head;
    }
};

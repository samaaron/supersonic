/*
    SuperSonic - SuperCollider AudioWorklet WebAssembly port
    Copyright (c) 2025 Sam Aaron

    Index-based bundle scheduler for sample-accurate OSC timing.
    Events are stored in a fixed metadata pool with a shared variable-size
    data pool (bump allocator). Priority queue only stores small indices.

    Slot count and data pool size are configurable via compile-time flags:
      -DSCHEDULER_DATA_POOL_SIZE=524288  (default: 512KB)
      -DSCHEDULER_SLOT_COUNT=512         (default: 512 slots)
*/

#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include "../scsynth/server/OSC_Packet.h"

// Forward declarations
struct World;
void PerformOSCBundle(World* inWorld, OSC_Packet* inPacket);

// Scheduler configuration - can be overridden via -D flags at compile time
#ifndef SCHEDULER_DATA_POOL_SIZE
#define SCHEDULER_DATA_POOL_SIZE (512 * 1024)  // 512KB — variable-size bundle data
#endif

#ifndef SCHEDULER_SLOT_COUNT
#define SCHEDULER_SLOT_COUNT 512
#endif

// Maximum scheduled events (RT-safe, statically allocated)
constexpr int MAX_SCHEDULED_BUNDLES = SCHEDULER_SLOT_COUNT;

// Scheduled OSC bundle metadata — data lives in shared pool
struct ScheduledBundle {
    int64_t mTime;
    int32_t mSize;
    uint32_t mDataOffset;              // Offset into BundleScheduler's data pool
    World* mWorld;
    int64_t mStabilityCount;
    ReplyAddress mReplyAddr;
    bool mInUse;
    int16_t mNextFree;                 // Intrusive free list link (-1 = end)

    ScheduledBundle() : mTime(0), mSize(0), mDataOffset(0), mWorld(nullptr),
                        mStabilityCount(0), mInUse(false), mNextFree(-1) {
        mReplyAddr.mReplyFunc = nullptr;
    }

    void Init(World* world, int64_t time, int32_t size, uint32_t dataOffset,
              const ReplyAddress& replyAddr, int64_t stabilityCount) {
        mTime = time;
        mSize = size;
        mDataOffset = dataOffset;
        mWorld = world;
        mStabilityCount = stabilityCount;
        mReplyAddr = replyAddr;
        mInUse = true;
    }

    void Perform(uint8_t* dataPool) {
        if (mWorld && mSize > 0) {
            OSC_Packet packet;
            packet.mData = reinterpret_cast<char*>(dataPool + mDataOffset);
            packet.mSize = mSize;
            packet.mIsBundle = true;
            packet.mReplyAddr = mReplyAddr;
            PerformOSCBundle(mWorld, &packet);
        }
    }

    void Release() {
        mInUse = false;
        mSize = 0;
        mWorld = nullptr;
    }
};

// Priority queue entry - small, safe to copy
struct QueueEntry {
    int64_t time;
    int64_t stabilityCount;
    int16_t poolIndex;  // Index into bundle pool (-1 = invalid)

    QueueEntry() : time(0), stabilityCount(0), poolIndex(-1) {}

    bool operator<(const QueueEntry& rhs) const {
        if (time < rhs.time) return true;
        if (time > rhs.time) return false;
        return stabilityCount < rhs.stabilityCount;
    }

    bool operator>(const QueueEntry& rhs) const {
        if (time > rhs.time) return true;
        if (time < rhs.time) return false;
        return stabilityCount > rhs.stabilityCount;
    }
};

// Index-based bundle scheduler with variable-size data pool
// - Metadata pool of fixed slots (never moved/copied)
// - Shared bump-allocated data pool for OSC bundle bytes
// - Priority queue of small entries (safe to copy)
// - Pool resets when scheduler empties (zero fragmentation)
class BundleScheduler {
private:
    ScheduledBundle mPool[MAX_SCHEDULED_BUNDLES];  // Metadata pool
    QueueEntry mQueue[MAX_SCHEDULED_BUNDLES];      // Priority queue (min-heap)
    uint8_t mDataPool[SCHEDULER_DATA_POOL_SIZE];   // Variable-size data pool
    uint32_t mDataPoolHead;                        // Bump pointer
    int mQueueSize;
    int64_t mStabilityCounter;
    int16_t mFreeHead;                             // Free list head (-1 = empty)

public:
    BundleScheduler() : mDataPoolHead(0), mQueueSize(0), mStabilityCounter(0) {
        // Build free list: 0 → 1 → 2 → ... → (N-1) → -1
        for (int i = 0; i < MAX_SCHEDULED_BUNDLES - 1; ++i) {
            mPool[i].mNextFree = static_cast<int16_t>(i + 1);
        }
        mPool[MAX_SCHEDULED_BUNDLES - 1].mNextFree = -1;
        mFreeHead = 0;
    }

    // Allocate a metadata slot from the pool — O(1) via free list
    int AllocateSlot() {
        if (mFreeHead < 0) return -1;  // Pool full
        int slot = mFreeHead;
        mFreeHead = mPool[slot].mNextFree;
        return slot;
    }

    // Release a metadata slot back to the free list — O(1)
    // When the queue empties, reset the data pool (zero-cost compaction).
    void ReleaseSlot(ScheduledBundle* bundle) {
        bundle->Release();
        int16_t slot = static_cast<int16_t>(bundle - mPool);
        bundle->mNextFree = mFreeHead;
        mFreeHead = slot;

        // When all bundles have fired, reset the data pool
        if (mQueueSize == 0) {
            mDataPoolHead = 0;
        }
    }

    // Compact the data pool: walk live bundles in their current
    // mDataOffset order and slide each chunk forward to eliminate the
    // gaps left by released bundles. The bump-allocator design only
    // resets mDataPoolHead when the queue is fully empty (see
    // ReleaseSlot below) — for callers that always have at least one
    // bundle scheduled (Sonic Pi's live loops, etc.) the pool grows
    // monotonically until exhausted, then bundles get dropped (audible
    // glitches / scheduling jitter in user audio). This compactor lets
    // us reclaim the gaps without ever needing the queue to drain.
    //
    // RT-safe: no allocation, max ~512 in-place insertion sort + memmove.
    // Called lazily from Add when a bundle wouldn't fit otherwise.
    void Compact() {
        if (mQueueSize == 0) {
            mDataPoolHead = 0;
            return;
        }

        // Collect (slot, currentOffset, size) for each live bundle by
        // scanning the metadata pool. mInUse is cleared by Release(),
        // so this enumerates exactly the bundles whose data must be
        // preserved.
        struct Live { int16_t slot; uint32_t off; uint32_t size; };
        Live lives[MAX_SCHEDULED_BUNDLES];
        int n = 0;
        for (int s = 0; s < MAX_SCHEDULED_BUNDLES; ++s) {
            if (mPool[s].mInUse && mPool[s].mSize > 0) {
                lives[n++] = { static_cast<int16_t>(s),
                               mPool[s].mDataOffset,
                               static_cast<uint32_t>(mPool[s].mSize) };
            }
        }

        // Sort by current offset ascending so we can slide each chunk
        // forward (newOff <= oldOff) without overlap hazards. Insertion
        // sort — N up to 512, expected use is far smaller.
        for (int i = 1; i < n; ++i) {
            Live key = lives[i];
            int j = i - 1;
            while (j >= 0 && lives[j].off > key.off) {
                lives[j + 1] = lives[j];
                --j;
            }
            lives[j + 1] = key;
        }

        // Slide each live chunk down to the new compact head.
        uint32_t newHead = 0;
        for (int i = 0; i < n; ++i) {
            uint32_t aligned = (lives[i].size + 3) & ~3u;
            if (newHead != lives[i].off) {
                std::memmove(mDataPool + newHead,
                             mDataPool + lives[i].off,
                             lives[i].size);
                mPool[lives[i].slot].mDataOffset = newHead;
            }
            newHead += aligned;
        }
        mDataPoolHead = newHead;
    }

    // Add a bundle to the scheduler (variable size)
    bool Add(World* world, int64_t time, const char* data, int32_t size,
             const ReplyAddress& replyAddr) {
        if (mQueueSize >= MAX_SCHEDULED_BUNDLES) {
            return false;
        }

        // Bump-allocate from data pool (4-byte aligned for OSC)
        uint32_t aligned = (static_cast<uint32_t>(size) + 3) & ~3u;
        if (mDataPoolHead + aligned > SCHEDULER_DATA_POOL_SIZE) {
            // Pool head past capacity — but the gaps left by previously-
            // released bundles may free enough room. Compact in-place
            // and retry. If it still doesn't fit, the genuinely-live
            // data exceeds capacity — true failure.
            Compact();
            if (mDataPoolHead + aligned > SCHEDULER_DATA_POOL_SIZE) {
                return false;
            }
        }

        // Get a metadata slot
        int slot = AllocateSlot();
        if (slot < 0) {
            return false;
        }

        // Copy data into pool
        uint32_t offset = mDataPoolHead;
        std::memcpy(mDataPool + offset, data, size);
        mDataPoolHead += aligned;

        // Initialize metadata (no data copy — already in pool)
        mPool[slot].Init(world, time, size, offset, replyAddr, mStabilityCounter++);

        // Create queue entry (small, safe to copy)
        QueueEntry entry;
        entry.time = time;
        entry.stabilityCount = mPool[slot].mStabilityCount;
        entry.poolIndex = static_cast<int16_t>(slot);

        // Percolate up the min-heap
        int me = mQueueSize++;
        int mom;
        while (me > 0) {
            mom = (me - 1) >> 1;
            if (entry < mQueue[mom]) {
                mQueue[me] = mQueue[mom];
                me = mom;
            } else {
                break;
            }
        }
        mQueue[me] = entry;

        return true;
    }

    // Get next scheduled time
    int64_t NextTime() const {
        if (mQueueSize <= 0) {
            return INT64_MAX;
        }
        return mQueue[0].time;
    }

    // Remove and return pointer to next bundle (bundle stays in pool until Release)
    ScheduledBundle* Remove() {
        if (mQueueSize <= 0) {
            return nullptr;
        }

        // Get the pool slot for the first entry
        int slot = mQueue[0].poolIndex;

        // Demote last element down the heap
        mQueueSize--;
        if (mQueueSize > 0) {
            QueueEntry temp = mQueue[mQueueSize];
            int mom = 0;
            int me = 1;
            while (me < mQueueSize) {
                if (me + 1 < mQueueSize && mQueue[me] > mQueue[me + 1])
                    me++;
                if (temp > mQueue[me]) {
                    mQueue[mom] = mQueue[me];
                    mom = me;
                    me = (me << 1) + 1;
                } else {
                    break;
                }
            }
            mQueue[mom] = temp;
        }

        // Return pointer to bundle (caller must call Release when done)
        if (slot >= 0 && slot < MAX_SCHEDULED_BUNDLES) {
            return &mPool[slot];
        }
        return nullptr;
    }

    // Access data pool (for Perform calls)
    uint8_t* DataPool() { return mDataPool; }

    bool Empty() const { return mQueueSize == 0; }
    bool IsFull() const { return mQueueSize >= MAX_SCHEDULED_BUNDLES; }
    int Size() const { return mQueueSize; }
    int Capacity() const { return MAX_SCHEDULED_BUNDLES; }
    uint32_t DataPoolUsed() const { return mDataPoolHead; }
    uint32_t DataPoolCapacity() const { return SCHEDULER_DATA_POOL_SIZE; }

    void Clear() {
        mQueueSize = 0;
        mDataPoolHead = 0;
        for (int i = 0; i < MAX_SCHEDULED_BUNDLES; ++i) {
            mPool[i].Release();
            mPool[i].mNextFree = static_cast<int16_t>(i + 1);
        }
        mPool[MAX_SCHEDULED_BUNDLES - 1].mNextFree = -1;
        mFreeHead = 0;
    }

    // Cross-thread clear handshake: Clear() above is not safe to call
    // concurrently with NextTime/Add/Remove on the audio thread. Instead the
    // control thread calls RequestClear() (atomic, lock-free), and the audio
    // thread calls DrainPendingClear() at a safe point in its callback.
    // Release on the request pairs with acquire on the drain so the control
    // thread's prior writes (e.g. metrics counter resets) are visible after
    // the drain returns true.
    void RequestClear() { mClearPending.store(true, std::memory_order_release); }

    // Returns true if a clear was pending and has now been executed.
    bool DrainPendingClear() {
        if (mClearPending.exchange(false, std::memory_order_acquire)) {
            Clear();
            return true;
        }
        return false;
    }

private:
    std::atomic<bool> mClearPending{false};
};

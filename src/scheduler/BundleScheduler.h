/*
    SuperSonic - SuperCollider AudioWorklet WebAssembly port
    Copyright (c) 2025 Sam Aaron

    Index-based bundle scheduler - avoids copying 8KB structs.
    Events are stored in a fixed pool and never moved.
    Priority queue only stores small indices.
*/

#pragma once

#include <cstdint>
#include <cstring>
#include "../scsynth/server/OSC_Packet.h"

// Forward declarations
struct World;
void PerformOSCBundle(World* inWorld, OSC_Packet* inPacket);

// Maximum scheduled events (RT-safe, statically allocated)
constexpr int MAX_SCHEDULED_BUNDLES = 128;

// Scheduled OSC bundle - stored in pool, never copied
struct ScheduledBundle {
    int64_t mTime;
    int32_t mSize;
    World* mWorld;
    int64_t mStabilityCount;
    ReplyAddress mReplyAddr;
    char mData[8192];  // Embedded OSC data
    bool mInUse;       // Pool slot tracking

    ScheduledBundle() : mTime(0), mSize(0), mWorld(nullptr), mStabilityCount(0), mInUse(false) {
        mReplyAddr.mReplyFunc = nullptr;
    }

    void Init(World* world, int64_t time, const char* data, int32_t size,
              const ReplyAddress& replyAddr, int64_t stabilityCount) {
        mTime = time;
        mSize = size;
        mWorld = world;
        mStabilityCount = stabilityCount;
        mReplyAddr = replyAddr;
        mInUse = true;
        if (size > 0 && size <= 8192) {
            std::memcpy(mData, data, size);
        }
    }

    void Perform() {
        if (mWorld && mSize > 0) {
            OSC_Packet packet;
            packet.mData = mData;
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

// Index-based bundle scheduler
// - Pool of bundles (never moved/copied)
// - Priority queue of small entries (safe to copy)
class BundleScheduler {
private:
    ScheduledBundle mPool[MAX_SCHEDULED_BUNDLES];  // Event pool
    QueueEntry mQueue[MAX_SCHEDULED_BUNDLES];      // Priority queue (sorted)
    int mQueueSize;
    int64_t mStabilityCounter;

public:
    BundleScheduler() : mQueueSize(0), mStabilityCounter(0) {}

    // Allocate a slot from the pool
    int AllocateSlot() {
        for (int i = 0; i < MAX_SCHEDULED_BUNDLES; ++i) {
            if (!mPool[i].mInUse) {
                return i;
            }
        }
        return -1;  // Pool full
    }

    // Add a bundle to the scheduler
    bool Add(World* world, int64_t time, const char* data, int32_t size,
             const ReplyAddress& replyAddr) {
        if (mQueueSize >= MAX_SCHEDULED_BUNDLES) {
            return false;
        }

        // Get a pool slot
        int slot = AllocateSlot();
        if (slot < 0) {
            return false;
        }

        // Initialize the bundle in place (no copy!)
        mPool[slot].Init(world, time, data, size, replyAddr, mStabilityCounter++);

        // Create queue entry (small, safe to copy)
        QueueEntry entry;
        entry.time = time;
        entry.stabilityCount = mPool[slot].mStabilityCount;
        entry.poolIndex = static_cast<int16_t>(slot);

        // Insert into sorted queue (binary search insertion point)
        int insertPos = mQueueSize;
        for (int i = 0; i < mQueueSize; ++i) {
            if (entry < mQueue[i]) {
                insertPos = i;
                break;
            }
        }

        // Shift entries to make room (only ~20 bytes each!)
        for (int i = mQueueSize; i > insertPos; --i) {
            mQueue[i] = mQueue[i - 1];
        }

        mQueue[insertPos] = entry;
        mQueueSize++;

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

        // Shift queue entries (small copies, ~20 bytes each)
        mQueueSize--;
        for (int i = 0; i < mQueueSize; ++i) {
            mQueue[i] = mQueue[i + 1];
        }

        // Return pointer to bundle (caller must call Release when done)
        if (slot >= 0 && slot < MAX_SCHEDULED_BUNDLES) {
            return &mPool[slot];
        }
        return nullptr;
    }

    bool Empty() const { return mQueueSize == 0; }
    bool IsFull() const { return mQueueSize >= MAX_SCHEDULED_BUNDLES; }
    int Size() const { return mQueueSize; }
    int Capacity() const { return MAX_SCHEDULED_BUNDLES; }

    void Clear() {
        mQueueSize = 0;
        for (int i = 0; i < MAX_SCHEDULED_BUNDLES; ++i) {
            mPool[i].Release();
        }
    }
};

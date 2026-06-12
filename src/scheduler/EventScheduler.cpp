/*
 * EventScheduler.cpp — see EventScheduler.h. Linear over a small fixed pool: at
 * a few hundred slots the per-tick scan is negligible and the structure is
 * RT-safe (no malloc, no priority-queue bookkeeping). Due events fire
 * at block granularity; within a block their relative order is unspecified,
 * which MIDI tolerates.
 */
#include "EventScheduler.h"

// src-relative so it resolves under both the native (CMake) and web (emcc -Isrc)
// include roots.
#include "workers/RingBufferWriter.h"

#include <cstring>

bool EventScheduler::enqueue(int64_t when, uint32_t dest, const uint8_t* osc, uint32_t len) {
    if (len > kMaxPayload) {
        mDropped.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    for (auto& s : mSlots) {
        if (s.inUse) continue;
        s.inUse = true;
        s.when = when;
        s.dest = dest;
        s.len = len;
        std::memcpy(s.data, osc, len);
        return true;
    }
    mDropped.fetch_add(1, std::memory_order_relaxed);   // pool full
    return false;
}

void EventScheduler::tick(int64_t nextOscTime) {
    for (auto& s : mSlots) {
        if (!s.inUse || s.when > nextOscTime) continue;

        // Frame [dest:u32][osc] into the OUT ring for the consumer. A full
        // ring (stalled consumer) drops the event — count it so the loss is
        // visible in the same counter as enqueue drops.
        uint8_t buf[sizeof(uint32_t) + kMaxPayload];
        std::memcpy(buf, &s.dest, sizeof(s.dest));
        std::memcpy(buf + sizeof(s.dest), s.data, s.len);
        if (!RingBufferWriter::write(mOut, kOutSize, &mOutHead, &mOutTail, &mOutSeq, &mOutLock,
                                     buf, static_cast<uint32_t>(sizeof(s.dest)) + s.len, 0)) {
            mDropped.fetch_add(1, std::memory_order_relaxed);
        }
        s.inUse = false;
    }
}

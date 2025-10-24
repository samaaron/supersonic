/*
    SuperCollider real time audio synthesis system
    Copyright (c) 2002 James McCartney. All rights reserved.
    http://www.audiosynth.com

    Adapted for SuperSonic (SuperCollider AudioWorklet WebAssembly port)
    Copyright (c) 2025 Sam Aaron

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
*/

#pragma once

#include <cstdint>
#include <cstring>
#include "../scsynth/server/OSC_Packet.h"

// Forward declarations
struct World;
void PerformOSCBundle(World* inWorld, OSC_Packet* inPacket);

// Scheduled OSC bundle event
// Based on SC_CoreAudio.h:66-119 SC_ScheduledEvent
// Modified to embed OSC data directly (no heap allocation)
struct SC_ScheduledEvent {
    // Comparison key for priority queue
    struct key_t {
        int64_t time;
        int64_t stabilityCount;

        bool operator<(key_t const& rhs) const {
            if (time < rhs.time)
                return true;
            if (time > rhs.time)
                return false;
            return stabilityCount < rhs.stabilityCount;
        }

        bool operator>(key_t const& rhs) const {
            if (time > rhs.time)
                return true;
            if (time < rhs.time)
                return false;
            return stabilityCount > rhs.stabilityCount;
        }

        bool operator==(key_t const& rhs) const {
            return (time == rhs.time) && (stabilityCount == rhs.stabilityCount);
        }
    };

    // Constructor
    SC_ScheduledEvent(): mTime(0), mSize(0), mWorld(nullptr), mStabilityCount(0) {
        mReplyAddr.mReplyFunc = nullptr;
    }

    // Constructor with embedded data (RT-safe, no malloc)
    SC_ScheduledEvent(World* inWorld, int64_t inTime, const char* inData, int32_t inSize,
                      bool /*isBundle*/, const ReplyAddress& inReplyAddr)
        : mTime(inTime), mSize(inSize), mWorld(inWorld), mStabilityCount(0), mReplyAddr(inReplyAddr) {
        if (inSize > 0 && inSize <= 8192) {
            std::memcpy(mData, inData, inSize);
        }
    }

    int64_t Time() const { return mTime; }

    key_t key() const {
        key_t ret;
        ret.time = mTime;
        ret.stabilityCount = mStabilityCount;
        return ret;
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

    int64_t mTime;
    int32_t mSize;
    World* mWorld;
    int64_t mStabilityCount;
    ReplyAddress mReplyAddr;
    char mData[8192]; // Embedded OSC data (no malloc!)
};

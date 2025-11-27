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

#include <algorithm>
#include <cstdint>

// Priority queue for OSC bundle scheduling
// Based on SC_CoreAudio.h:159 PriorityQueueT
// Uses a static array for RT-safety (no malloc in audio thread)
template <class Event, int N>
class PriorityQueueT {
private:
    Event mEvents[N];
    int mSize;

public:
    PriorityQueueT(): mSize(0) {}

    bool Add(Event& inEvent) {
        if (mSize >= N)
            return false;

        // Find insertion point (maintain sorted order by time)
        int insertIndex = mSize;
        for (int i = 0; i < mSize; ++i) {
            if (inEvent.key() < mEvents[i].key()) {
                insertIndex = i;
                break;
            }
        }

        // Shift elements to make room
        for (int i = mSize; i > insertIndex; --i) {
            mEvents[i] = mEvents[i - 1];
        }

        // Insert event
        mEvents[insertIndex] = inEvent;
        mSize++;

        return true;
    }

    Event Remove() {
        if (mSize <= 0) {
            Event empty;
            return empty;
        }
        Event event = mEvents[0];
        mSize--;
        for (int i = 0; i < mSize; ++i) {
            mEvents[i] = mEvents[i + 1];
        }
        return event;
    }

    int64_t NextTime() const {
        if (mSize <= 0)
            return INT64_MAX;
        return mEvents[0].Time();
    }

    bool Empty() const { return mSize == 0; }

    bool IsFull() const { return mSize >= N; }

    void Empty() { mSize = 0; }

    int Size() const { return mSize; }

    int Capacity() const { return N; }
};

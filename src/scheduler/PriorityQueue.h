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
// Binary min-heap for O(log n) insert/remove. RT-safe (no malloc).
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

        // Percolate up
        int me = mSize++;
        int mom;
        while (me > 0) {
            mom = (me - 1) >> 1;
            if (inEvent.key() < mEvents[mom].key()) {
                mEvents[me] = mEvents[mom];
                me = mom;
            } else {
                break;
            }
        }
        mEvents[me] = inEvent;

        return true;
    }

    Event Remove() {
        if (mSize <= 0) {
            Event empty;
            return empty;
        }
        Event event = mEvents[0];
        if (--mSize == 0)
            return event;

        // Demote last element down the heap
        Event temp = mEvents[mSize];
        int mom = 0;
        int me = 1;
        while (me < mSize) {
            if (me + 1 < mSize && mEvents[me].key() > mEvents[me + 1].key())
                me++;
            if (temp.key() > mEvents[me].key()) {
                mEvents[mom] = mEvents[me];
                mom = me;
                me = (me << 1) + 1;
            } else {
                break;
            }
        }
        mEvents[mom] = temp;

        return event;
    }

    int64_t NextTime() const {
        if (mSize <= 0)
            return INT64_MAX;
        return mEvents[0].Time();
    }

    bool Empty() const { return mSize == 0; }

    bool IsFull() const { return mSize >= N; }

    void Clear() { mSize = 0; }

    int Size() const { return mSize; }

    int Capacity() const { return N; }
};

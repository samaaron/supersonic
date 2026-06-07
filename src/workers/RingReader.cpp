/*
 * RingReader.cpp — see RingReader.h.
 */
#include "RingReader.h"
#include <cstring>

RingReader::RingReader(const char* threadName) : juce::Thread(threadName) {
    mDrains.reserve(4);  // fixed before startThread(); reserve so run() never reallocs
}

RingReader::~RingReader() {
    signalThreadShouldExit();
    // A bare notify_all() cannot break std::atomic::wait(old): the wait only
    // returns when the value DIFFERS from old, so the value must change first.
    // Bump-then-notify (signal already set above) guarantees the run loop wakes,
    // sees threadShouldExit(), and exits, with no dependence on an external
    // processCount tick (the headless driver may already have stopped ticking).
    // Without this, stopThread(2000) times out and JUCE resorts to killThread()
    // / pthread_cancel(), which can deadlock under ThreadSanitizer.
    if (mWake) {
        mWake->fetch_add(1, std::memory_order_release);
        mWake->notify_all();
    }
    stopThread(2000);
}

void RingReader::addDrain(uint8_t*              buffer,
                          uint32_t              bufferSize,
                          std::atomic<int32_t>* head,
                          std::atomic<int32_t>* tail,
                          OnMessage             onMessage,
                          Metrics               metrics) {
    Drain d;
    d.buffer    = buffer;
    d.size      = bufferSize;
    d.head      = head;
    d.tail      = tail;
    d.onMessage = std::move(onMessage);
    d.metrics   = metrics;
    d.msgBuf.reserve(65536);
    mDrains.push_back(std::move(d));
}

void RingReader::drainOne(Drain& d) {
    if (!d.buffer || !d.head || !d.tail) return;

    while (true) {
        int32_t head = d.head->load(std::memory_order_acquire);
        int32_t tail = d.tail->load(std::memory_order_relaxed);
        if (head == tail) break;

        uint32_t ut    = static_cast<uint32_t>(tail);
        uint32_t uh    = static_cast<uint32_t>(head);
        uint32_t avail = (uh - ut + d.size) % d.size;
        if (avail < sizeof(Message)) break;

        // Read header (wrapping).
        Message hdr;
        {
            uint32_t sz    = sizeof(Message);
            uint32_t first = d.size - ut;
            if (sz <= first) {
                std::memcpy(&hdr, d.buffer + ut, sz);
            } else {
                std::memcpy(&hdr, d.buffer + ut, first);
                std::memcpy(reinterpret_cast<uint8_t*>(&hdr) + first, d.buffer, sz - first);
            }
        }

        if (hdr.magic == PADDING_MAGIC) {
            d.tail->store(0, std::memory_order_release);
            break;
        }
        if (hdr.magic != MESSAGE_MAGIC) {
            if (d.metrics.corrupted) d.metrics.corrupted->fetch_add(1, std::memory_order_relaxed);
            d.tail->store(head, std::memory_order_release);  // resync
            break;
        }

        uint32_t totalLen = hdr.length;
        if (totalLen < sizeof(Message) || totalLen > d.size) {
            if (d.metrics.corrupted) d.metrics.corrupted->fetch_add(1, std::memory_order_relaxed);
            d.tail->store(head, std::memory_order_release);
            break;
        }
        if (avail < totalLen) break;

        if (d.metrics.seqGaps) {
            int32_t seq = static_cast<int32_t>(hdr.sequence);
            if (d.lastSeq >= 0 && seq != d.lastSeq + 1)
                d.metrics.seqGaps->fetch_add(1, std::memory_order_relaxed);
            d.lastSeq = seq;
        }

        uint32_t payloadSize  = totalLen - sizeof(Message);
        uint32_t payloadStart = (ut + sizeof(Message)) % d.size;
        d.msgBuf.resize(payloadSize);
        {
            uint32_t first = d.size - payloadStart;
            if (payloadSize <= first) {
                std::memcpy(d.msgBuf.data(), d.buffer + payloadStart, payloadSize);
            } else {
                std::memcpy(d.msgBuf.data(), d.buffer + payloadStart, first);
                std::memcpy(d.msgBuf.data() + first, d.buffer, payloadSize - first);
            }
        }

        d.tail->store(static_cast<int32_t>((ut + totalLen) % d.size),
                      std::memory_order_release);

        if (d.metrics.received) d.metrics.received->fetch_add(1, std::memory_order_relaxed);
        if (d.metrics.bytes)    d.metrics.bytes->fetch_add(payloadSize, std::memory_order_relaxed);

        if (d.onMessage && payloadSize > 0)
            d.onMessage(hdr.sourceId, d.msgBuf.data(), payloadSize, hdr.sequence);
    }
}

void RingReader::run() {
    if (mWake) mLastWake = mWake->load(std::memory_order_relaxed);

    while (!threadShouldExit()) {
        if (mWake) {
            mWake->wait(mLastWake);  // C++20 equivalent of Atomics.wait()
            mLastWake = mWake->load(std::memory_order_acquire);
        } else {
            juce::Thread::sleep(1);
        }

        if (threadShouldExit()) break;
        for (auto& d : mDrains) drainOne(d);
    }
}

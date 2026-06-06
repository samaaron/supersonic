/*
 * RingReader.h — a thread that drains one or more Message-framed SPSC rings.
 *
 * Each registered ring (a "drain") has its own buffer, head/tail, callback and
 * optional metric counters; the thread blocks on a wake word and, on each wake,
 * drains every registered ring in turn. Most readers register a single ring; the
 * NRT egress gateway registers two (the OUT reply ring and the control ring) so
 * one thread is the sole non-RT consumer.
 *
 * Wake: the thread blocks on *wakeWord (a C++20 atomic wait). The audio callback
 * bumps `processCount` every block, so readers that drain a ring the audio thread
 * fills pass that and drain each block.
 *
 * Metrics: each counter is optional; pass nullptr to skip it.
 */
#pragma once

#include <juce_core/juce_core.h>
#include <atomic>
#include <cstdint>
#include <functional>
#include <vector>
#include "src/shared_memory.h"

class RingReader : public juce::Thread {
public:
    // Optional per-ring counters; any left null is simply not tracked.
    struct Metrics {
        std::atomic<uint32_t>* received  = nullptr;
        std::atomic<uint32_t>* bytes     = nullptr;
        std::atomic<uint32_t>* corrupted = nullptr;
        std::atomic<uint32_t>* seqGaps   = nullptr;
    };

    // (sourceId, payload, payloadSize, sequence). sourceId carries the origin
    // token on the control ring; OUT/DEBUG ignore it.
    using OnMessage = std::function<void(uint32_t, const uint8_t*, uint32_t, uint32_t)>;

    explicit RingReader(const char* threadName);
    ~RingReader() override;

    // Set the wake word the thread blocks on. Call before startThread().
    void setWake(std::atomic<uint32_t>* wakeWord) { mWake = wakeWord; }

    // Register a ring to drain on each wake. Call all of these before startThread()
    // (the drain list is fixed once the thread runs).
    void addDrain(uint8_t*              buffer,
                  uint32_t              bufferSize,
                  std::atomic<int32_t>* head,
                  std::atomic<int32_t>* tail,
                  OnMessage             onMessage,
                  Metrics               metrics);

private:
    void run() override;

    struct Drain {
        uint8_t*              buffer = nullptr;
        uint32_t              size   = 0;
        std::atomic<int32_t>* head   = nullptr;
        std::atomic<int32_t>* tail   = nullptr;
        OnMessage             onMessage;
        Metrics               metrics;
        int32_t               lastSeq = -1;
        std::vector<uint8_t>  msgBuf;
    };
    void drainOne(Drain& d);

    std::atomic<uint32_t>* mWake     = nullptr;
    uint32_t               mLastWake = 0;
    std::vector<Drain>     mDrains;
};

/*
 * Prescheduler.cpp
 */
#include "Prescheduler.h"
#include <algorithm>

Prescheduler::Prescheduler() : juce::Thread("SuperSonic-Prescheduler") {}

Prescheduler::~Prescheduler() {
    signalThreadShouldExit();
    mNewEventSignal.signal();
    stopThread(2000);
}

void Prescheduler::initialise(uint8_t*              inBufferStart,
                               uint32_t              inBufferSize,
                               std::atomic<int32_t>* inHead,
                               std::atomic<int32_t>* inTail,
                               std::atomic<int32_t>* inSequence,
                               std::atomic<int32_t>* inWriteLock,
                               PerformanceMetrics*   metrics,
                               double lookaheadS)
{
    mInBufferStart = inBufferStart;
    mInBufferSize  = inBufferSize;
    mInHead        = inHead;
    mInTail        = inTail;
    mInSequence    = inSequence;
    mInWriteLock   = inWriteLock;
    mMetrics       = metrics;
    mLookaheadS    = lookaheadS;
}

void Prescheduler::schedule(const uint8_t* data, uint32_t size, double ntpTimeSec) {
    Event e;
    e.ntpTimeSec = ntpTimeSec;
    e.size       = size;
    e.id         = mNextId.fetch_add(1, std::memory_order_relaxed);
    e.data.assign(data, data + size);

    {
        juce::ScopedLock sl(mLock);
        mHeap.push(std::move(e));

        if (mMetrics) {
            mMetrics->prescheduler_pending.store(
                static_cast<uint32_t>(mHeap.size()), std::memory_order_relaxed);
            mMetrics->prescheduler_bundles_scheduled.fetch_add(1, std::memory_order_relaxed);
        }
    }

    mNewEventSignal.signal();
}

void Prescheduler::cancelAll() {
    juce::ScopedLock sl(mLock);
    uint32_t cancelled = static_cast<uint32_t>(mHeap.size());
    mHeap.clear();

    if (mMetrics) {
        mMetrics->prescheduler_pending.store(0, std::memory_order_relaxed);
        mMetrics->prescheduler_events_cancelled.fetch_add(cancelled, std::memory_order_relaxed);
    }
}

void Prescheduler::checkAndDispatch() {
    if (!mInBufferStart) return;

    while (true) {
        Event event;
        {
            juce::ScopedLock sl(mLock);
            if (mHeap.empty()) break;
            double dispatchTime = mHeap.top().ntpTimeSec - mLookaheadS;
            if (wallClockNTP() < dispatchTime) break;
            event = mHeap.pop_move();

            if (mMetrics) {
                mMetrics->prescheduler_pending.store(
                    static_cast<uint32_t>(mHeap.size()), std::memory_order_relaxed);
            }
        }

        bool ok = RingBufferWriter::write(
            mInBufferStart,
            mInBufferSize,
            mInHead,
            mInTail,
            mInSequence,
            mInWriteLock,
            event.data.data(),
            event.size
        );

        if (mMetrics) {
            if (ok) {
                mMetrics->prescheduler_dispatched.fetch_add(1, std::memory_order_relaxed);
                mMetrics->prescheduler_total_dispatches.fetch_add(1, std::memory_order_relaxed);
            } else {
                mMetrics->prescheduler_retries_failed.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
}

void Prescheduler::run() {
    while (!threadShouldExit()) {
        double sleepMs = 50.0;

        {
            juce::ScopedLock sl(mLock);
            if (!mHeap.empty()) {
                double dispatchTime = mHeap.top().ntpTimeSec - mLookaheadS;
                double diff = dispatchTime - wallClockNTP();
                if (diff > 0.0) {
                    sleepMs = juce::jmin(diff * 1000.0, 50.0);
                } else {
                    sleepMs = 0.0;
                }
            }
        }

        if (sleepMs > 0.5) {
            mNewEventSignal.wait(static_cast<int>(sleepMs));  // JUCE WaitableEvent::wait(timeoutMs)
        }

        if (threadShouldExit()) break;
        checkAndDispatch();
    }
}

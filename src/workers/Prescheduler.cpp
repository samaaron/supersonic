/*
 * Prescheduler.cpp
 *
 * Mirrors js/workers/osc_out_prescheduler_worker.js — see that file for
 * the canonical algorithm. Both implementations share the same metric
 * struct (PerformanceMetrics in src/shared_memory.h) and the same
 * scheduling semantics: events with NTP timestamps go into a min-heap,
 * dispatch fires when (event.ntpTimeSec - lookaheadS) <= now. On
 * RingBufferWriter::write failure (buffer full) events go into a retry
 * queue and are re-attempted on the next dispatch cycle.
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

    // Initialise min-headroom sentinel (mirrors JS init path).
    if (mMetrics) {
        mMetrics->prescheduler_min_headroom_ms.store(
            kHeadroomUnsetSentinel, std::memory_order_relaxed);
    }
}

void Prescheduler::updatePendingPeak() {
    // Call under mLock. Mirrors JS updateMetrics(): peak tracks heap size
    // only, not heap + retry queue.
    if (!mMetrics) return;
    auto current = static_cast<uint32_t>(mHeap.size());
    auto peak = mMetrics->prescheduler_pending_peak.load(std::memory_order_relaxed);
    while (current > peak) {
        if (mMetrics->prescheduler_pending_peak.compare_exchange_weak(
                peak, current, std::memory_order_relaxed))
            break;
    }
}

void Prescheduler::schedule(const uint8_t* data, uint32_t size, double ntpTimeSec) {
    Event e;
    e.ntpTimeSec = ntpTimeSec;
    e.size       = size;
    e.id         = mNextId.fetch_add(1, std::memory_order_relaxed);
    e.data.assign(data, data + size);

    {
        juce::ScopedLock sl(mLock);

        // Backpressure: combined heap + retry queue cap. Mirrors JS
        // scheduleEvent backpressure path. Drop silently — JS posts an error
        // to its parent; native has no symmetric channel and the OscUdpServer
        // call site doesn't await a return value.
        if (mHeap.size() + mRetryQueue.size() >= kMaxPendingMessages)
            return;

        mHeap.push(std::move(e));

        if (mMetrics) {
            mMetrics->prescheduler_pending.store(
                static_cast<uint32_t>(mHeap.size()), std::memory_order_relaxed);
            mMetrics->prescheduler_bundles_scheduled.fetch_add(1, std::memory_order_relaxed);
            updatePendingPeak();
        }
    }

    mNewEventSignal.signal();
}

void Prescheduler::cancelAll() {
    juce::ScopedLock sl(mLock);
    uint32_t cancelled = static_cast<uint32_t>(mHeap.size() + mRetryQueue.size());
    mHeap.clear();
    mRetryQueue.clear();

    if (mMetrics) {
        mMetrics->prescheduler_pending.store(0, std::memory_order_relaxed);
        mMetrics->prescheduler_retry_queue_size.store(0, std::memory_order_relaxed);
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

        // Track timing relative to scheduled execution time. Mirrors JS:
        // headroom on on-time, lateness on late. Mutually exclusive per dispatch.
        if (mMetrics) {
            double timeUntilExec = event.ntpTimeSec - wallClockNTP();
            if (timeUntilExec < 0.0) {
                int32_t lateMs = static_cast<int32_t>(std::round(-timeUntilExec * 1000.0));
                mMetrics->prescheduler_lates.fetch_add(1, std::memory_order_relaxed);
                int32_t currentMaxLate =
                    mMetrics->prescheduler_max_late_ms.load(std::memory_order_relaxed);
                while (lateMs > currentMaxLate) {
                    if (mMetrics->prescheduler_max_late_ms.compare_exchange_weak(
                            currentMaxLate, lateMs, std::memory_order_relaxed))
                        break;
                }
            } else {
                uint32_t headroomMs = static_cast<uint32_t>(std::round(timeUntilExec * 1000.0));
                uint32_t currentMin =
                    mMetrics->prescheduler_min_headroom_ms.load(std::memory_order_relaxed);
                while (currentMin == kHeadroomUnsetSentinel || headroomMs < currentMin) {
                    if (mMetrics->prescheduler_min_headroom_ms.compare_exchange_weak(
                            currentMin, headroomMs, std::memory_order_relaxed))
                        break;
                }
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
            mMetrics->prescheduler_total_dispatches.fetch_add(1, std::memory_order_relaxed);
        }

        if (ok) {
            if (mMetrics)
                mMetrics->prescheduler_dispatched.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        // Buffer full — push to retry queue. Mirrors JS queueForRetry.
        juce::ScopedLock sl(mLock);
        if (mHeap.size() + mRetryQueue.size() >= kMaxPendingMessages) {
            // Combined backpressure cap exceeded; can't queue. Drop.
            if (mMetrics)
                mMetrics->prescheduler_retries_failed.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        mRetryQueue.push_back(std::move(event));
        if (mMetrics) {
            mMetrics->prescheduler_messages_retried.fetch_add(1, std::memory_order_relaxed);
            auto rq = static_cast<uint32_t>(mRetryQueue.size());
            mMetrics->prescheduler_retry_queue_size.store(rq, std::memory_order_relaxed);
            auto peak = mMetrics->prescheduler_retry_queue_peak.load(std::memory_order_relaxed);
            while (rq > peak) {
                if (mMetrics->prescheduler_retry_queue_peak.compare_exchange_weak(
                        peak, rq, std::memory_order_relaxed))
                    break;
            }
        }
    }
}

void Prescheduler::processRetryQueue() {
    // Drains the retry queue head-first. Mirrors JS processRetryQueue:
    // stop on first failure (buffer still full); the next dispatch cycle
    // will try again.
    while (true) {
        Event event;
        {
            juce::ScopedLock sl(mLock);
            if (mRetryQueue.empty()) return;
            event = std::move(mRetryQueue.front());
            mRetryQueue.pop_front();
            if (mMetrics) {
                mMetrics->prescheduler_retry_queue_size.store(
                    static_cast<uint32_t>(mRetryQueue.size()), std::memory_order_relaxed);
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

        if (ok) {
            if (mMetrics)
                mMetrics->prescheduler_retries_succeeded.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        // Still full — put it back at the head of the queue and stop.
        juce::ScopedLock sl(mLock);
        mRetryQueue.push_front(std::move(event));
        if (mMetrics) {
            mMetrics->prescheduler_retry_queue_size.store(
                static_cast<uint32_t>(mRetryQueue.size()), std::memory_order_relaxed);
        }
        return;
    }
}

void Prescheduler::run() {
    while (!threadShouldExit()) {
        double sleepMs = 50.0;
        bool retryActive = false;

        {
            juce::ScopedLock sl(mLock);
            retryActive = !mRetryQueue.empty();
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

        // When the retry queue is non-empty we want a faster wake to flush
        // it once the audio thread advances tail. C++ has no equivalent of
        // the JS Atomics.waitAsync(IN_TAIL) edge-trigger, so we poll with
        // a short timeout. 5 ms is ~2 audio blocks at 48 kHz / 128 frames.
        if (retryActive) sleepMs = juce::jmin(sleepMs, 5.0);

        if (sleepMs > 0.5) {
            mNewEventSignal.wait(static_cast<int>(sleepMs));
        }

        if (threadShouldExit()) break;
        checkAndDispatch();
        processRetryQueue();
    }
}

/*
 * Prescheduler.cpp — native host around PreschedulerCore. See Prescheduler.h.
 *
 * The min-heap, ordering, backpressure and cancellation semantics all live in
 * PreschedulerCore (pinned by test/vectors/prescheduler.json). This file owns
 * the native concerns: the dispatch thread, wall-clock timing from SuperClock,
 * ring-buffer writes, the retry queue for a full buffer, and the metrics.
 *
 * Locking: the core is not internally synchronised, so every core call is made
 * under mLock. Ring-buffer writes can block on the audio thread, so they are
 * done OUTSIDE mLock — checkAndDispatch() stages the due events under the lock
 * and writes them after releasing it.
 */
#include "Prescheduler.h"
#include "src/SuperClock.h"
#include <algorithm>
#include <cmath>

using supersonic::PreschedulerConfig;
using supersonic::PreschedulerEvent;
using supersonic::PreschedulerRequest;
using supersonic::PreschedulerStatus;

namespace {
// Core callback: stage a due event for dispatch outside the lock.
void stageSink(const PreschedulerEvent& ev, void* ctx) {
    static_cast<std::vector<PreschedulerEvent>*>(ctx)->push_back(ev);
}
// Core callback: free a cancelled event's payload buffer.
void freePayload(const PreschedulerEvent& ev, void*) {
    delete static_cast<std::vector<uint8_t>*>(ev.payload);
}
} // namespace

Prescheduler::Prescheduler() : juce::Thread("SuperSonic-Prescheduler") {}

Prescheduler::~Prescheduler() {
    signalThreadShouldExit();
    mNewEventSignal.signal();
    stopThread(2000);

    // Reclaim any payloads still queued at teardown.
    mCore.cancelAll(freePayload, nullptr);
    for (auto* buf : mRetryQueue) delete buf;
    mRetryQueue.clear();
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

    PreschedulerConfig cfg;
    cfg.lookaheadS = lookaheadS;
    cfg.maxPending = kMaxPendingMessages;
    mCore.setConfig(cfg);

    // Initialise the min-headroom sentinel.
    if (mMetrics) {
        mMetrics->prescheduler_min_headroom_ms.store(
            kHeadroomUnsetSentinel, std::memory_order_relaxed);
    }
}

void Prescheduler::updatePendingPeak() {
    // Call under mLock. Peak tracks heap size only, not heap + retry queue.
    if (!mMetrics) return;
    auto current = mCore.size();
    auto peak = mMetrics->prescheduler_pending_peak.load(std::memory_order_relaxed);
    while (current > peak) {
        if (mMetrics->prescheduler_pending_peak.compare_exchange_weak(
                peak, current, std::memory_order_relaxed))
            break;
    }
}

bool Prescheduler::writeToRing(const PayloadBuffer& buf) {
    return RingBufferWriter::write(
        mInBufferStart, mInBufferSize, mInHead, mInTail, mInSequence, mInWriteLock,
        buf.data(), static_cast<uint32_t>(buf.size()));
}

void Prescheduler::schedule(const uint8_t* data, uint32_t size, double ntpTimeSec) {
    // Native only ever schedules far-future bundles (the OscClassifier splits
    // off immediate / near / late), so every request carries a time. Session
    // and tag default to 0 — native does not yet route tagged cancellation.
    auto* buf = new PayloadBuffer(data, data + size);

    PreschedulerRequest req;
    req.hasTime = true;
    req.ntpTime = ntpTimeSec;
    req.bytes   = size;
    req.payload = buf;

    bool scheduled = false;
    {
        juce::ScopedLock sl(mLock);
        mCore.retryCount = static_cast<uint32_t>(mRetryQueue.size());
        if (mCore.schedule(req, mSuperClock->wallNow()) == PreschedulerStatus::Scheduled) {
            scheduled = true;
            if (mMetrics) {
                mMetrics->prescheduler_pending.store(mCore.size(), std::memory_order_relaxed);
                mMetrics->prescheduler_bundles_scheduled.fetch_add(1, std::memory_order_relaxed);
                updatePendingPeak();
            }
        }
    }

    if (scheduled) {
        mNewEventSignal.signal();
    } else {
        // Rejected (backpressure / too large / too far future): drop. Native has
        // no symmetric error channel back to the sender.
        delete buf;
    }
}

void Prescheduler::cancelAll() {
    juce::ScopedLock sl(mLock);
    uint32_t cancelled = mCore.size() + static_cast<uint32_t>(mRetryQueue.size());

    mCore.cancelAll(freePayload, nullptr);
    for (auto* buf : mRetryQueue) delete buf;
    mRetryQueue.clear();

    if (mMetrics) {
        mMetrics->prescheduler_pending.store(0, std::memory_order_relaxed);
        mMetrics->prescheduler_retry_queue_size.store(0, std::memory_order_relaxed);
        mMetrics->prescheduler_events_cancelled.fetch_add(cancelled, std::memory_order_relaxed);
    }
}

void Prescheduler::checkAndDispatch() {
    if (!mInBufferStart) return;

    // Stage every due event under the lock; do the ring-buffer I/O afterwards
    // so writes never block other threads on mLock.
    std::vector<PreschedulerEvent> due;
    {
        juce::ScopedLock sl(mLock);
        mCore.dispatchDue(mSuperClock->wallNow(), stageSink, &due);
        if (mMetrics)
            mMetrics->prescheduler_pending.store(mCore.size(), std::memory_order_relaxed);
    }

    for (const PreschedulerEvent& event : due) {
        auto* buf = static_cast<PayloadBuffer*>(event.payload);

        // Track timing relative to scheduled execution time: headroom when
        // on-time, lateness when late. Mutually exclusive per dispatch.
        if (mMetrics) {
            double timeUntilExec = event.ntpTime - mSuperClock->wallNow();
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

        bool ok = writeToRing(*buf);

        if (mMetrics)
            mMetrics->prescheduler_total_dispatches.fetch_add(1, std::memory_order_relaxed);

        if (ok) {
            if (mMetrics)
                mMetrics->prescheduler_dispatched.fetch_add(1, std::memory_order_relaxed);
            delete buf;
            continue;
        }

        // Buffer full — hand the payload to the retry queue.
        juce::ScopedLock sl(mLock);
        if (mCore.size() + mRetryQueue.size() >= kMaxPendingMessages) {
            // Combined backpressure cap exceeded; can't queue. Drop.
            if (mMetrics)
                mMetrics->prescheduler_retries_failed.fetch_add(1, std::memory_order_relaxed);
            delete buf;
            continue;
        }
        mRetryQueue.push_back(buf);
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
    // Drains the retry queue head-first: stop on the first failure (buffer
    // still full); the next dispatch cycle will try again.
    while (true) {
        PayloadBuffer* buf = nullptr;
        {
            juce::ScopedLock sl(mLock);
            if (mRetryQueue.empty()) return;
            buf = mRetryQueue.front();
            mRetryQueue.pop_front();
            if (mMetrics) {
                mMetrics->prescheduler_retry_queue_size.store(
                    static_cast<uint32_t>(mRetryQueue.size()), std::memory_order_relaxed);
            }
        }

        if (writeToRing(*buf)) {
            if (mMetrics)
                mMetrics->prescheduler_retries_succeeded.fetch_add(1, std::memory_order_relaxed);
            delete buf;
            continue;
        }

        // Still full — put it back at the head of the queue and stop.
        juce::ScopedLock sl(mLock);
        mRetryQueue.push_front(buf);
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
            double dueAt;
            if (mCore.nextDueTime(&dueAt)) {  // already lookahead-adjusted
                double diff = dueAt - mSuperClock->wallNow();
                sleepMs = (diff > 0.0) ? juce::jmin(diff * 1000.0, 50.0) : 0.0;
            }
        }

        // When the retry queue is non-empty, wake sooner to flush it once the
        // audio thread advances tail. There is no edge-trigger on tail here, so
        // poll with a short timeout — 5 ms is ~2 audio blocks at 48 kHz / 128 frames.
        if (retryActive) sleepMs = juce::jmin(sleepMs, 5.0);

        if (sleepMs > 0.5) {
            mNewEventSignal.wait(static_cast<int>(sleepMs));
        }

        if (threadShouldExit()) break;
        checkAndDispatch();
        processRetryQueue();
    }
}

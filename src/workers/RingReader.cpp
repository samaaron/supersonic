/*
 * RingReader.cpp — see RingReader.h.
 */
#include "RingReader.h"
#include <cstdio>

void RingReader::start() {
    if (mThread.joinable()) return;
    mExit.store(false, std::memory_order_release);
    mThread = std::thread([this] { run(); });
}

void RingReader::stop() {
    if (!mThread.joinable()) return;
    mExit.store(true, std::memory_order_release);
    // A bare notify cannot break std::atomic::wait(old): the wait only returns
    // when the value DIFFERS from old, so the value must change first. Bump-then-
    // notify (exit already set) guarantees the run loop wakes, sees mExit, and
    // exits with no dependence on an external processCount tick.
    if (mWake) {
        mWake->fetch_add(1, std::memory_order_release);
        mWake->notify_all();
    }
    resume();  // release a thread parked on mPauseRequest before joining
    mThread.join();
}

void RingReader::pause() {
    if (!mThread.joinable()) return;
    // Self-pause would deadlock waiting for our own park acknowledgement. It is
    // also unnecessary: a drain handler that triggers the caller's critical
    // section (e.g. a cold swap run from an OSC command on the NRT gateway
    // thread) is by definition not draining concurrently with it.
    if (std::this_thread::get_id() == mThread.get_id()) return;
    mPauseRequest.store(1, std::memory_order_release);
    // Kick the wake word so a reader blocked on it re-checks the request.
    if (mWake) {
        mWake->fetch_add(1, std::memory_order_release);
        mWake->notify_all();
    }
    // Park acknowledgement normally lands within one drain pass. Bound the wait
    // anyway: a reader wedged behind a foreign lock must degrade to an unparked
    // (pre-pause) swap, not hang the device switch.
    for (int waited = 0; mParked.load(std::memory_order_acquire) == 0; ++waited) {
        if (waited >= 2000) {
            fprintf(stderr, "[%s] pause: reader did not park within 2s — "
                    "proceeding unparked\n", mName);
            fflush(stderr);
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void RingReader::resume() {
    mPauseRequest.store(0, std::memory_order_release);
    mPauseRequest.notify_all();
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
    mDrains.push_back(std::move(d));
}

void RingReader::addTask(std::function<void()> task) {
    Drain d;
    d.task = std::move(task);
    mDrains.push_back(std::move(d));
}

void RingReader::drainOne(Drain& d) {
    if (d.task) {
        d.task();
        return;
    }
    // The walk itself is the shared lanes algorithm (src/lanes/ring_drain.h)
    // — the same code the lanes egress drains run. This class only adds the
    // thread, the wake word and the drain registry.
    ss_drain_ring(d.buffer, d.size, d.head, d.tail, d.state, d.metrics,
                  0 /* drain everything available */,
                  [&d](uint32_t sourceId, const uint8_t* payload,
                       uint32_t payloadSize, uint32_t sequence) {
                      if (d.onMessage)
                          d.onMessage(sourceId, payload, payloadSize, sequence);
                      return SsDrainVerdict::Consume;
                  });
}

void RingReader::run() {
    if (mWake) mLastWake = mWake->load(std::memory_order_relaxed);

    while (!mExit.load(std::memory_order_acquire)) {
        if (mWake) {
            mWake->wait(mLastWake);  // C++20 equivalent of Atomics.wait()
            mLastWake = mWake->load(std::memory_order_acquire);
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        if (mExit.load(std::memory_order_acquire)) break;

        if (mPauseRequest.load(std::memory_order_acquire)) {
            // Acknowledge between drain passes — the release-store makes every
            // write of the pass visible to the pauser — then sleep until
            // resume() (or exit) clears the request.
            mParked.store(1, std::memory_order_release);
            while (mPauseRequest.load(std::memory_order_acquire) &&
                   !mExit.load(std::memory_order_acquire))
                mPauseRequest.wait(1, std::memory_order_acquire);
            mParked.store(0, std::memory_order_relaxed);
            continue;
        }

        // Time the whole pass rather than each drain: what matters is how long
        // this thread was unavailable to everything queued behind it.
        const uint64_t startUs = nowUs();
        mPassStartUs.store(startUs, std::memory_order_relaxed);

        for (auto& d : mDrains) drainOne(d);

        const uint64_t elapsed = nowUs() - startUs;
        mPassStartUs.store(0, std::memory_order_relaxed);
        const uint32_t us = static_cast<uint32_t>(
            elapsed > UINT32_MAX ? UINT32_MAX : elapsed);
        if (us > mMaxPassUs.load(std::memory_order_relaxed))
            mMaxPassUs.store(us, std::memory_order_relaxed);
        if (us >= mSlowPassThresholdUs && mOnSlowPass) mOnSlowPass(us);
    }
}

uint64_t RingReader::nowUs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

uint32_t RingReader::inFlightUs() const {
    const uint64_t start = mPassStartUs.load(std::memory_order_relaxed);
    if (start == 0) return 0;
    const uint64_t elapsed = nowUs() - start;
    return static_cast<uint32_t>(elapsed > UINT32_MAX ? UINT32_MAX : elapsed);
}

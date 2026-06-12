/*
 * RingReader.cpp — see RingReader.h.
 */
#include "RingReader.h"
#include <cstdio>
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
    // A thread parked in pause() waits on mPauseRequest, not the wake word —
    // release it too so stopThread() never times out into killThread().
    resume();
    stopThread(2000);
}

void RingReader::pause() {
    if (!isThreadRunning()) return;
    // Self-pause would deadlock waiting for our own park acknowledgement.
    // It is also unnecessary: a drain handler that triggers the caller's
    // critical section (e.g. a cold swap run from an OSC command on the NRT
    // gateway thread) is by definition not draining concurrently with it.
    if (juce::Thread::getCurrentThreadId() == getThreadId()) return;
    mPauseRequest.store(1, std::memory_order_release);
    // Kick the wake word so a reader blocked on it re-checks the request
    // (same bump-then-notify reasoning as the destructor above).
    if (mWake) {
        mWake->fetch_add(1, std::memory_order_release);
        mWake->notify_all();
    }
    // Park acknowledgement normally lands within one drain pass. Bound the
    // wait anyway: a reader wedged behind a foreign lock must degrade to an
    // unparked (pre-pause) swap, not hang the device switch.
    for (int waited = 0; mParked.load(std::memory_order_acquire) == 0; ++waited) {
        if (!isThreadRunning()) return;
        if (waited >= 2000) {
            fprintf(stderr, "[%s] pause: reader did not park within 2s — "
                    "proceeding unparked\n", getThreadName().toRawUTF8());
            fflush(stderr);
            return;
        }
        juce::Thread::sleep(1);
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

    while (!threadShouldExit()) {
        if (mWake) {
            mWake->wait(mLastWake);  // C++20 equivalent of Atomics.wait()
            mLastWake = mWake->load(std::memory_order_acquire);
        } else {
            juce::Thread::sleep(1);
        }

        if (threadShouldExit()) break;

        if (mPauseRequest.load(std::memory_order_acquire)) {
            // Acknowledge between drain passes — the release-store makes every
            // write of the pass visible to the pauser — then sleep until
            // resume() (or exit) clears the request.
            mParked.store(1, std::memory_order_release);
            while (mPauseRequest.load(std::memory_order_acquire) && !threadShouldExit())
                mPauseRequest.wait(1, std::memory_order_acquire);
            mParked.store(0, std::memory_order_relaxed);
            continue;
        }

        for (auto& d : mDrains) drainOne(d);
    }
}

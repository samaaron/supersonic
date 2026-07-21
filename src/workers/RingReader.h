/*
 * SuperSonic
 * Copyright (c) 2025 Sam Aaron
 *
 * Dual-licensed MIT OR GPL-3.0-or-later (see repo LICENSE).
 *
 * RingReader.h — a std::thread that, on each wake, runs its registered work
 * items in registration order: ring drains (buffer + head/tail + callback +
 * optional metric counters) and plain tasks (used for the lanes egress drains,
 * whose ring state lives in lanes.cpp). The NRT egress gateway registers the
 * control-ring drain plus the two lanes egress tasks, so one thread is the sole
 * non-RT consumer.
 *
 * Wake: the thread blocks on *wakeWord (a C++20 atomic wait). The audio callback
 * bumps `processCount` every block, so readers that drain a ring the audio thread
 * fills pass that and drain each block. JUCE-free — only the destination leaves
 * touch JUCE; the ring transport does not.
 *
 * Metrics: each counter is optional; pass nullptr to skip it.
 */
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <thread>
#include <vector>
#include "src/lanes/ring_drain.h"

class RingReader {
public:
    // Optional per-ring counters; any left null is simply not tracked.
    // The drain algorithm itself lives in lanes (ring_drain.h) — this class
    // is only the thread + wake + registration wrapper around it.
    using Metrics = SsDrainMetrics;

    // (sourceId, payload, payloadSize, sequence). sourceId carries the origin
    // token on the control ring; the MIDI dispatch ring ignores it.
    using OnMessage = std::function<void(uint32_t, const uint8_t*, uint32_t, uint32_t)>;

    explicit RingReader(const char* threadName) : mName(threadName) {
        mDrains.reserve(4);  // fixed before start(); reserve so run() never reallocs
    }
    ~RingReader() { stop(); }

    RingReader(const RingReader&) = delete;
    RingReader& operator=(const RingReader&) = delete;

    // Set the wake word the thread blocks on. Call before start().
    void setWake(std::atomic<uint32_t>* wakeWord) { mWake = wakeWord; }

    // Register a ring to drain on each wake. Call all of these before start()
    // (the drain list is fixed once the thread runs).
    void addDrain(uint8_t*              buffer,
                  uint32_t              bufferSize,
                  std::atomic<int32_t>* head,
                  std::atomic<int32_t>* tail,
                  OnMessage             onMessage,
                  Metrics               metrics);

    // Register a task to run on each wake, in registration order with the ring
    // drains. Used for rings whose consumer state lives elsewhere — the lanes
    // egress drains (ss_egress_rt_drain / ss_egress_nrt_drain) own theirs.
    void addTask(std::function<void()> task);

    // Spawn the thread (after setWake + all addDrain/addTask). Idempotent.
    void start();
    // Signal exit, wake the loop (and release any park), join. Idempotent.
    void stop();

    // Quiescent pause: returns once the run loop is parked between drain
    // passes (or the thread has exited), so the caller may safely reset the
    // ring/drain state the drains read — cold swap tears down and re-inits the
    // shared-memory world while this thread would otherwise keep draining.
    // No-op if the thread was never started or when called from this reader's
    // own thread (a drain handler that triggers the swap is by definition not
    // draining concurrently with it). resume() unparks; extra resumes are
    // harmless.
    void pause();
    void resume();

    // ── Blocking observability ───────────────────────────────────────────────
    // This thread is the sole non-RT consumer: a handler that blocks stops every
    // later control command AND the egress drains registered behind it, so the
    // server keeps accepting packets it can no longer answer. Nothing about that
    // is visible from outside — socket up, process alive, audio ticking — which
    // is how sonic-pi#3551 cost a user a 30 s boot and told them only that the
    // Ruby server would not connect.

    // Longest drain pass observed, microseconds (high-water mark).
    uint32_t maxPassUs() const { return mMaxPassUs.load(std::memory_order_relaxed); }
    void resetMaxPassUs() { mMaxPassUs.store(0, std::memory_order_relaxed); }

    // Microseconds the current pass has been running; 0 between passes. A
    // high-water mark only records a stall once it ENDS — this shows one that
    // is still happening, which is the state a wedged server is stuck in.
    uint32_t inFlightUs() const;

    // Invoked (on this thread) when a pass exceeds the threshold, with its
    // duration. The engine hooks this to name the command that was in flight.
    void onSlowPass(std::function<void(uint32_t)> fn) { mOnSlowPass = std::move(fn); }
    void setSlowPassThresholdUs(uint32_t us) { mSlowPassThresholdUs = us; }

private:
    void run();
    static uint64_t nowUs();

    struct Drain {
        uint8_t*              buffer = nullptr;
        uint32_t              size   = 0;
        std::atomic<int32_t>* head   = nullptr;
        std::atomic<int32_t>* tail   = nullptr;
        OnMessage             onMessage;
        Metrics               metrics;
        SsDrainState          state;
        std::function<void()> task;  // when set, run instead of the ring walk
    };
    void drainOne(Drain& d);

    const char*            mName;
    std::atomic<uint32_t>* mWake     = nullptr;
    uint32_t               mLastWake = 0;
    std::vector<Drain>     mDrains;
    std::atomic<bool>      mExit{false};

    // pause()/resume() handshake. mPauseRequest is the caller's ask; mParked
    // acknowledges that the run loop is outside any drain pass.
    std::atomic<uint32_t>  mPauseRequest{0};
    std::atomic<uint32_t>  mParked{0};

    // Pass timing. mPassStartUs is a steady-clock stamp while a pass runs and 0
    // between passes, so a reader can tell "blocked now" from "was slow once".
    std::atomic<uint64_t>  mPassStartUs{0};
    std::atomic<uint32_t>  mMaxPassUs{0};
    uint32_t               mSlowPassThresholdUs = 250'000;   // 250 ms
    std::function<void(uint32_t)> mOnSlowPass;

    std::thread            mThread;
};

/*
 * RingReader.h — a thread that, on each wake, runs its registered work items
 * in registration order: ring drains (buffer + head/tail + callback +
 * optional metric counters) and plain tasks (used for the lanes egress
 * drains, whose ring state lives in lanes.cpp). The NRT egress gateway
 * registers the control-ring drain plus the two lanes egress tasks, so one
 * thread is the sole non-RT consumer; the MIDI dispatcher registers a single
 * ring.
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
#include "src/lanes/ring_drain.h"

class RingReader : public juce::Thread {
public:
    // Optional per-ring counters; any left null is simply not tracked.
    // The drain algorithm itself lives in lanes (ring_drain.h) — this class
    // is only the thread + wake + registration wrapper around it.
    using Metrics = SsDrainMetrics;

    // (sourceId, payload, payloadSize, sequence). sourceId carries the origin
    // token on the control ring; the MIDI dispatch ring ignores it.
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

    // Register a task to run on each wake, in registration order with the ring
    // drains. Used for rings whose consumer state lives elsewhere — the lanes
    // egress drains (ss_egress_rt_drain / ss_egress_nrt_drain) own theirs.
    void addTask(std::function<void()> task);

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

private:
    void run() override;

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

    std::atomic<uint32_t>* mWake     = nullptr;
    uint32_t               mLastWake = 0;
    std::vector<Drain>     mDrains;

    // pause()/resume() handshake. mPauseRequest is the caller's ask; mParked
    // acknowledges that the run loop is outside any drain pass.
    std::atomic<uint32_t>  mPauseRequest{0};
    std::atomic<uint32_t>  mParked{0};
};

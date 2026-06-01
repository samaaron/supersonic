/*
 * Prescheduler.h — native host around the portable PreschedulerCore.
 *
 * The scheduling algorithm (NTP min-heap + cancellation) lives in
 * PreschedulerCore, shared with the web worker and the ESP32 build and pinned
 * by test/vectors/prescheduler.json. This class is the native host: a
 * juce::Thread that feeds the core wall-clock NTP from SuperClock, writes due
 * events to the engine's ring buffer, and retries on a full buffer.
 *
 * Payload ownership: the core stores opaque payload pointers. Native owns the
 * OSC bytes as heap buffers (it has an allocator) and frees them when an event
 * is dispatched or cancelled; the ESP32 host instead backs payloads with a
 * fixed pool. Same core, different payload strategy.
 */
#pragma once

#include <juce_core/juce_core.h>
#include <atomic>
#include <cstdint>
#include <deque>
#include <vector>

#include "PreschedulerCore.h"
#include "src/native/WallClock.h"
#include "RingBufferWriter.h"
#include "src/shared_memory.h"

class SuperClock;

// Test seam: lets the native unit test drive the dispatch cycle directly
// (no thread, no sleeps) and inspect queue depths / shrink the cap. Defined in
// test/native/test_prescheduler.cpp.
struct PreschedulerTestAccess;

class Prescheduler : public juce::Thread {
public:
    // Wire the engine's SuperClock — single source of wall-clock NTP for
    // the prescheduler thread's dispatch-timing decisions. Must be set
    // before startThread().
    void setSuperClock(SuperClock* sc) { mSuperClock = sc; }

    // Backpressure cap on the combined heap + retry queue.
    static constexpr uint32_t kMaxPendingMessages = 65536;

    // Sentinel meaning "min headroom never recorded yet" — 0 is a valid headroom
    // value, so it can't double as "unset".
    static constexpr uint32_t kHeadroomUnsetSentinel = 0xFFFFFFFFu;

    void schedule(const uint8_t* data, uint32_t size, double ntpTimeSec);
    void cancelAll();

    void initialise(uint8_t*              inBufferStart,
                    uint32_t              inBufferSize,
                    std::atomic<int32_t>* inHead,
                    std::atomic<int32_t>* inTail,
                    std::atomic<int32_t>* inSequence,
                    std::atomic<int32_t>* inWriteLock,
                    PerformanceMetrics*   metrics,
                    double lookaheadS = 0.500);

    Prescheduler();
    ~Prescheduler() override;

private:
    friend struct PreschedulerTestAccess;

    void run() override;
    void checkAndDispatch();
    void processRetryQueue();
    void updatePendingPeak();   // call under mLock

    // Native payload buffer: the OSC bytes for one scheduled event. The core
    // holds these by opaque pointer; this host owns and frees them.
    using PayloadBuffer = std::vector<uint8_t>;

    // Write one buffer to the ring buffer. Shared by dispatch and retry.
    bool writeToRing(const PayloadBuffer& buf);

    uint8_t*              mInBufferStart = nullptr;
    uint32_t              mInBufferSize  = 0;
    std::atomic<int32_t>* mInHead        = nullptr;
    std::atomic<int32_t>* mInTail        = nullptr;
    std::atomic<int32_t>* mInSequence    = nullptr;
    std::atomic<int32_t>* mInWriteLock   = nullptr;
    PerformanceMetrics*   mMetrics       = nullptr;
    SuperClock*           mSuperClock    = nullptr;
    double                mLookaheadS    = 0.500;

    juce::CriticalSection mLock;
    juce::WaitableEvent   mNewEventSignal;

    // Heap storage and the core that orders it. Storage is sized to the
    // backpressure cap so the core's array can never overflow. Both are
    // guarded by mLock (the core is not internally synchronised).
    std::vector<supersonic::PreschedulerEvent> mStorage{kMaxPendingMessages};
    supersonic::PreschedulerCore               mCore{mStorage.data(), kMaxPendingMessages,
                                                     supersonic::PreschedulerConfig{}};

    std::deque<PayloadBuffer*> mRetryQueue;   // owned buffers, guarded by mLock
};

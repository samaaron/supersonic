/*
 * Prescheduler.h — NTP min-heap scheduler thread
 */
#pragma once

#include <juce_core/juce_core.h>
#include <atomic>
#include <cstdint>
#include <functional>
#include <vector>
#include <queue>

#include "NTPClock.h"
#include "RingBufferWriter.h"
#include "src/shared_memory.h"

class Prescheduler : public juce::Thread {
public:
    void schedule(const uint8_t* data, uint32_t size, double ntpTimeSec);
    void cancelAll();

    void initialise(NTPClock* clock,
                    uint8_t*              inBufferStart,
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
    void run() override;
    void checkAndDispatch();

    struct Event {
        double   ntpTimeSec;
        uint32_t size;
        uint32_t id;
        std::vector<uint8_t> data;

        bool operator>(const Event& rhs) const { return ntpTimeSec > rhs.ntpTimeSec; }
    };

    using MinHeap = std::priority_queue<Event, std::vector<Event>, std::greater<Event>>;

    NTPClock*             mClock         = nullptr;
    uint8_t*              mInBufferStart = nullptr;
    uint32_t              mInBufferSize  = 0;
    std::atomic<int32_t>* mInHead        = nullptr;
    std::atomic<int32_t>* mInTail        = nullptr;
    std::atomic<int32_t>* mInSequence    = nullptr;
    std::atomic<int32_t>* mInWriteLock   = nullptr;
    PerformanceMetrics*   mMetrics       = nullptr;
    double                mLookaheadS    = 0.500;

    juce::CriticalSection mLock;
    juce::WaitableEvent   mNewEventSignal;
    MinHeap               mHeap;
    std::atomic<uint32_t> mNextId{0};
};

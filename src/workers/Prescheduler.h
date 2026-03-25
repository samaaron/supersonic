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

#include "src/native/WallClock.h"
#include "RingBufferWriter.h"
#include "src/shared_memory.h"

class Prescheduler : public juce::Thread {
public:
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
    void run() override;
    void checkAndDispatch();

    struct Event {
        double   ntpTimeSec;
        uint32_t size;
        uint32_t id;
        std::vector<uint8_t> data;

        bool operator>(const Event& rhs) const { return ntpTimeSec > rhs.ntpTimeSec; }
    };

    // Min-heap that supports move-pop (avoids copying Event's vector data)
    struct MinHeap {
        std::vector<Event> c;
        bool empty() const { return c.empty(); }
        size_t size() const { return c.size(); }
        const Event& top() const { return c.front(); }
        void push(Event e) {
            c.push_back(std::move(e));
            std::push_heap(c.begin(), c.end(), std::greater<Event>());
        }
        Event pop_move() {
            std::pop_heap(c.begin(), c.end(), std::greater<Event>());
            Event e = std::move(c.back());
            c.pop_back();
            return e;
        }
        void clear() { c.clear(); }
    };

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

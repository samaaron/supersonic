/*
 * PreschedulerCore.h — portable OSC-out prescheduler: an NTP min-heap with
 * cancellation, and nothing else. No threads, no timers, no ring buffer, no
 * allocation. Hosts wrap it: the native build drives it from a juce::Thread,
 * the ESP32 build from a FreeRTOS task; both feed it a wall-clock NTP time and
 * a sink that writes to the engine's ring buffer.
 *
 * Behaviour is pinned by test/vectors/prescheduler.json, the single source of
 * truth for scheduler semantics, shared with the JS implementation
 * (js/lib/prescheduler_core.js).
 *
 * Events are ordered by NTP time, ties broken by insertion order (seq) → FIFO.
 * An event due at D is released when now >= D - lookahead.
 *
 * Ownership: the core never allocates. Event storage is caller-provided; the
 * OSC payload bytes live in a host-owned pool, referenced opaquely (payload +
 * bytes). Dispatch and cancellation hand each affected event back to the host
 * via a callback so it can write or reclaim the payload.
 */
#ifndef SUPERSONIC_PRESCHEDULER_CORE_H
#define SUPERSONIC_PRESCHEDULER_CORE_H

#include <cstddef>
#include <cstdint>

namespace supersonic {

// 64-bit FNV-1a over a run-tag string. Both the host (when it schedules) and
// the test runner (when it reads a vector) hash tags through this one function,
// so tag identity is consistent everywhere. nullptr / "" hash to the basis.
uint64_t PreschedulerTagHash(const char* tag);

struct PreschedulerConfig {
    double   lookaheadS = 0.500;    // event due at D releases at D - lookaheadS
    uint32_t maxPending = 1000;     // backpressure cap on heap + retryCount
    double   maxFutureS = 3600.0;   // reject events scheduled further out than this
    uint32_t poolBytes  = 524288;   // reject events whose encoded size exceeds a pool slot
};

// One queued event. Fixed-size and trivially copyable so it lives in a flat,
// caller-provided array (no per-event allocation).
struct PreschedulerEvent {
    double   ntpTime;     // absolute NTP execution time (seconds)
    uint32_t seq;         // insertion order, breaks ntpTime ties → FIFO
    uint32_t sessionId;   // routing: cancel-by-session
    uint64_t tagId;       // routing: cancel-by-tag (PreschedulerTagHash of the tag)
    uint32_t bytes;       // encoded OSC size, for the pool-slot check
    void*    payload;     // opaque handle into the host's byte pool
};

enum class PreschedulerStatus {
    Scheduled,            // queued into the heap (outScheduled is filled)
    Immediate,            // not a bundle (no time) — host should dispatch now, not queued
    RejectQueueFull,      // heap + retryCount at maxPending
    RejectTooLarge,       // bytes > poolBytes
    RejectTooFarFuture,   // ntpTime - now > maxFutureS
};

// A scheduling request. hasTime == false means "immediate" (a non-bundle):
// it is never queued — the host dispatches the payload right away.
struct PreschedulerRequest {
    bool     hasTime   = false;
    double   ntpTime   = 0.0;
    uint32_t bytes     = 0;
    uint32_t sessionId = 0;
    uint64_t tagId     = 0;
    void*    payload   = nullptr;
};

// Called once per released event in dispatchDue(), in due-time order.
using PreschedulerSink = void (*)(const PreschedulerEvent& ev, void* ctx);
// Called once per removed event in cancel*(), so the host can reclaim payload.
using PreschedulerOnRemoved = void (*)(const PreschedulerEvent& ev, void* ctx);

class PreschedulerCore {
public:
    // storage/capacity: a caller-owned array the heap lives in. capacity must
    // be >= the effective maxPending; maxPending is clamped to capacity so the
    // array can never overflow.
    PreschedulerCore(PreschedulerEvent* storage, uint32_t capacity,
                     const PreschedulerConfig& cfg);

    void setConfig(const PreschedulerConfig& cfg);
    const PreschedulerConfig& config() const { return mCfg; }

    uint32_t size() const { return mCount; }

    // Output-side retry pressure, owned by the host and counted toward the same
    // backpressure cap as queued events. Set this before schedule() to reflect
    // the current retry backlog.
    uint32_t retryCount = 0;

    // Schedule a request as of `now`. On Scheduled, *outScheduled (if non-null)
    // receives the queued event. See PreschedulerStatus for the other outcomes.
    PreschedulerStatus schedule(const PreschedulerRequest& req, double now,
                                PreschedulerEvent* outScheduled = nullptr);

    // When the next event should be released (its ntpTime - lookaheadS).
    // Returns false (leaving *out untouched) when the heap is empty.
    bool nextDueTime(double* out) const;

    // Release every event due by `now` (ntpTime <= now + lookaheadS), in order,
    // handing each to sink(ev, ctx). Returns the count released.
    uint32_t dispatchDue(double now, PreschedulerSink sink, void* ctx);

    // Cancellation. Each removes matching events and returns the count removed;
    // onRemoved (optional) is invoked per removed event so the host can reclaim
    // the payload. cancelAll empties the heap.
    uint32_t cancelTag(uint64_t tagId,
                       PreschedulerOnRemoved onRemoved = nullptr, void* ctx = nullptr);
    uint32_t cancelSession(uint32_t sessionId,
                           PreschedulerOnRemoved onRemoved = nullptr, void* ctx = nullptr);
    uint32_t cancelSessionTag(uint32_t sessionId, uint64_t tagId,
                              PreschedulerOnRemoved onRemoved = nullptr, void* ctx = nullptr);
    uint32_t cancelAll(PreschedulerOnRemoved onRemoved = nullptr, void* ctx = nullptr);

private:
    // Predicate used by the shared filter; encodes which routing fields matter.
    enum class Match { Tag, Session, SessionTag };
    uint32_t cancelMatching(Match match, uint32_t sessionId, uint64_t tagId,
                            PreschedulerOnRemoved onRemoved, void* ctx);

    // Array-backed min-heap.
    bool cmpLess(const PreschedulerEvent& a, const PreschedulerEvent& b) const;
    void swap(uint32_t i, uint32_t j);
    void push(const PreschedulerEvent& e);
    PreschedulerEvent pop();
    void siftUp(uint32_t i);
    void siftDown(uint32_t i);
    void heapify();

    PreschedulerEvent* mStorage;
    uint32_t           mCapacity;
    uint32_t           mCount = 0;
    uint32_t           mSeq   = 0;
    PreschedulerConfig mCfg;
};

} // namespace supersonic

#endif // SUPERSONIC_PRESCHEDULER_CORE_H

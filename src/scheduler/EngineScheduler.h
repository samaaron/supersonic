/*
    SuperSonic
    Copyright (c) 2025 Sam Aaron

    Dual-licensed MIT OR GPL-3.0-or-later (see repo LICENSE).

    The engine's timed-OSC scheduler: a generic delay line. It stores opaque OSC
    bytes keyed by an int64 timetag and releases them in time order; what each
    payload means and where it goes when due is the caller's concern (the fire
    loop re-enters the same dispatch() the immediate drain uses — synth inline,
    /midi/ + /osc/ forwarded by address). No backend, no scsynth — storage +
    ordering come from the generic Scheduler; this layer only adds the
    oversize/drop accounting and the process-wide instance accessors.
*/

#pragma once

#include <atomic>
#include <cstdint>

#include "Scheduler.h"
#include "../memory_profile.h"

class EngineScheduler {
public:
    // A scheduled event is bounded only by the scheduler's own data pool — the
    // engine fires due events through dispatch() (synth inline; /midi/ + /osc/
    // forwarded by address). An oversize payload can never fit the pool, so the
    // drain drops+logs it rather than back-pressuring forever.
    static constexpr uint32_t kMaxPayload = SCHEDULER_DATA_POOL_SIZE;

    // Per-event metadata: the origin token of the ingress message that scheduled
    // it, so a due event's reply (synth) routes back to that caller. 0 = no/broadcast
    // origin (e.g. engine-generated MIDI clock). Carried opaquely — just a number.
    struct EngineMeta { uint32_t origin = 0; };

    using Core  = Scheduler<EngineMeta, SCHEDULER_SLOT_COUNT, SCHEDULER_DATA_POOL_SIZE>;
    using Event = Core::Event;

    // Store an OSC packet to fire at timetag `when`, keyed by `tag` (for flush),
    // carrying the scheduling caller's `origin`. Rejects oversize payloads and a
    // full pool (both counted as drops). RT-safe.
    bool addScheduled(int64_t when, uint32_t tag, uint32_t origin, const uint8_t* osc, uint32_t len) {
        if (len > kMaxPayload) { mDropped.fetch_add(1, std::memory_order_relaxed); return false; }
        if (!mCore.add(when, tag, EngineMeta{origin}, osc, len)) {
            mDropped.fetch_add(1, std::memory_order_relaxed);   // pool full
            return false;
        }
        return true;
    }

    // ── shared queue ops (audio thread) ─────────────────────────────────────────
    Event    popDue(int64_t now) { return mCore.popDue(now); }
    void     release(const Event& e) { mCore.release(e); }
    void     flush(uint32_t tag) { mCore.flush(tag); }
    void     clear() { mCore.clear(); }
    void     requestClear() { mCore.requestClear(); }
    bool     drainPendingClear() { return mCore.drainPendingClear(); }
    int      size() const { return mCore.size(); }
    bool     full() const { return mCore.full(); }

    // Events dropped before reaching the queue (oversize vs the data pool, or
    // pool momentarily full).
    uint32_t dropped() const { return mDropped.load(std::memory_order_relaxed); }

private:
    Core                  mCore;
    std::atomic<uint32_t> mDropped{0};
};

// The process-wide scheduler (defined in audio_processor.cpp, where it is
// ticked). The fire loop re-dispatches due events through dispatch().
EngineScheduler& get_scheduler();

// Schedule an OSC packet from `ntp_seconds` (converted to an OSC timetag),
// carrying the scheduling caller's `origin` token (0 = none/broadcast).
extern "C" void ss_defer_schedule(double ntp_seconds, uint32_t tag, uint32_t origin,
                                  const uint8_t* osc, uint32_t len);

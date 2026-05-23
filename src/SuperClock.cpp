/*
 * SuperClock.cpp — shared NTP-domain getters + beat math.
 *
 * Methods that touch Link or that vary by platform (mutators, Link-aware
 * getters, Link-clock RPC, audio-thread Link Audio paths) live in the
 * platform .cpp files. This file holds only the pieces that are
 * identical on native and WASM: pure reads of the SuperClockState
 * atomics + the NTP-domain beat math composed from them.
 */
#include "SuperClock.h"
#include "shared_memory.h"

#include <atomic>
#include <cmath>

using supersonic::bitsToDouble;

std::atomic<SuperClock*> g_active_superclock{nullptr};

// ── NTP-domain getters ──────────────────────────────────────────────────

double SuperClock::getBeatOriginNtp() const {
    const SuperClockState* s = state();
    if (!s) return 0.0;
    return bitsToDouble(s->beat_origin_ntp.load(std::memory_order_relaxed));
}

double SuperClock::getIsPlayingAtNtp() const {
    const SuperClockState* s = state();
    if (!s) return 0.0;
    return bitsToDouble(s->is_playing_at_ntp.load(std::memory_order_relaxed));
}

// ── NTP-domain beat math ────────────────────────────────────────────────
// Pure functions of (bpm, beat_origin). Independent atomic reads — no
// multi-field coherence guarantee. App-thread callers don't need it;
// audio-thread callers should use captureSessionState() instead.

double SuperClock::beatAtTime(double ntpSeconds, double quantum) const {
    (void)quantum;
    return (ntpSeconds - getBeatOriginNtp()) * getBpm() / 60.0;
}

double SuperClock::phaseAtTime(double ntpSeconds, double quantum) const {
    const double beat = beatAtTime(ntpSeconds, quantum);
    double phase = std::fmod(beat, quantum);
    if (phase < 0.0) phase += quantum;
    return phase;
}

double SuperClock::timeAtBeat(double beat, double quantum) const {
    (void)quantum;
    return getBeatOriginNtp() + beat * 60.0 / getBpm();
}

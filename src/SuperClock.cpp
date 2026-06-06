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
#include <cstdio>

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

// tempo / transport read from the shared SuperClockState mirror — identical on
// every build. The mirror is written by setBpm/setIsPlaying and, on native, kept
// in sync with Link's converged value by Link's tempo/transport callbacks, so a
// relaxed atomic read here is RT-safe and consistent on every build.
double SuperClock::getBpm() const {
    const SuperClockState* s = state();
    if (!s) return 120.0;
    return bitsToDouble(s->bpm.load(std::memory_order_relaxed));
}

bool SuperClock::isPlaying() const {
    const SuperClockState* s = state();
    if (!s) return false;
    return s->is_playing.load(std::memory_order_relaxed) != 0u;
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

// ── Cross-platform clock metrics ────────────────────────────────────────
// Reads the SuperClockState SAB mirror directly (relaxed atomics) rather than
// getBpm()/isPlaying(), which on native+Link route through the locking
// captureAppSessionState() (not RT-safe). The mirror is the SAB region on WASM
// and a Link-callback-synced private mirror on native, so a direct read is
// RT-safe and consistent on both builds. Fixed-point encoding matches the
// link_* clock slots so the same display formats apply.

void SuperClock::publishClockMetrics(PerformanceMetrics* m, double ntpNow, double quantum) {
    if (!m) return;

    const SuperClockState* s = state();
    if (!s) return;

    const double bpm = bitsToDouble(s->bpm.load(std::memory_order_relaxed));
    const double beatOrigin = bitsToDouble(s->beat_origin_ntp.load(std::memory_order_relaxed));
    const bool playing = s->is_playing.load(std::memory_order_relaxed) != 0u;

    const double beat = (bpm > 0.0) ? (ntpNow - beatOrigin) * bpm / 60.0 : 0.0;
    double phase = (quantum > 0.0) ? std::fmod(beat, quantum) : 0.0;
    if (phase < 0.0) phase += quantum;

    m->clock_tempo_mbpm.store(bpm > 0.0 ? static_cast<uint32_t>(bpm * 1000.0 + 0.5) : 0u,
                              std::memory_order_relaxed);
    m->clock_beat_centi.store(beat > 0.0 ? static_cast<uint32_t>(beat * 100.0) : 0u,
                              std::memory_order_relaxed);
    m->clock_phase_centi.store(phase > 0.0 ? static_cast<uint32_t>(phase * 100.0) : 0u,
                               std::memory_order_relaxed);
    m->clock_playing.store(playing ? 1u : 0u, std::memory_order_relaxed);
}

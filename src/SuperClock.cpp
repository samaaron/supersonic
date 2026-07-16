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
#include "clock_math.h"
#include "shared_memory.h"
#include "synth/common/shm_scope_stream.hpp"  // g_engine_frames (stream anchor)

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

using supersonic::bitsToDouble;

std::atomic<SuperClock*> g_active_superclock{nullptr};

// ── Sample clock (sample position ↔ wall-clock DAC time) ───────────────

void SuperClock::bindSampleClockToShm(uint8_t* region) {
    mSampleClockRegion = region;
}

void SuperClock::advanceEngineFrames(double samplePosition) {
    g_engine_frames.store(static_cast<uint64_t>(samplePosition),
                          std::memory_order_relaxed);
}

// Seqlock writer: odd seq, release fence (orders the odd store before the
// field stores for any reader that sees them), relaxed atomic field stores,
// even seq with release. Fields are atomics so no read tears; the seq guards
// cross-field consistency. Audio thread only — single writer by construction.
void SuperClock::publishSampleClock(double samplePosition, double sampleRate,
                                    double renderNtp,
                                    uint32_t outputLatencyFrames) {
    const uint64_t frames = static_cast<uint64_t>(samplePosition);
    // Streams anchor their per-block writes here whether or not a shm
    // region is bound (headless/unit contexts).
    g_engine_frames.store(frames, std::memory_order_relaxed);

    uint8_t* sc = mSampleClockRegion;
    if (!sc || sampleRate <= 0.0) return;
    auto* seq = reinterpret_cast<std::atomic<uint32_t>*>(sc + SAMPLE_CLOCK_SEQ);
    auto* sr  = reinterpret_cast<std::atomic<uint32_t>*>(sc + SAMPLE_CLOCK_SAMPLE_RATE);
    auto* fr  = reinterpret_cast<std::atomic<uint64_t>*>(sc + SAMPLE_CLOCK_ENGINE_FRAMES);
    auto* nb  = reinterpret_cast<std::atomic<uint64_t>*>(sc + SAMPLE_CLOCK_DAC_NTP);
    auto* lat = reinterpret_cast<std::atomic<uint32_t>*>(sc + SAMPLE_CLOCK_OUT_LATENCY);

    // The anchor is speaker-time: render NTP plus the device output latency
    // (Link convention — "host time at speaker").
    const double dacNtp = renderNtp
        + static_cast<double>(outputLatencyFrames) / sampleRate;
    const uint64_t ntpBits = supersonic::doubleToBits(dacNtp);

    const uint32_t s = seq->load(std::memory_order_relaxed);
    seq->store(s + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    sr->store(static_cast<uint32_t>(sampleRate), std::memory_order_relaxed);
    fr->store(frames, std::memory_order_relaxed);
    nb->store(ntpBits, std::memory_order_relaxed);
    lat->store(outputLatencyFrames, std::memory_order_relaxed);
    seq->store(s + 2, std::memory_order_release);
}

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
    return supersonic::beatAt(ntpSeconds, getBeatOriginNtp(), getBpm());
}

double SuperClock::phaseAtTime(double ntpSeconds, double quantum) const {
    return supersonic::wrapPhase(beatAtTime(ntpSeconds, quantum), quantum);
}

double SuperClock::timeAtBeat(double beat, double quantum) const {
    (void)quantum;
    return supersonic::timeAtBeat(beat, getBeatOriginNtp(), getBpm());
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

    // Acquire-load bpm BEFORE origin: applyTempoChange stores the origin first
    // and releases bpm, so a new bpm here guarantees the matching origin.
    const double bpm = bitsToDouble(s->bpm.load(std::memory_order_acquire));
    const double beatOrigin = bitsToDouble(s->beat_origin_ntp.load(std::memory_order_relaxed));
    const bool playing = s->is_playing.load(std::memory_order_relaxed) != 0u;

    const double beat = (bpm > 0.0) ? supersonic::beatAt(ntpNow, beatOrigin, bpm) : 0.0;
    const double phase = supersonic::wrapPhase(beat, quantum);

    m->clock_tempo_mbpm.store(bpm > 0.0 ? static_cast<uint32_t>(bpm * 1000.0 + 0.5) : 0u,
                              std::memory_order_relaxed);
    m->clock_beat_centi.store(beat > 0.0 ? static_cast<uint32_t>(beat * 100.0) : 0u,
                              std::memory_order_relaxed);
    m->clock_phase_centi.store(phase > 0.0 ? static_cast<uint32_t>(phase * 100.0) : 0u,
                               std::memory_order_relaxed);
    m->clock_playing.store(playing ? 1u : 0u, std::memory_order_relaxed);
}

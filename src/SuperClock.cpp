/*
 * SuperClock.cpp — shared mutators, getters, and beat math.
 *
 * Native and WASM differ only in where the SuperClockState struct lives
 * (private member on native; SAB-resident on WASM). The platform .cpps
 * provide state(); everything else is shared.
 */
#include "SuperClock.h"
#include "shared_memory.h"

#include <atomic>
#include <cmath>

using supersonic::doubleToBits;
using supersonic::bitsToDouble;

// ── Mutators ─────────────────────────────────────────────────────────────

void SuperClock::setBpm(double bpm, double atNtpSeconds) {
    (void)atNtpSeconds;  // Honoured by a Link backing; takes effect now in session-of-one.
    SuperClockState* s = state();
    if (!s) return;
    s->bpm.store(doubleToBits(bpm), std::memory_order_relaxed);
}

void SuperClock::setIsPlaying(bool playing, double atNtpSeconds) {
    SuperClockState* s = state();
    if (!s) return;
    s->is_playing_at_ntp.store(doubleToBits(atNtpSeconds), std::memory_order_relaxed);
    s->is_playing.store(playing ? 1u : 0u, std::memory_order_relaxed);
}

void SuperClock::setLinkEnabled(bool enabled) {
    (void)enabled;
}

void SuperClock::requestBeatAtTime(double beat, double atNtpSeconds, double quantum) {
    (void)quantum;
    SuperClockState* s = state();
    if (!s) return;
    const double bpm = bitsToDouble(s->bpm.load(std::memory_order_relaxed));
    const double newOrigin = atNtpSeconds - beat * 60.0 / bpm;
    s->beat_origin_ntp.store(doubleToBits(newOrigin), std::memory_order_relaxed);
}

void SuperClock::forceBeatAtTime(double beat, double atNtpSeconds, double quantum) {
    requestBeatAtTime(beat, atNtpSeconds, quantum);
}

// ── Getters ──────────────────────────────────────────────────────────────

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

bool SuperClock::isLinkEnabled() const {
    return false;
}

size_t SuperClock::numPeers() const {
    return 0;
}

// ── Beat math ────────────────────────────────────────────────────────────

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

/*
 * clock_math.h — the shared clock arithmetic, once.
 *
 * The tempo-grid formula (beat = (t − origin) · bpm / 60), the NTP epoch
 * constant, and the NTP → OSC-timetag packing all appear in several unrelated
 * TUs (SuperClock NTP math, the session-of-one mirror, timeline free-run,
 * metrics, the standalone host, the scheduler ingest). One definition here so
 * they can't drift apart.
 */
#pragma once

#include <chrono>
#include <cmath>
#include <cstdint>

namespace supersonic {

// Seconds between the NTP epoch (1900) and the Unix epoch (1970).
inline constexpr double kNtpEpochOffset = 2208988800.0;

// Gain of the slow drift-correction IIRs (audio-thread NTP in TimeSource,
// Link-domain host anchor in SuperClock::linkAudioHostMicros): converge toward
// the reference at ~1% per audio callback — fast enough to track real drift,
// slow enough to reject callback-wake jitter.
inline constexpr double kDriftIirGain = 0.01;

inline double beatAt(double t, double origin, double bpm) {
    return (t - origin) * bpm / 60.0;
}

inline double timeAtBeat(double beat, double origin, double bpm) {
    return origin + beat * 60.0 / bpm;
}

// The origin that puts `beat` at time `t`.
inline double originFor(double beat, double t, double bpm) {
    return t - beat * 60.0 / bpm;
}

}  // namespace supersonic

// Current wall-clock time as NTP seconds (global name: pre-dates the
// namespace and is referenced unqualified across the native tree and the
// shm reader side). The engine's TimeSource and every cross-process
// sample-clock reader must use this one formula.
inline double wallClockNTP() {
    return std::chrono::duration<double>(
               std::chrono::system_clock::now().time_since_epoch()).count()
         + supersonic::kNtpEpochOffset;
}

namespace supersonic {

// Non-negative phase of `beat` within `quantum` (0 when quantum <= 0).
inline double wrapPhase(double beat, double quantum) {
    if (quantum <= 0.0) return 0.0;
    double p = std::fmod(beat, quantum);
    if (p < 0.0) p += quantum;
    return p;
}

// NTP seconds (since 1900) → OSC 64-bit 32.32 fixed-point timetag.
inline int64_t ntpToOscTimetag(double ntpSeconds) {
    const uint32_t s = static_cast<uint32_t>(ntpSeconds);
    const uint32_t f = static_cast<uint32_t>((ntpSeconds - s) * 4294967296.0);
    return static_cast<int64_t>((static_cast<uint64_t>(s) << 32) | f);
}

}  // namespace supersonic

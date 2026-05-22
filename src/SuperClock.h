/*
 * SuperClock.h — engine session-timeline service.
 *
 * Single source of truth for the engine's tempo, transport state, beat
 * origin, and NTP "now". Native can be backed by ableton::Link without
 * changing this header; WASM stays local.
 */
#pragma once

#include "shared_memory.h"

#include <cstddef>
#include <cstdint>
#include <memory>

class SuperClock {
public:
    SuperClock();
    ~SuperClock();

    SuperClock(const SuperClock&) = delete;
    SuperClock& operator=(const SuperClock&) = delete;
    SuperClock(SuperClock&&) = delete;
    SuperClock& operator=(SuperClock&&) = delete;

    // ─── Mutators (app-thread) ────────────────────────────────────────────

    void setBpm(double bpm, double atNtpSeconds);
    void setIsPlaying(bool playing, double atNtpSeconds);
    void setLinkEnabled(bool enabled);
    void requestBeatAtTime(double beat, double atNtpSeconds, double quantum);
    void forceBeatAtTime(double beat, double atNtpSeconds, double quantum);

    // ─── Session-state getters (app-thread) ───────────────────────────────

    double getBpm() const;
    bool   isPlaying() const;
    double getBeatOriginNtp() const;
    double getIsPlayingAtNtp() const;
    bool   isLinkEnabled() const;
    size_t numPeers() const;

    // ─── Beat math ────────────────────────────────────────────────────────
    // Pure functions of (bpm, beat_origin). Reads each field independently
    // — no multi-field coherence guarantee. Acceptable today because no
    // audio-thread consumer needs one.

    double beatAtTime(double ntpSeconds, double quantum) const;
    double phaseAtTime(double ntpSeconds, double quantum) const;
    double timeAtBeat(double beat, double quantum) const;

    // ─── Audio-thread API ─────────────────────────────────────────────────
    // `now()` is the app-thread read: returns the latest audio-thread NTP,
    // cached by the most recent update call. Both builds.
    //
    // `nowAt(audioCurrentTime)` is the WASM worklet's audio-thread entry
    // point: computes NTP from the supplied AudioContext currentTime and
    // publishes it to the cache. On native, audio-thread NTP comes from
    // the IIR — `nowAt` ignores its argument and returns `now()`.

    double now() const;
    double nowAt(double audioCurrentTime) const;

    // App-thread wall-clock NTP entry point. Native returns wallClockNTP()
    // directly; WASM has no fresh wall clock and returns 0.
    double wallNow() const;

    // Audio-thread time-base. Native runs one IIR step per callback;
    // WASM evaluates the SAB formula.
    double updateAudioThreadNTP(double samplePosition,
                                double sampleRate,
                                double audioCurrentTime = 0.0);
    void   resetAudioThreadTime(double samplePosition, double sampleRate);

private:
    struct Impl;
    std::unique_ptr<Impl> mImpl;

    SuperClockState*       state();
    const SuperClockState* state() const;
};

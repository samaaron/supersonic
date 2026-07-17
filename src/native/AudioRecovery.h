//--
// This file is part of Sonic Pi: http://sonic-pi.net
// Full project source: https://github.com/samaaron/sonic-pi
// License: https://github.com/samaaron/sonic-pi/blob/main/LICENSE.md
//
// Copyright 2026 by Sam Aaron (http://sam.aaron.name).
// All rights reserved.
//
// Permission is granted for use, copying, modification, and
// distribution of modified versions of this work as long as this
// notice is included.
//++

#pragma once

#include <cstdint>

// Pure, unit-agnostic audio liveness/recovery logic — the authoritative answer
// to "is the audio thread actually alive, and if not, what recovery step are we
// on". Kept free of JUCE/CoreAudio and of any real clock so the rules are
// unit-testable without hardware or timing, mirroring DevicePolicy and
// HeadlessDriver::cappedNextWake. The engine feeds it processCount samples and a
// monotonic `now` (any integer unit) and acts on its verdicts; it owns no state
// the engine also owns.
namespace sonicpi::audio {

// Derived purely from the audio-thread tick counter (processCount) sampled over
// time. The key distinction: a single resumed tick ("twitch") is Confirming,
// NOT Live — liveness requires ticks SUSTAINED across the confirm window, so a
// device that emits one callback per reopen attempt can never masquerade as
// recovered.
enum class LivenessPhase {
    Live,        // ticks have been sustained for >= the confirm window
    Confirming,  // ticks resumed after a stall but not yet sustained
    Stalled,     // no tick for >= the stall window
};

class LivenessMonitor {
public:
    // stallWindow: no-tick duration that counts as a stall.
    // confirmWindow: how long ticks must keep advancing, after a stall, before
    // the audio is trusted as Live again. Same integer unit as `now`.
    LivenessMonitor(int64_t stallWindow, int64_t confirmWindow);

    // Feed the current audio tick counter sampled at time `now`.
    void observe(uint64_t tickCount, int64_t now);

    // Current phase at time `now`. Pure query — Stalled can be reached by time
    // passing with no new observation.
    LivenessPhase phase(int64_t now) const;

private:
    int64_t  mStallWindow;
    int64_t  mConfirmWindow;
    uint64_t mLastCount   = 0;
    int64_t  mLastAdvance = 0;   // time tickCount last increased
    int64_t  mRunStart    = 0;   // time the current uninterrupted advancing run began
    bool     mSeen        = false;
};

// Detects a device whose callbacks keep ticking (LivenessMonitor reads Live)
// but deliver samples at the wrong rate — the post-sleep DirectSound failure
// where the emulation timer free-runs fast or slow. The clock IIR
// (TimeSource::updateAudioThreadNTP) then parks at a permanent equilibrium
// offset between wall clock and audio timebase (drift where correction rate
// equals inflow), which no NTP re-anchor can converge; only reopening the
// device restores the rate.
//
// Feed cumulative rendered frames plus a monotonic `now`; every completed
// window yields a delivered/nominal ratio, and only N CONSECUTIVE
// out-of-tolerance windows produce a verdict. One window is never enough: a
// single transient callback stall (the "[gap] audio callback stalled" case)
// skews that window's ratio and must not trigger a cold swap.
class RateSkewMonitor {
public:
    // window: measurement span per ratio. maxGap: observation gap that marks a
    // discontinuity (the caller stopped sampling — swap in flight, benign
    // states, machine asleep). tolerance: fractional deviation from nominal
    // that makes a window bad. badWindowsRequired: consecutive bad windows
    // before skewed() reports true. window/maxGap in the same unit as `now`.
    RateSkewMonitor(int64_t window, int64_t maxGap, double tolerance,
                    int badWindowsRequired);

    // Feed cumulative rendered frames at time `now`, with the nominal device
    // rate in frames per time-unit. An observation gap > maxGap, a frames
    // rollback (device restart resets the counter) or a nominal-rate change
    // discards the current window AND the bad streak — each marks a
    // discontinuity the verdict must restart from.
    void observe(uint64_t frames, double nominalFramesPerUnit, int64_t now);

    bool   skewed()    const { return mBadStreak >= mBadWindowsRequired; }
    // delivered/nominal of the last completed window (1.0 before the first).
    double lastRatio() const { return mLastRatio; }
    void   reset();

private:
    int64_t mWindow;
    int64_t mMaxGap;
    double  mTolerance;
    int     mBadWindowsRequired;

    bool     mSeen        = false;
    uint64_t mStartFrames = 0;   // window anchor
    int64_t  mStartTime   = 0;
    uint64_t mLastFrames  = 0;   // previous observation (discontinuity checks)
    int64_t  mLastTime    = 0;
    double   mRate        = 0.0; // nominal frames-per-unit the window was anchored with
    int      mBadStreak   = 0;
    double   mLastRatio   = 1.0;
};

} // namespace sonicpi::audio

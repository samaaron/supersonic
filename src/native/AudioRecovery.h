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

} // namespace sonicpi::audio

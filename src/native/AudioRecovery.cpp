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

#include "AudioRecovery.h"

namespace sonicpi::audio {

LivenessMonitor::LivenessMonitor(int64_t stallWindow, int64_t confirmWindow)
    : mStallWindow(stallWindow), mConfirmWindow(confirmWindow) {}

void LivenessMonitor::observe(uint64_t tickCount, int64_t now) {
    if (!mSeen) {
        // Seed as already-Live: a fresh boot that is ticking needs no confirm
        // window. Backdating mRunStart by the confirm window makes phase() read
        // Live immediately.
        mSeen        = true;
        mLastCount   = tickCount;
        mLastAdvance = now;
        mRunStart    = now - mConfirmWindow;
        return;
    }

    // Whether we had already crossed the stall threshold as of this sample,
    // judged against the LAST advance (before we fold in this observation).
    // mLastAdvance is frozen until a tick advances and `now` only grows, so once
    // this is true it stays true until the next advance — no separate "was
    // stalled" flag is needed.
    const bool stalledNow = (now - mLastAdvance) >= mStallWindow;

    if (tickCount != mLastCount) {
        // Ticks advanced. If they are resuming out of a stall, this begins a
        // fresh run that must survive the confirm window before it counts as
        // Live — so a single twitch tick (one callback per reopen) can never
        // masquerade as recovered.
        if (stalledNow) mRunStart = now;
        mLastAdvance = now;
        mLastCount   = tickCount;
    }
}

LivenessPhase LivenessMonitor::phase(int64_t now) const {
    if (!mSeen) return LivenessPhase::Stalled;
    if (now - mLastAdvance >= mStallWindow)   return LivenessPhase::Stalled;
    if (now - mRunStart    >= mConfirmWindow) return LivenessPhase::Live;
    return LivenessPhase::Confirming;
}

RateSkewMonitor::RateSkewMonitor(int64_t window, int64_t maxGap,
                                 double tolerance, int badWindowsRequired)
    : mWindow(window), mMaxGap(maxGap), mTolerance(tolerance),
      mBadWindowsRequired(badWindowsRequired) {}

void RateSkewMonitor::reset() {
    mSeen      = false;
    mBadStreak = 0;
}

void RateSkewMonitor::observe(uint64_t frames, double nominalFramesPerUnit,
                              int64_t now) {
    // No usable nominal rate (device mid-transition) — nothing measured now
    // can be trusted later.
    if (nominalFramesPerUnit <= 0.0) {
        reset();
        return;
    }

    const bool discontinuity = !mSeen
        || (now - mLastTime) > mMaxGap
        || frames < mLastFrames
        || nominalFramesPerUnit != mRate;
    if (discontinuity) {
        mSeen        = true;
        mStartFrames = frames;
        mStartTime   = now;
        mRate        = nominalFramesPerUnit;
        mBadStreak   = 0;
        mLastFrames  = frames;
        mLastTime    = now;
        return;
    }

    mLastFrames = frames;
    mLastTime   = now;

    const int64_t span = now - mStartTime;
    if (span < mWindow) return;

    const double delivered = static_cast<double>(frames - mStartFrames);
    const double ratio =
        delivered / (mRate * static_cast<double>(span));
    mLastRatio = ratio;

    const double deviation = ratio < 1.0 ? 1.0 - ratio : ratio - 1.0;
    if (deviation > mTolerance) ++mBadStreak;
    else                        mBadStreak = 0;

    // Next window starts where this one ended — contiguous coverage, no
    // overlap, so badWindowsRequired windows means that much sustained skew.
    mStartFrames = frames;
    mStartTime   = now;
}

} // namespace sonicpi::audio

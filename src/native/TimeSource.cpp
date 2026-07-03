/*
 * TimeSource.cpp — audio-thread time-source. Two shapes, one guard
 * (SUPERSONIC_WORKLET_CLOCK), matching TimeSource.h.
 *
 * native (WallClock + IIR): now() / nowAt() / updateAudioThreadNTP() are
 * audio-thread reads: concrete (no vtable), atomics only. updateAudioThreadNTP
 * runs one low-pass step per callback, converging the sample-derived clock
 * toward the wall clock at ~1% per call; freewheel mode bypasses that and
 * returns a pure sample-derived NTP.
 *
 * worklet (self-driven): nowAt() evaluates the SAB time formula from the bound
 * offset pointers and publishes it to the now()-cache — no wall clock, no IIR,
 * no freewheel.
 */
#include "native/TimeSource.h"

using supersonic::doubleToBits;
using supersonic::bitsToDouble;

extern "C" int ss_log(const char* fmt, ...);

#if SUPERSONIC_WORKLET_CLOCK

double TimeSource::now() const {
    return bitsToDouble(mLastAudioThreadNTPBits.load(std::memory_order_acquire));
}

double TimeSource::nowAt(double audioCurrentTime) const {
    const double drift_seconds =
        mDriftOffset ? (mDriftOffset->load(std::memory_order_acquire) / 1000000.0) : 0.0;
    const double ntp_start =
        (mNtpStartTime && *mNtpStartTime != 0.0) ? *mNtpStartTime : 0.0;
    const double global_seconds =
        mGlobalOffset ? (mGlobalOffset->load(std::memory_order_relaxed) / 1000.0) : 0.0;
    const double result = audioCurrentTime + ntp_start + drift_seconds + global_seconds;
    mLastAudioThreadNTPBits.store(doubleToBits(result), std::memory_order_release);
    return result;
}

double TimeSource::wallNow() const {
    return 0.0;
}

double TimeSource::updateAudioThreadNTP(double samplePosition,
                                        double sampleRate,
                                        double audioCurrentTime) {
    (void)samplePosition;
    (void)sampleRate;
    return nowAt(audioCurrentTime);
}

void TimeSource::resetAudioThreadTime(double samplePosition, double sampleRate) {
    (void)samplePosition;
    (void)sampleRate;
}

void TimeSource::setFreewheelClock(bool enabled) {
    // No-op: the worklet evaluates the SAB time formula in nowAt() and has no
    // headless driver / drift IIR to bypass.
    (void)enabled;
}

#else  // !SUPERSONIC_WORKLET_CLOCK

double TimeSource::now() const {
    const uint64_t bits =
        mCurrentAudioThreadNTPBits.load(std::memory_order_acquire);
    if (bits == 0) return readWallClock();
    return bitsToDouble(bits);
}

double TimeSource::nowAt(double audioCurrentTime) const {
    (void)audioCurrentTime;  // Native uses its own time source.
    return now();
}

double TimeSource::wallNow() const {
    return readWallClock();
}

double TimeSource::updateAudioThreadNTP(double samplePosition,
                                        double sampleRate,
                                        double audioCurrentTime) {
    (void)audioCurrentTime;
    const double sampleOffsetSec = samplePosition / sampleRate;

    // Freewheel: pure sample-derived NTP, no wall-clock drift IIR. The headless
    // driver thread can be preempted on a busy machine; chasing that as "drift"
    // injects scheduling jitter a real device callback never sees. Deterministic
    // for offline/accuracy tests.
    if (mFreewheelClock.load(std::memory_order_relaxed)) {
        const double wallNTP =
            bitsToDouble(mBaseNTPBits.load(std::memory_order_relaxed))
            + sampleOffsetSec;
        mCurrentAudioThreadNTPBits.store(doubleToBits(wallNTP),
                                         std::memory_order_release);
        return wallNTP;
    }

    //   sampleNTP = mBaseNTP + samplePosition / sampleRate
    //   drift     = wallNow - sampleNTP
    //   mBaseNTP += drift * 0.01   (low-pass converge ~1% per call)
    //   result    = mBaseNTP + samplePosition / sampleRate
    const double wallNow = readWallClock();
    const double baseNTP = bitsToDouble(
        mBaseNTPBits.load(std::memory_order_relaxed));
    const double drift = wallNow - (baseNTP + sampleOffsetSec);

    // Surface timebase disturbances (wall-clock step/slew, callback stall):
    // steady-state drift is µs-level, so anything past 5ms is an event worth
    // logging. A genuine step stays over threshold while the IIR converges
    // (~1%/callback), so rate-limit to one line/second — the decaying values
    // trace the re-convergence. ss_log is a lock-free egress-ring write.
    const double absDrift = drift < 0.0 ? -drift : drift;
    if (absDrift > 0.005) {
        ++mDriftOverCount;
        if (wallNow - mLastDriftLogNTP >= 1.0) {
            mLastDriftLogNTP = wallNow;
            ss_log("DRIFT: wall clock %+.1fms from audio timebase, re-converging (count=%u)",
                   drift * 1000.0, mDriftOverCount);
        }
    }

    const double newBaseNTP = baseNTP + drift * 0.01;
    const double wallNTP = newBaseNTP + sampleOffsetSec;
    mBaseNTPBits.store(doubleToBits(newBaseNTP), std::memory_order_relaxed);
    mCurrentAudioThreadNTPBits.store(doubleToBits(wallNTP),
                                     std::memory_order_release);
    return wallNTP;
}

void TimeSource::resetAudioThreadTime(double samplePosition, double sampleRate) {
    const double sampleOffsetSec = samplePosition / sampleRate;
    const double newBaseNTP = readWallClock() - sampleOffsetSec;
    mBaseNTPBits.store(doubleToBits(newBaseNTP), std::memory_order_relaxed);
    mCurrentAudioThreadNTPBits.store(
        doubleToBits(newBaseNTP + sampleOffsetSec),
        std::memory_order_release);
}

void TimeSource::setFreewheelClock(bool enabled) {
    mFreewheelClock.store(enabled, std::memory_order_relaxed);
}

#endif  // SUPERSONIC_WORKLET_CLOCK

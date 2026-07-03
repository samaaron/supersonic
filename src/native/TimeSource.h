/*
 * TimeSource.h — audio-thread time-source. Two shapes, one guard.
 *
 * SUPERSONIC_WORKLET_CLOCK selects the variant:
 *
 *   undefined (native) — the WallClock + IIR clock. An IIR-smoothed wall clock
 *     anchored to the monotonic sample counter; each callback runs one IIR step
 *     (updateAudioThreadNTP) and now() returns the latest cached value. Freewheel
 *     mode bypasses the drift IIR and derives NTP purely from sample position
 *     (deterministic for offline / accuracy tests).
 *
 *   defined (WASM / ESP32) — the self-driven worklet clock. now()/nowAt() evaluate
 *     the SAB time formula (audioCurrentTime + ntp_start + drift + global) from
 *     offset pointers bound by the host at boot (bindOffsets). There is no headless
 *     driver thread or wall clock here, so freewheel is a no-op and wallNow() is 0.
 *
 * Both shapes depend only on the centralised double↔uint64 bit-cast — no JUCE,
 * no Ableton, no scsynth. The native shape additionally pulls WallClock. The
 * Link-clock-domain host-micros anchor (linkAudioHostMicros) is a separate bridge
 * that lives in SuperClockNative, not here.
 *
 * Dispatch is link-time-concrete (no vtable) so the audio-thread reads
 * (now / nowAt / updateAudioThreadNTP) stay inlinable and branch-free.
 */
#pragma once

#include "shared_memory.h"

#include <atomic>
#include <cstdint>

#if SUPERSONIC_WORKLET_CLOCK

// ─── Worklet time-source (self-driven; WASM / ESP32) ─────────────────────────
// now()/nowAt() evaluate the SAB time formula (audioCurrentTime + ntp_start +
// drift + global) from offset pointers bound at boot. The host (WASM init_memory)
// binds the SAB offset pointers via bindOffsets; an unbound source returns the
// last cached value (0 before the first nowAt).
class TimeSource {
public:
    TimeSource() = default;

    TimeSource(const TimeSource&) = delete;
    TimeSource& operator=(const TimeSource&) = delete;

    // Bind the host's SAB offset pointers (NTP start / drift µs / global ms).
    // Called once at boot before the audio thread runs — no concurrency.
    void bindOffsets(const double* ntpStartTime,
                     const std::atomic<int32_t>* driftOffset,
                     const std::atomic<int32_t>* globalOffset) {
        mNtpStartTime = ntpStartTime;
        mDriftOffset  = driftOffset;
        mGlobalOffset = globalOffset;
    }

    double now() const;
    double nowAt(double audioCurrentTime) const;
    double wallNow() const;

    double updateAudioThreadNTP(double samplePosition,
                                double sampleRate,
                                double audioCurrentTime);
    void   resetAudioThreadTime(double samplePosition, double sampleRate);

    void   setFreewheelClock(bool enabled);

private:
    const double*               mNtpStartTime{nullptr};
    const std::atomic<int32_t>* mDriftOffset{nullptr};
    const std::atomic<int32_t>* mGlobalOffset{nullptr};

    // Cache published by nowAt(), read by now(). Bit-pattern in uint64 because
    // std::atomic<double> isn't lock-free everywhere.
    mutable std::atomic<uint64_t> mLastAudioThreadNTPBits{0};
};

#else  // !SUPERSONIC_WORKLET_CLOCK

#include "native/WallClock.h"

// ─── Native time-source (WallClock + IIR) ────────────────────────────────────
class TimeSource {
public:
    TimeSource() = default;

    TimeSource(const TimeSource&) = delete;
    TimeSource& operator=(const TimeSource&) = delete;

    // App-thread read: the latest audio-thread NTP cached by the most recent
    // update call. Falls back to a fresh wall-clock read before the first update.
    double now() const;
    // Native uses its own time source — the supplied AudioContext currentTime
    // (the WASM worklet's input) is ignored.
    double nowAt(double audioCurrentTime) const;
    // App-thread wall-clock NTP entry point.
    double wallNow() const;

    // Audio-thread time-base: one IIR step per callback, returning the
    // drift-corrected NTP for this block (or the pure sample-derived NTP in
    // freewheel mode).
    double updateAudioThreadNTP(double samplePosition,
                                double sampleRate,
                                double audioCurrentTime);
    void   resetAudioThreadTime(double samplePosition, double sampleRate);

    void   setFreewheelClock(bool enabled);

    // Test seam: when non-zero, the IIR reads this instead of wallClockNTP(),
    // so scenario tests (test_time_source.cpp) can drive exact stall /
    // rate-mismatch shapes against exact expected answers. Costs one relaxed
    // atomic load on the audio path; zero (the default) = real clock.
    void setTestWallClock(double ntpSeconds) {
        mTestWallBits.store(supersonic::doubleToBits(ntpSeconds),
                            std::memory_order_relaxed);
    }

private:
    double readWallClock() const {
        const uint64_t bits = mTestWallBits.load(std::memory_order_relaxed);
        return bits ? supersonic::bitsToDouble(bits) : wallClockNTP();
    }

    std::atomic<uint64_t> mTestWallBits{0};
    // IIR-smoothed audio-thread NTP. Bit-pattern in uint64 because
    // std::atomic<double> isn't lock-free everywhere.
    std::atomic<uint64_t> mBaseNTPBits{0};
    std::atomic<uint64_t> mCurrentAudioThreadNTPBits{0};
    // When true, updateAudioThreadNTP skips the wall-clock drift IIR and
    // returns a pure sample-derived NTP (deterministic; see setFreewheelClock).
    std::atomic<bool>     mFreewheelClock{false};
    // Drift-step logging state (audio-thread only).
    double   mLastDriftLogNTP{0.0};
    uint32_t mDriftOverCount{0};
};

#endif  // SUPERSONIC_WORKLET_CLOCK

/*
 * WallClock.h — Wall-clock NTP time (free function)
 *
 * Returns the current wall-clock time as NTP seconds.
 * Used by OscUdpServer and Prescheduler for timestamping and scheduling.
 *
 * ## Drift architecture (web vs native)
 *
 * Both targets derive audio-thread NTP from the sample counter for jitter-free
 * timing, then apply a slow correction to stay aligned with the wall clock.
 *
 * Native: The audio callback can read the wall clock directly, so it runs an
 * IIR low-pass filter (alpha=0.01) every ~2.7ms audio block:
 *     baseNTP += (wallNow - sampleNTP) * 0.01
 * This smooths out OS scheduling jitter and clock quantisation.
 *
 * Web: The AudioWorklet cannot read the wall clock, so the main-thread JS
 * measures drift via getOutputTimestamp() every 1s and writes the correction
 * to SharedArrayBuffer. The WASM audio thread reads the correction each frame:
 *     currentNTP = currentTime + ntpStart + driftUs/1e6 + globalOffset
 * Drift is stored in microseconds (Int32) to avoid the 1ms quantisation
 * artifacts that would come from integer milliseconds.
 */
#pragma once

#include <chrono>

static constexpr double NTP_EPOCH_OFFSET = 2208988800.0;

inline double wallClockNTP() {
    auto now = std::chrono::system_clock::now();
    double secsSinceEpoch = std::chrono::duration<double>(
        now.time_since_epoch()).count();
    return secsSinceEpoch + NTP_EPOCH_OFFSET;
}

/*
 * WallClock.h — current wall-clock time as NTP seconds.
 */
#pragma once

#include "clock_math.h"

#include <chrono>

inline double wallClockNTP() {
    auto now = std::chrono::system_clock::now();
    double secsSinceEpoch = std::chrono::duration<double>(
        now.time_since_epoch()).count();
    return secsSinceEpoch + supersonic::kNtpEpochOffset;
}

/*
 * WallClock.h — current wall-clock time as NTP seconds.
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

/*
    SuperSonic
    Copyright (c) 2025 Sam Aaron

    Dual-licensed MIT OR GPL-3.0-or-later (see repo LICENSE).

    Wall-clock OSC time for the standalone host. Timetags are NTP (seconds since
    1900) packed as a 64-bit fixed-point value, matching the domain Sonic Pi uses
    for its scheduled /schedule messages.
*/

#pragma once

#include "clock_math.h"

#include <chrono>
#include <cstdint>

namespace ss_host {

inline int64_t ntp_to_osc_timetag(double ntp_seconds) {
    return supersonic::ntpToOscTimetag(ntp_seconds);
}

// Current time as an OSC timetag from the system clock.
inline int64_t osc_now() {
    using namespace std::chrono;
    auto now = system_clock::now().time_since_epoch();
    int64_t us = duration_cast<microseconds>(now).count();
    double unix_seconds = static_cast<double>(us) / 1'000'000.0;
    return ntp_to_osc_timetag(unix_seconds + supersonic::kNtpEpochOffset);
}

}  // namespace ss_host

/*
    SuperSonic
    Copyright (c) 2025 Sam Aaron

    Dual-licensed MIT OR GPL-3.0-or-later (see repo LICENSE).

    Wall-clock OSC time for the standalone host. Timetags are NTP (seconds since
    1900) packed as a 64-bit fixed-point value, matching the domain Sonic Pi uses
    for its scheduled /schedule messages.
*/

#pragma once

#include <chrono>
#include <cstdint>

namespace ss_host {

// Seconds between the NTP epoch (1900) and the Unix epoch (1970).
constexpr int64_t kNtpUnixOffset = 2208988800LL;

inline int64_t ntp_to_osc_timetag(double ntp_seconds) {
    uint32_t s = static_cast<uint32_t>(ntp_seconds);
    uint32_t f = static_cast<uint32_t>((ntp_seconds - s) * 4294967296.0);
    return static_cast<int64_t>((static_cast<uint64_t>(s) << 32) | f);
}

// Current time as an OSC timetag from the system clock.
inline int64_t osc_now() {
    using namespace std::chrono;
    auto now = system_clock::now().time_since_epoch();
    int64_t us = duration_cast<microseconds>(now).count();
    double unix_seconds = static_cast<double>(us) / 1'000'000.0;
    return ntp_to_osc_timetag(unix_seconds + static_cast<double>(kNtpUnixOffset));
}

}  // namespace ss_host

/*
 * SuperSonic
 * Copyright (c) 2025 Sam Aaron
 *
 * Dual-licensed MIT OR GPL-3.0-or-later (see repo LICENSE).
 *
 * schedule_parse.h — the two ways to schedule, parsed in one place so the engine
 * and the standalone host agree on the wire form:
 *   - a timestamped OSC bundle ("#bundle" + an 8-byte timetag), and
 *   - "/schedule <timetag> <blob>", the flat twin (a single addressed inner
 *     message the scheduler re-ingests on time).
 * Pure byte work — no synth, no engine globals.
 */
#pragma once

#include <cstdint>
#include <cstring>

// NTP-seconds (double) → OSC int64 timetag (seconds<<32 | fraction).
inline int64_t ss_ntp_to_timetag(double ntp) {
    uint32_t s = static_cast<uint32_t>(ntp);
    uint32_t f = static_cast<uint32_t>((ntp - static_cast<double>(s)) * 4294967296.0);
    return static_cast<int64_t>((static_cast<uint64_t>(s) << 32) | f);
}

// A timestamped OSC bundle: "#bundle" + an 8-byte timetag.
inline bool ss_is_bundle(const uint8_t* data, uint32_t size) {
    return size >= 16 && std::memcmp(data, "#bundle", 7) == 0;
}

// The 8-byte NTP timetag at offset 8 of a bundle.
inline uint64_t ss_bundle_timetag(const uint8_t* bundle) {
    uint64_t t = 0;
    for (int i = 0; i < 8; ++i) t = (t << 8) | static_cast<uint8_t>(bundle[8 + i]);
    return t;
}

struct SchedulePacket {
    bool           ok      = false;
    int64_t        when    = 0;        // OSC timetag
    const uint8_t* blob    = nullptr;  // inner OSC to re-dispatch on time
    uint32_t       blobLen = 0;
};

// Parse "/schedule <timetag> <blob>". `timetag` is the OSC int64 'h' (full
// sub-sample resolution) or, as a convenience, a 'd'/'f' NTP-seconds value.
inline SchedulePacket ss_parse_schedule(const uint8_t* msg, uint32_t size) {
    SchedulePacket r;
    // Address "/schedule" (9 chars) + NUL, padded to 12 bytes; type tag at [12].
    if (size < 20 || std::memcmp(msg, "/schedule", 9) != 0 || msg[9] != '\0') return r;
    const char*    tt  = reinterpret_cast<const char*>(msg) + 12;   // ",<t>b\0"
    const uint8_t* p   = msg + 16;                                  // args (tag padded to 4)
    const uint8_t* end = msg + size;
    if (tt[0] != ',') return r;
    if (tt[1] == 'h') {                        // int64 OSC timetag
        if (p + 8 > end) return r;
        uint64_t v = 0; for (int i = 0; i < 8; ++i) v = (v << 8) | p[i];
        r.when = static_cast<int64_t>(v); p += 8;
    } else if (tt[1] == 'd') {                 // double NTP seconds
        if (p + 8 > end) return r;
        uint64_t b = 0; for (int i = 0; i < 8; ++i) b = (b << 8) | p[i];
        double ntp; std::memcpy(&ntp, &b, 8);
        r.when = ss_ntp_to_timetag(ntp); p += 8;
    } else if (tt[1] == 'f') {                 // float NTP seconds
        if (p + 4 > end) return r;
        uint32_t b = 0; for (int i = 0; i < 4; ++i) b = (b << 8) | p[i];
        float ntp; std::memcpy(&ntp, &b, 4);
        r.when = ss_ntp_to_timetag(static_cast<double>(ntp)); p += 4;
    } else return r;
    if (tt[2] != 'b') return r;                // inner OSC blob
    if (p + 4 > end) return r;
    uint32_t n = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
                 (uint32_t(p[2]) << 8)  |  uint32_t(p[3]);
    p += 4;
    if (p + n > end) return r;
    r.blob = p; r.blobLen = n; r.ok = true;
    return r;
}

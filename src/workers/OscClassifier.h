/*
 * OscClassifier.h — Bundle/message classification (header-only)
 *
 * Mirrors osc_classifier.js:
 *   IMMEDIATE    — plain OSC message (not a bundle) or timetag 0/1
 *   NEAR_FUTURE  — within lookahead window (send directly to IN buffer)
 *   FAR_FUTURE   — beyond lookahead window (send to Prescheduler)
 *   LATE         — past the current time (treat as IMMEDIATE)
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>

enum class OscCategory {
    IMMEDIATE,
    NEAR_FUTURE,
    FAR_FUTURE,
    LATE
};

class OscClassifier {
public:
    // lookaheadS: how far ahead (in seconds) to dispatch directly vs. preschedule
    explicit OscClassifier(double lookaheadS = 0.500) : lookahead(lookaheadS) {}

    void setLookahead(double s) { lookahead.store(s, std::memory_order_relaxed); }
    double getLookahead() const { return lookahead.load(std::memory_order_relaxed); }

    // Classify a raw OSC packet given the current NTP wall-clock time.
    // wallNtpNow: result of NTPClock::wallNTP()
    OscCategory classify(const uint8_t* data, uint32_t size, double wallNtpNow) const {
        if (size < 8) return OscCategory::IMMEDIATE;

        // Check if it is a bundle ("#bundle\0")
        if (std::memcmp(data, "#bundle\0", 8) != 0) {
            return OscCategory::IMMEDIATE;  // Plain OSC message
        }

        if (size < 16) return OscCategory::IMMEDIATE;

        // Extract NTP timetag at bytes 8..15 (big-endian)
        uint64_t timetag = 0;
        for (int i = 0; i < 8; i++) {
            timetag = (timetag << 8) | data[8 + i];
        }

        // Timetag 0 or 1 means "execute immediately"
        if (timetag == 0 || timetag == 1) {
            return OscCategory::IMMEDIATE;
        }

        // Convert NTP timetag (fixed-point 32.32) to seconds
        double tagSec = static_cast<double>(timetag >> 32)
                      + static_cast<double>(timetag & 0xFFFFFFFFULL) / 4294967296.0;

        double diff = tagSec - wallNtpNow;  // positive = in the future
        double la = lookahead.load(std::memory_order_relaxed);

        if (diff < 0.0) {
            return OscCategory::LATE;
        } else if (diff <= la) {
            return OscCategory::NEAR_FUTURE;
        } else {
            return OscCategory::FAR_FUTURE;
        }
    }

    // Extract the NTP timetag from a bundle as double seconds.
    // Returns 0.0 if not a bundle or too small.
    static double bundleTimeSec(const uint8_t* data, uint32_t size) {
        if (size < 16 || std::memcmp(data, "#bundle\0", 8) != 0) return 0.0;
        uint64_t timetag = 0;
        for (int i = 0; i < 8; i++) timetag = (timetag << 8) | data[8 + i];
        return static_cast<double>(timetag >> 32)
             + static_cast<double>(timetag & 0xFFFFFFFFULL) / 4294967296.0;
    }

private:
    std::atomic<double> lookahead;
};

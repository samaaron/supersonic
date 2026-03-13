/*
 * NTPClock.h — Wall-clock NTP time + audio-to-NTP conversion
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <juce_core/juce_core.h>

class NTPClock {
public:
    static constexpr double NTP_EPOCH_OFFSET = 2208988800.0;

    NTPClock() = default;

    double wallNTP() const {
        double unixSec = static_cast<double>(juce::Time::currentTimeMillis()) * 0.001;
        return unixSec + NTP_EPOCH_OFFSET + driftOffsetSec.load(std::memory_order_relaxed);
    }

    double audioToNTP(double audioTimeSec) const {
        return audioTimeSec + ntpAtAudioZero.load(std::memory_order_relaxed);
    }

    void setAudioZero(double ntpNow) {
        ntpAtAudioZero.store(ntpNow, std::memory_order_relaxed);
    }

    void updateDrift(double audioTimeSec, double wallNtpNow) {
        double expectedNTP = audioToNTP(audioTimeSec);
        double error = wallNtpNow - expectedNTP;
        double current = driftOffsetSec.load(std::memory_order_relaxed);
        driftOffsetSec.store(current + error * 0.01, std::memory_order_relaxed);
    }

    double getNtpAtAudioZero() const {
        return ntpAtAudioZero.load(std::memory_order_relaxed);
    }

    double getDriftOffset() const {
        return driftOffsetSec.load(std::memory_order_relaxed);
    }

private:
    std::atomic<double> ntpAtAudioZero{0.0};
    std::atomic<double> driftOffsetSec{0.0};
};

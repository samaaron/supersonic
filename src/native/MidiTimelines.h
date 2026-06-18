/*
 * MidiTimelines.h — MIDI-clock follower-timeline registry.
 *
 * A fixed K-slot registry of midi:<port> follower timelines, independent of
 * Ableton Link. The MIDI subsystem feeds tempo / transport per port; OSC
 * clients read the timelines via /clock/midi:<port>/*. Slot assignment,
 * primary selection, and staleness all live here.
 *
 * Depends only on the SuperClock clock core: a SuperClock& back-reference
 * supplies the clock-domain "now" (linkClockMicros) and the id==0 Link reads
 * that timeline-parameterised queries route to. No Ableton/Link headers, no
 * threads beyond the std::mutex that serialises off-RT access.
 *
 * Timeline id: 0 = Link (routes to the SuperClock& reads); 1..K = midi slots.
 * Beat math for midi timelines is in the Link-clock micros domain so OSC RPCs
 * answer identically to the Link timeline.
 */
#pragma once

#include "SuperClock.h"
#include "memory_profile.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

class MidiTimelines {
public:
    explicit MidiTimelines(SuperClock& clock) : mClock(clock) {}

    MidiTimelines(const MidiTimelines&) = delete;
    MidiTimelines& operator=(const MidiTimelines&) = delete;

    int  claimMidiTimeline(const char* normalized, const char* raw);
    void freeMidiTimeline(int id);
    int  resolveTimeline(const char* name) const;
    int  resolveOrClaimTimeline(const char* name);

    void midiTimelinePulse(int id, uint64_t tsUs);
    void setMidiTimelineTempo(int id, double bpm);
    void setMidiTimelineTransport(int id, int kind, double beat);
    void tickMidiStaleness();

    double  timelineBpm(int id) const;
    bool    timelineIsPlaying(int id) const;
    bool    timelineIsAnchored(int id) const;
    int64_t timelineTimeForIsPlayingMicros(int id) const;
    double  timelineBeatAtLinkTime(int id, int64_t timeMicros, double quantum) const;
    double  timelinePhaseAtLinkTime(int id, int64_t timeMicros, double quantum) const;
    int64_t timelineTimeAtBeatLinkMicros(int id, double beat, double quantum) const;

    std::vector<SuperClock::TimelineInfo> listTimelines() const;
    void setTimelinesChangedCallback(std::function<void()> cb);

private:
    // One follower timeline. Beat is the exact 0xF8 count pinned to the latest
    // pulse; periodUs (the smoothed window-mean tempo) only extrapolates between
    // pulses, never integrating into the beat. All access under mMtx.
    struct MidiTimeline {
        static constexpr double kPpqn            = 24.0;
        static constexpr double kDefaultPeriodUs = 60.0e6 / (60.0 * 24.0); // 60 BPM
        // Tempo = MEAN of a window of inter-pulse intervals (averages out OS
        // arrival bunching). An adaptive outlier test (deviation
        // > kOutlierSd * window-SD) separates a real tempo STEP from ±jitter:
        // kStepConfirm same-side outliers reset the window to the new value.
        static constexpr int    kWin           = 48;  // window (~2 beats)
        static constexpr double kOutlierSd     = 3.0; // step threshold in SDs
        static constexpr int    kMinForOutlier = 10;  // samples before judging outliers
        static constexpr int    kStepConfirm   = 3;   // same-side outliers before reset

        bool        active{false};
        bool        stale{false};
        std::string normalized;            // OSC-safe handle: match key + address
        std::string raw;                   // original OS device name (display)

        // Beat is the exact 0xF8 count, pinned to the latest pulse
        // (beat = pulseCount/24 at lastTsEngine). periodUs only extrapolates
        // between/after pulses; it is never integrated into the beat.
        int64_t     pulseCount{0};         // 0xF8s since the last transport reset
        double      baseBeat{0.0};         // beat at pulseCount == 0 (Start=0, SPP=beat)
        int64_t     tsToEngineUs{0};       // pulse-clock -> engine-clock offset, set once
        bool        tsOriginSet{false};
        std::array<double, kWin> ivBuf{};  // recent inter-pulse intervals (us)
        int         ivCount{0}, ivHead{0};
        double      ivSum{0.0}, ivSumSq{0.0};
        double      periodUs{kDefaultPeriodUs}; // smoothed us per pulse (= mean ivBuf)
        double      lastTsEngine{0.0};     // engine-time of the latest pulse (phase anchor)
        int         pulses{0};             // 0 = none, 1 = have anchor, >=2 = locked
        int         outlierRun{0};         // consecutive same-side step outliers
        int         outlierSign{0};

        bool        playing{false};
        bool        anchored{false};       // START/SPP has defined beat 0
        int64_t     isPlayingAtMicros{0};
        int64_t     lastFedMicros{0};      // last pulse/transport feed
        int64_t     wentStaleMicros{0};
        double      lastNotifiedBpm{0.0};  // bpm at the last /clock/timelines push

        double bpm()     const { return 60.0e6 / (periodUs * kPpqn); }   // derived read-out
        double curBeat() const { return baseBeat + static_cast<double>(pulseCount) / kPpqn; }
        double tsAtBeat(double beat)  const { return lastTsEngine + (beat - curBeat()) * kPpqn * periodUs; }
        double beatAtTs(double tsEng) const { return curBeat() + (tsEng - lastTsEngine) / (kPpqn * periodUs); }

        void ivClear() { ivCount = 0; ivHead = 0; ivSum = 0.0; ivSumSq = 0.0; }
        void ivAdd(double iv) {
            if (ivCount == kWin) { const double old = ivBuf[ivHead]; ivSum -= old; ivSumSq -= old * old; }
            else ++ivCount;
            ivBuf[ivHead] = iv; ivSum += iv; ivSumSq += iv * iv;
            ivHead = (ivHead + 1) % kWin;
            if (ivCount > 0) periodUs = ivSum / ivCount;
        }
        // Update the smoothed tempo from one inter-pulse interval (locked state).
        void feedInterval(double iv) {
            // Absolute plausibility band: outside 10..400 BPM the interval is a
            // delivery artefact (queue-flush burst / stall gap), not a tempo
            // observation — it must never seed or reset the window.
            static constexpr double kMinIvUs = 60.0e6 / (400.0 * kPpqn);
            static constexpr double kMaxIvUs = 60.0e6 / (10.0 * kPpqn);
            if (iv < kMinIvUs || iv > kMaxIvUs) return;
            const double mean = ivCount > 0 ? ivSum / ivCount : iv;
            double var = ivCount > 1 ? (ivSumSq / ivCount - mean * mean) : 0.0;
            if (var < 0.0) var = 0.0;
            const double sdFloor = 0.005 * mean;               // clean-clock SD floor (~0.5%)
            if (ivCount >= kMinForOutlier
                && std::fabs(iv - mean) > kOutlierSd * std::max(std::sqrt(var), sdFloor)) {
                const int sign = iv > mean ? 1 : -1;           // a possible step (or glitch)
                if (sign == outlierSign) ++outlierRun; else { outlierSign = sign; outlierRun = 1; }
                if (outlierRun >= kStepConfirm) { ivClear(); ivAdd(iv); outlierRun = 0; } // confirmed → reset
                // else: a lone outlier (dropout/bounce) — keep the old tempo
            } else {
                ivAdd(iv);
                outlierRun = 0;
            }
        }
    };

    void notifyTimelinesChanged() { if (mTimelinesChangedCb) mTimelinesChangedCb(); }

    // The midi slot for `normalized` (1..K), or 0 if none. Caller holds mMtx.
    int  midiSlotForPortLocked(const char* normalized) const;
    void recomputePrimaryLocked();

    // The active midi slot for id (1..K), or nullptr if out of range / inactive.
    // Caller holds mMtx. (id 0 = Link is handled by callers, not here.)
    MidiTimeline* activeSlotLocked(int id) {
        if (id < 1 || id > SC_MAX_TIMELINES) return nullptr;
        auto& t = mMidi[id - 1];
        return t.active ? &t : nullptr;
    }
    const MidiTimeline* activeSlotLocked(int id) const {
        if (id < 1 || id > SC_MAX_TIMELINES) return nullptr;
        const auto& t = mMidi[id - 1];
        return t.active ? &t : nullptr;
    }

    SuperClock&                                 mClock;
    mutable std::mutex                          mMtx;
    std::array<MidiTimeline, SC_MAX_TIMELINES>  mMidi{};
    int                                         mPrimaryMidiSlot{-1};  // 1..K, or -1
    std::function<void()>                       mTimelinesChangedCb;
};

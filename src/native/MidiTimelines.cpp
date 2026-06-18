/*
 * MidiTimelines.cpp — MIDI-clock follower-timeline registry.
 *
 * Off-RT registry of midi:<port> timelines. mMtx serialises the MIDI subsystem
 * feed, OSC reads, and the staleness sweep. Beat math is in the Link-clock
 * micros domain (via mClock.linkClockMicros) so /clock/midi:<port>/rpc answers
 * match the Link timeline. The id==0 Link reads route to mClock.
 */
#include "native/MidiTimelines.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

namespace {
constexpr int64_t kMidiStaleMicros = 1'500'000;    // mark stale (clock gone) after 1.5 s gap
// Stale timelines are NOT freed: they keep free-running at their last-known tempo
// so use_bpm :midi[:port] holds that tempo until the clock returns. The slot is
// only reclaimed (LRU) when a new port needs one and none are free.
constexpr double  kFallbackBpm     = 60.0;          // never-seen timeline: free-run default
}  // namespace

int MidiTimelines::midiSlotForPortLocked(const char* normalized) const {
    if (!normalized) return 0;
    for (int i = 1; i <= SC_MAX_TIMELINES; ++i) {
        const auto& t = mMidi[i - 1];
        if (t.active && t.normalized == normalized) return i;
    }
    return 0;
}

void MidiTimelines::recomputePrimaryLocked() {
    // Primary = lowest-slot active non-stale timeline; else lowest active; else none.
    int firstActive = -1;
    for (int i = 1; i <= SC_MAX_TIMELINES; ++i) {
        const auto& t = mMidi[i - 1];
        if (!t.active) continue;
        if (firstActive < 0) firstActive = i;
        if (!t.stale) { mPrimaryMidiSlot = i; return; }
    }
    mPrimaryMidiSlot = firstActive;
}

int MidiTimelines::claimMidiTimeline(const char* normalized, const char* raw) {
    if (!normalized || !*normalized) return -1;
    bool changed = false;
    int slot;
    {
        std::lock_guard<std::mutex> lk(mMtx);
        const int64_t now = mClock.linkClockMicros();
        slot = midiSlotForPortLocked(normalized);
        if (slot == 0) {
            // Prefer a free slot; else evict the one stale (free-running) longest.
            int64_t oldestAt = INT64_MAX;
            for (int i = 1; i <= SC_MAX_TIMELINES; ++i) {
                auto& t = mMidi[i - 1];
                if (!t.active) { slot = i; break; }
                if (t.stale && t.wentStaleMicros < oldestAt) { oldestAt = t.wentStaleMicros; slot = i; }
            }
            if (slot == 0) return -1;  // registry full of live clocks
            auto& t = mMidi[slot - 1];
            t = MidiTimeline{};
            t.active = true;
            t.normalized = normalized;
            t.raw = (raw && *raw) ? raw : normalized;
            t.lastTsEngine = static_cast<double>(now);   // beat==baseBeat until first pulse
            t.lastFedMicros = now;
            recomputePrimaryLocked();
            changed = true;
        } else if (mMidi[slot - 1].stale) {
            // The port returned: continue the beat from where the free-run reached,
            // then re-lock to the live clock on the next pulse. `anchored` is left
            // intact — the grid origin still traces to the original START/SPP, just
            // extrapolated across the gap; a returning port re-sends START if it
            // wants to redefine beat 0.
            auto& t = mMidi[slot - 1];
            t.baseBeat = t.beatAtTs(static_cast<double>(now));
            t.pulseCount = 0; t.pulses = 0; t.outlierRun = 0; t.ivClear();
            t.tsOriginSet = false; t.lastTsEngine = static_cast<double>(now);
            t.stale = false;
            t.lastFedMicros = now;
            recomputePrimaryLocked();
            changed = true;
        }
    }
    if (changed) notifyTimelinesChanged();
    return slot;
}

void MidiTimelines::freeMidiTimeline(int id) {
    if (id < 1 || id > SC_MAX_TIMELINES) return;
    bool changed = false;
    {
        std::lock_guard<std::mutex> lk(mMtx);
        if (mMidi[id - 1].active) {
            mMidi[id - 1] = MidiTimeline{};
            recomputePrimaryLocked();
            changed = true;
        }
    }
    if (changed) notifyTimelinesChanged();
}

int MidiTimelines::resolveTimeline(const char* name) const {
    if (!name || !*name || std::strcmp(name, "link") == 0) return 0;
    if (std::strcmp(name, "midi") == 0) {
        std::lock_guard<std::mutex> lk(mMtx);
        return mPrimaryMidiSlot;                 // -1 if none active
    }
    if (std::strncmp(name, "midi:", 5) == 0) {
        std::lock_guard<std::mutex> lk(mMtx);
        const int s = midiSlotForPortLocked(name + 5);
        return s ? s : -1;                       // -1 = unclaimed port (placeholder)
    }
    return -1;
}

// Write-path resolver: like resolveTimeline, but a "midi:<port>" name claims the
// slot if unseen (the registry owns the midi: grammar + claim-on-write, so the
// OSC layer doesn't special-case it). Bare "midi" can't claim (no port).
int MidiTimelines::resolveOrClaimTimeline(const char* name) {
    if (std::strncmp(name ? name : "", "midi:", 5) == 0)
        return claimMidiTimeline(name + 5, name + 5);
    return resolveTimeline(name);
}

// Manual tempo set (OSC /clock/midi:<port>/tempo/set + the unfed placeholder).
// No real pulses, so set the tempo directly and re-anchor to keep the current
// beat continuous. A live clock's pulses (midiTimelinePulse) take over.
void MidiTimelines::setMidiTimelineTempo(int id, double bpm) {
    if (!(bpm >= 1.0)) bpm = 1.0;               // also rejects NaN
    bool changed = false;
    {
        std::lock_guard<std::mutex> lk(mMtx);
        auto* tp = activeSlotLocked(id);
        if (!tp) return;
        auto& t = *tp;
        const int64_t now = mClock.linkClockMicros();
        const double beatNow = t.beatAtTs(static_cast<double>(now));
        const double manualPeriod = 60.0e6 / (bpm * MidiTimeline::kPpqn);
        t.ivClear(); t.ivAdd(manualPeriod);      // seed the window to the manual tempo
        // Re-base so curBeat() maps to beatNow at anchor=now.
        t.baseBeat = beatNow - static_cast<double>(t.pulseCount) / MidiTimeline::kPpqn;
        t.lastTsEngine = static_cast<double>(now);
        t.pulses = 2;                            // locked on the manual tempo; pulses smooth from here
        t.outlierRun = 0;
        t.lastFedMicros = now;
        if (t.stale) { t.stale = false; changed = true; }
        if (std::abs(bpm - t.lastNotifiedBpm) >= 1.0) {
            t.lastNotifiedBpm = bpm;
            changed = true;
        }
    }
    if (changed) notifyTimelinesChanged();
}

// Live clock feed: one 0xF8 pulse at OS timestamp `tsUs`. The beat is the exact
// pulse count pinned to this pulse; the window-mean tempo is used only to
// extrapolate between pulses.
void MidiTimelines::midiTimelinePulse(int id, uint64_t tsUs) {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lk(mMtx);
        auto* tp = activeSlotLocked(id);
        if (!tp) return;
        auto& t = *tp;
        const int64_t now = mClock.linkClockMicros();
        // Capture the pulse-clock -> engine-clock offset once (they're the same
        // mach/monotonic clock on macOS, so this is ~the first dispatch latency).
        if (!t.tsOriginSet) { t.tsToEngineUs = now - static_cast<int64_t>(tsUs); t.tsOriginSet = true; }
        const double tsEng = static_cast<double>(static_cast<int64_t>(tsUs) + t.tsToEngineUs);

        if (t.pulses == 0) {
            // First pulse after a transport reset IS the downbeat: per the MIDI
            // spec the first 0xF8 following Start/Continue marks beat 0 (or the
            // SPP position) — anchor there, don't count past it.
            t.lastTsEngine = tsEng;
            t.pulses = 1;
        } else {
            const double iv = tsEng - t.lastTsEngine;
            // Bunched delivery (a sender/driver stall flushing queued ticks in a
            // burst) — the ticks are real, each 0xF8 is 1/24 beat of position,
            // but the interval is arrival jitter, not tempo: always advance the
            // beat, only feed plausibly-timed intervals to the tempo window.
            const bool bunched = t.pulses >= 2 && iv < 0.25 * t.periodUs;
            if (!bunched) {
                t.feedInterval(iv);              // seeds the window when empty, else smooths/resets
                t.pulses = 2;
            }
            t.lastTsEngine = tsEng;
            t.pulseCount += 1;
        }
        t.lastFedMicros = now;
        if (t.stale) { t.stale = false; changed = true; }
        if (std::abs(t.bpm() - t.lastNotifiedBpm) >= 1.0) {
            t.lastNotifiedBpm = t.bpm();
            changed = true;
        }
    }
    if (changed) notifyTimelinesChanged();
}

void MidiTimelines::setMidiTimelineTransport(int id, int kind, double beat) {
    std::lock_guard<std::mutex> lk(mMtx);
    auto* tp = activeSlotLocked(id);
    if (!tp) return;
    auto& t = *tp;
    const int64_t now = mClock.linkClockMicros();
    t.lastFedMicros = now;
    switch (kind) {
        case 0:  // START — reset the pulse counter; next 0xF8 anchors the downbeat (beat 0)
            t.baseBeat = 0.0; t.pulseCount = 0;
            t.pulses = 0; t.outlierRun = 0; t.ivClear(); t.tsOriginSet = false; t.lastTsEngine = static_cast<double>(now);
            t.playing = true;  t.isPlayingAtMicros = now;
            t.anchored = true;
            break;
        case 3:  // POSITION (SPP) — re-base the pulse counter at `beat`
            t.baseBeat = beat; t.pulseCount = 0;
            t.pulses = 0; t.outlierRun = 0; t.ivClear(); t.tsOriginSet = false; t.lastTsEngine = static_cast<double>(now);
            t.anchored = true;
            break;
        case 1:  // CONTINUE
            t.playing = true;  t.isPlayingAtMicros = now;
            break;
        case 2:  // STOP
            t.playing = false; t.isPlayingAtMicros = now;
            break;
        default: break;
    }
}

void MidiTimelines::tickMidiStaleness() {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lk(mMtx);
        const int64_t now = mClock.linkClockMicros();
        for (int i = 1; i <= SC_MAX_TIMELINES; ++i) {
            auto& t = mMidi[i - 1];
            if (!t.active) continue;
            if (!t.stale && (now - t.lastFedMicros) > kMidiStaleMicros) {
                t.stale = true;                 // clock gone; keep free-running at last tempo
                t.wentStaleMicros = now;        // for LRU eviction order
                changed = true;
            }
        }
        if (changed) recomputePrimaryLocked();
    }
    if (changed) notifyTimelinesChanged();
}

double MidiTimelines::timelineBpm(int id) const {
    if (id == 0) return mClock.getBpm();
    std::lock_guard<std::mutex> lk(mMtx);
    auto* t = activeSlotLocked(id);
    return t ? t->bpm() : kFallbackBpm;                  // never-seen timeline fallback
}

bool MidiTimelines::timelineIsPlaying(int id) const {
    if (id == 0) return mClock.isPlaying();
    std::lock_guard<std::mutex> lk(mMtx);
    auto* t = activeSlotLocked(id);
    return t && t->playing;
}

bool MidiTimelines::timelineIsAnchored(int id) const {
    if (id == 0) return true;
    std::lock_guard<std::mutex> lk(mMtx);
    auto* t = activeSlotLocked(id);
    return t && t->anchored;
}

int64_t MidiTimelines::timelineTimeForIsPlayingMicros(int id) const {
    if (id == 0) return mClock.timeForIsPlayingMicros();
    std::lock_guard<std::mutex> lk(mMtx);
    auto* t = activeSlotLocked(id);
    return t ? t->isPlayingAtMicros : 0;
}

double MidiTimelines::timelineBeatAtLinkTime(int id, int64_t timeMicros, double quantum) const {
    if (id == 0) return mClock.beatAtLinkTime(timeMicros, quantum);
    std::lock_guard<std::mutex> lk(mMtx);
    auto* t = activeSlotLocked(id);
    if (!t || t->periodUs <= 0.0)
        return static_cast<double>(timeMicros) * 1e-6 * kFallbackBpm / 60.0;  // free-run fallback
    return t->beatAtTs(static_cast<double>(timeMicros));
}

double MidiTimelines::timelinePhaseAtLinkTime(int id, int64_t timeMicros, double quantum) const {
    const double beat = timelineBeatAtLinkTime(id, timeMicros, quantum);
    if (quantum <= 0.0) return 0.0;
    double phase = std::fmod(beat, quantum);
    if (phase < 0.0) phase += quantum;
    return phase;
}

int64_t MidiTimelines::timelineTimeAtBeatLinkMicros(int id, double beat, double quantum) const {
    if (id == 0) return mClock.timeAtBeatLinkMicros(beat, quantum);
    std::lock_guard<std::mutex> lk(mMtx);
    auto* t = activeSlotLocked(id);
    if (!t || t->periodUs <= 0.0)
        return static_cast<int64_t>(beat * 60.0 / kFallbackBpm * 1e6);        // free-run fallback
    return static_cast<int64_t>(t->tsAtBeat(beat));
}

std::vector<SuperClock::TimelineInfo> MidiTimelines::listTimelines() const {
    std::vector<SuperClock::TimelineInfo> out;
    SuperClock::TimelineInfo link;
    link.name     = "link";
    link.raw      = "link";
    link.bpm      = mClock.getBpm();
    link.clocking = true;       // Link is always live
    out.push_back(std::move(link));

    std::lock_guard<std::mutex> lk(mMtx);
    for (int i = 1; i <= SC_MAX_TIMELINES; ++i) {
        const auto& t = mMidi[i - 1];
        if (!t.active) continue;
        SuperClock::TimelineInfo info;
        info.name     = "midi:" + t.normalized;
        info.raw      = t.raw;
        info.bpm = t.bpm();
        info.clocking = !t.stale;
        info.stale    = t.stale;
        info.primary  = (i == mPrimaryMidiSlot);
        out.push_back(std::move(info));
    }
    return out;
}

void MidiTimelines::setTimelinesChangedCallback(std::function<void()> cb) {
    std::lock_guard<std::mutex> lk(mMtx);
    mTimelinesChangedCb = std::move(cb);
}

/*
 * MidiClockOut.cpp — see MidiClockOut.h.
 */
#include "MidiClockOut.h"

#include "EngineScheduler.h"
#include "SuperClock.h"
#include "shared_memory.h"
#include "osc/OscOutboundPacketStream.h"

#include <utility>

MidiClockOut& get_midi_clock_out() {
    static MidiClockOut instance;
    return instance;
}

std::vector<uint8_t> MidiClockOut::encodeClockTick(const std::string& port) {
    char buf[128];
    osc::OutboundPacketStream s(buf, sizeof(buf));
    s << osc::BeginMessage("/midi/clock/tick") << port.c_str() << osc::EndMessage;
    const auto* p = reinterpret_cast<const uint8_t*>(s.Data());
    return std::vector<uint8_t>(p, p + s.Size());
}

MidiClockOut::Port* MidiClockOut::findPort(const std::string& name) {
    for (auto& p : mPorts)
        if (p.name == name) return &p;
    return nullptr;
}

MidiClockOut::Port& MidiClockOut::portRef(const std::string& name) {
    if (Port* p = findPort(name)) return *p;
    mPorts.emplace_back();
    Port& p = mPorts.back();
    p.name    = name;
    p.tickOsc = encodeClockTick(name);
    return p;
}

void MidiClockOut::removePort(const std::string& name) {
    for (size_t i = 0; i < mPorts.size(); ++i)
        if (mPorts[i].name == name) {
            mPorts.erase(mPorts.begin() + static_cast<long>(i));
            return;
        }
}

// Re-anchor (bpm, origin) at nowNtp so the port's current beat is preserved, then
// (re)start the train. A running port keeps its pulse phase across a tempo change.
void MidiClockOut::setPortTempo(Port& p, double nowNtp, double bpm) {
    if (!(bpm > 0.0)) return;
    const double curBeat = p.gen.running() ? (nowNtp - p.originNtp) * p.bpm / 60.0 : 0.0;
    p.bpm       = bpm;
    p.originNtp = nowNtp - curBeat * 60.0 / bpm;
    if (!p.gen.running()) p.gen.start(curBeat);
}

void MidiClockOut::applyTempoSource(Port& p, Source src, const std::string& timeline,
                                    double nowNtp, double bpm) {
    if (p.source != src) p.gen.stop();   // source switch → fresh grid
    p.source   = src;
    p.timeline = timeline;               // "" for Fixed
    setPortTempo(p, nowNtp, bpm);
}

void MidiClockOut::reset() {
    std::lock_guard<std::mutex> guard(mLock);
    mPorts.clear();
    mPending.clear();
}

void MidiClockOut::onClockOutTempo(SuperClock& clock, const std::string& port, double bpm) {
    if (!(bpm > 0.0)) return;
    const double now = clock.now();
    std::lock_guard<std::mutex> guard(mLock);
    Port& p = portRef(port);
    if (p.source == Source::Fixed && p.gen.running() && p.bpm == bpm) return;  // no-op
    applyTempoSource(p, Source::Fixed, "", now, bpm);
}

void MidiClockOut::onClockOutFollow(SuperClock& clock, const std::string& port,
                                    const std::string& timeline) {
    const int    id  = clock.resolveTimeline(timeline.c_str());
    const double now = clock.now();
    if (id == 0) {                                   // Link timeline — track the SHM mirror live
        const double linkBeat = clock.beatAtTime(now, kQuantum);
        std::lock_guard<std::mutex> guard(mLock);
        Port& p = portRef(port);
        if (p.source == Source::Link && p.gen.running()) return;   // no-op
        p.timeline.clear();
        p.source = Source::Link;
        p.gen.start(linkBeat);                       // align this port's pulses to Link's beat
        return;
    }
    // midi:<handle> follower — snapshot its smoothed tempo (id<0 ⇒ 60 fallback until it clocks).
    const double bpm = clock.timelineBpm(id);
    std::lock_guard<std::mutex> guard(mLock);
    Port& p = portRef(port);
    if (p.source == Source::Timeline && p.timeline == timeline && p.gen.running()) {
        setPortTempo(p, now, bpm);                   // already following → just refresh tempo
        return;
    }
    applyTempoSource(p, Source::Timeline, timeline, now, bpm);
}

void MidiClockOut::onClockOutOff(const std::string& port) {
    std::lock_guard<std::mutex> guard(mLock);
    removePort(port);   // clock-only: no transport byte (Start/Stop is explicit)
}

void MidiClockOut::refreshTimelineFollowers(SuperClock& clock) {
    const double now = clock.now();
    // Snapshot which ports follow which timelines, drop the lock, read each
    // timeline's bpm (takes the off-RT midi mutex), then apply under the lock —
    // never nest mLock and the midi mutex.
    std::vector<std::pair<std::string, std::string>> follows;
    {
        std::lock_guard<std::mutex> guard(mLock);
        for (auto& p : mPorts)
            if (p.source == Source::Timeline) follows.push_back({p.name, p.timeline});
    }
    if (follows.empty()) return;
    std::vector<std::pair<std::string, double>> updates;
    updates.reserve(follows.size());
    for (auto& f : follows)
        updates.push_back({f.first, clock.timelineBpm(clock.resolveTimeline(f.second.c_str()))});
    std::lock_guard<std::mutex> guard(mLock);
    for (auto& u : updates) {
        Port* p = findPort(u.first);
        if (p && p->source == Source::Timeline) setPortTempo(*p, now, u.second);
    }
}

void MidiClockOut::onBeat(SuperClock& clock, const std::string& port, double durationSeconds) {
    const double now = clock.now();
    auto osc = encodeClockTick(port);
    std::lock_guard<std::mutex> guard(mLock);
    for (int64_t i = 0; i < MidiClockGenerator::PPQN; ++i) {
        const double t = now + durationSeconds * static_cast<double>(i)
                                   / static_cast<double>(MidiClockGenerator::PPQN);
        mPending.push_back({t, osc});
    }
}

void MidiClockOut::generate(SuperClock& clock, double nowNtp) {
    if (!mLock.try_lock()) return;   // a command is mid-update — next block catches up
    std::lock_guard<std::mutex> guard(mLock, std::adopt_lock);

    const double horizonNtp = nowNtp + kLookaheadSeconds;

    // 1. Burst one-shots (manual midi_clock_beat) whose time entered the window.
    if (!mPending.empty()) {
        size_t w = 0;
        for (size_t r = 0; r < mPending.size(); ++r) {
            if (mPending[r].atNtp <= horizonNtp) {
                ss_defer_schedule(mPending[r].atNtp, SCHED_TAG_CLOCK, /*origin*/ 0,
                                  mPending[r].osc.data(),
                                  static_cast<uint32_t>(mPending[r].osc.size()));
            } else {
                if (w != r) mPending[w] = std::move(mPending[r]);
                ++w;
            }
        }
        mPending.resize(w);   // shrink only — no allocation on the audio thread
    }

    // 2. Per-port continuous 24-PPQN trains. Each port resolves its own (bpm,
    //    origin): Link reads the RT-safe SuperClockState mirror; Fixed/Timeline
    //    use the port's snapshot. Pulses scheduled at SuperClock time → sample-
    //    locked to scsynth audio.
    if (mPorts.empty()) return;
    double linkBpm = 0.0, linkOrigin = 0.0;
    if (const SuperClockState* s = clock.state()) {
        linkBpm    = supersonic::bitsToDouble(s->bpm.load(std::memory_order_relaxed));
        linkOrigin = supersonic::bitsToDouble(s->beat_origin_ntp.load(std::memory_order_relaxed));
    }
    for (auto& p : mPorts) {
        if (!p.gen.running()) continue;
        const bool   link   = p.source == Source::Link;   // live SHM read vs per-port snapshot
        const double bpm    = link ? linkBpm    : p.bpm;
        const double origin = link ? linkOrigin : p.originNtp;
        if (bpm <= 0.0) continue;
        const double horizonBeat = (horizonNtp - origin) * bpm / 60.0;
        p.gen.collect(horizonBeat, [&](double pulseBeat) {
            const double t = origin + pulseBeat * 60.0 / bpm;
            ss_defer_schedule(t, SCHED_TAG_CLOCK, /*origin*/ 0,
                              p.tickOsc.data(), static_cast<uint32_t>(p.tickOsc.size()));
        });
    }
}

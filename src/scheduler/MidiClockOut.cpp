/*
 * MidiClockOut.cpp — see MidiClockOut.h.
 */
#include "MidiClockOut.h"

#include "EventScheduler.h"
#include "SuperClock.h"
#include "shared_memory.h"
#include "osc/OscOutboundPacketStream.h"

#include <utility>

MidiClockOut& get_midi_clock_out() {
    static MidiClockOut instance;
    return instance;
}

std::vector<uint8_t> MidiClockOut::encodeTransport(const char* addr, const std::string& port) {
    char buf[128];
    osc::OutboundPacketStream s(buf, sizeof(buf));
    s << osc::BeginMessage(addr) << port.c_str() << osc::EndMessage;
    const auto* p = reinterpret_cast<const uint8_t*>(s.Data());
    return std::vector<uint8_t>(p, p + s.Size());
}

std::vector<uint8_t> MidiClockOut::encodeClockTick(const std::string& port) {
    return encodeTransport("/midi/clock/tick", port);
}

MidiClockOut::Port* MidiClockOut::findPort(const std::string& name) {
    for (auto& p : mPorts)
        if (p.name == name) return &p;
    return nullptr;
}

void MidiClockOut::addPort(const std::string& name) {
    if (findPort(name)) return;
    mPorts.push_back({name, encodeClockTick(name)});
}

void MidiClockOut::removePort(const std::string& name) {
    for (size_t i = 0; i < mPorts.size(); ++i)
        if (mPorts[i].name == name) {
            mPorts.erase(mPorts.begin() + static_cast<long>(i));
            return;
        }
}

void MidiClockOut::reset() {
    std::lock_guard<std::mutex> guard(mLock);
    mGen.stop();
    mPorts.clear();
    mPending.clear();
}

void MidiClockOut::onStart(SuperClock& clock, const std::string& port) {
    const double now     = clock.now();
    const double beatNow = clock.beatAtTime(now, kQuantum);
    std::lock_guard<std::mutex> guard(mLock);
    addPort(port);
    if (!mGen.running()) mGen.start(beatNow);   // first port starts the train; others join it
    mPending.push_back({now, encodeTransport("/midi/out/start", port)});
}

void MidiClockOut::onContinue(SuperClock& clock, const std::string& port) {
    const double now     = clock.now();
    const double beatNow = clock.beatAtTime(now, kQuantum);
    std::lock_guard<std::mutex> guard(mLock);
    addPort(port);
    if (!mGen.running()) mGen.start(beatNow);
    mPending.push_back({now, encodeTransport("/midi/out/continue", port)});
}

void MidiClockOut::onStop(SuperClock& clock, const std::string& port) {
    const double now = clock.now();
    std::lock_guard<std::mutex> guard(mLock);
    removePort(port);
    if (mPorts.empty()) mGen.stop();
    mPending.push_back({now, encodeTransport("/midi/out/stop", port)});
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

    // 1. Transport + burst one-shots whose time has entered the window.
    if (!mPending.empty()) {
        size_t w = 0;
        for (size_t r = 0; r < mPending.size(); ++r) {
            if (mPending[r].atNtp <= horizonNtp) {
                ss_defer_schedule(mPending[r].atNtp, EventScheduler::DEST_MIDI,
                                  mPending[r].osc.data(),
                                  static_cast<uint32_t>(mPending[r].osc.size()));
            } else {
                if (w != r) mPending[w] = std::move(mPending[r]);
                ++w;
            }
        }
        mPending.resize(w);   // shrink only — no allocation on the audio thread
    }

    // 2. Continuous 24-PPQN clock, timed via SuperClock so pulses stay sample-
    //    locked to scsynth audio and track tempo / Link. Coherent (bpm, origin)
    //    snapshot from the RT-safe SuperClockState mirror.
    if (mGen.running() && !mPorts.empty()) {
        const SuperClockState* s = clock.state();
        if (!s) return;
        const double bpm    = supersonic::bitsToDouble(s->bpm.load(std::memory_order_relaxed));
        const double origin = supersonic::bitsToDouble(s->beat_origin_ntp.load(std::memory_order_relaxed));
        if (bpm <= 0.0) return;

        const double horizonBeat = (horizonNtp - origin) * bpm / 60.0;
        mGen.collect(horizonBeat, [&](double pulseBeat) {
            const double t = origin + pulseBeat * 60.0 / bpm;
            for (const auto& p : mPorts)
                ss_defer_schedule(t, EventScheduler::DEST_MIDI,
                                  p.tickOsc.data(),
                                  static_cast<uint32_t>(p.tickOsc.size()));
        });
    }
}

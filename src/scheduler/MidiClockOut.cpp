/*
 * MidiClockOut.cpp — see MidiClockOut.h.
 */
#include "MidiClockOut.h"

#include "EngineScheduler.h"
#include "SuperClock.h"
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

void MidiClockOut::reset() {
    std::lock_guard<std::mutex> guard(mLock);
    mPending.clear();
}

void MidiClockOut::onBeat(SuperClock& clock, const std::string& port, double durationSeconds) {
    const double now = clock.now();
    auto osc = encodeClockTick(port);
    std::lock_guard<std::mutex> guard(mLock);
    for (int64_t i = 0; i < kPulsesPerBeat; ++i) {
        const double t = now + durationSeconds * static_cast<double>(i)
                                   / static_cast<double>(kPulsesPerBeat);
        mPending.push_back({t, osc});
    }
}

void MidiClockOut::generate(double nowNtp) {
    if (!mLock.try_lock()) return;   // a command is mid-update — next block catches up
    std::lock_guard<std::mutex> guard(mLock, std::adopt_lock);
    if (mPending.empty()) return;

    // Burst one-shots (manual midi_clock_beat) whose time entered the window.
    const double horizonNtp = nowNtp + kLookaheadSeconds;
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

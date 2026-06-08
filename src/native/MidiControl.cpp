/*
 * MidiControl.cpp — see MidiControl.h. The Rust subsystem runs midir on its own
 * threads; its callbacks may fire off the audio thread, so everything they touch
 * (the egress ring, SuperClock setters) is already thread-safe.
 */
#include "MidiControl.h"

#include "OscEgress.h"
#include "src/SuperClock.h"
#include "ss_midi.h"
#include "scheduler/MidiClockOut.h"
#include "osc/OscReceivedElements.h"

#include <cstring>
#include <string>

namespace {
// Quantum only affects phase wrapping; the generated/derived clock uses the
// absolute beat, so any fixed value works. Match the engine's default bar.
constexpr double kQuantum = 4.0;
} // namespace

void MidiControl::init(OscEgress* egress, SuperClock* clock) {
    mEgress = egress;
    mClock  = clock;
    if (!mMidi) {
        mMidi = ss_midi_create(this, &MidiControl::emitCb, &MidiControl::tempoCb,
                               &MidiControl::transportCb);
    }
}

void MidiControl::shutdown() {
    if (mMidi) {
        ss_midi_destroy(mMidi);
        mMidi = nullptr;
    }
}

bool MidiControl::handleMidiCommand(const uint8_t* data, uint32_t size) {
    if (size < 8 || std::memcmp(data, "/midi/", 6) != 0) return false;

    // Subscription drives the egress audience (owned by the transport). The
    // address is the leading, NUL-terminated OSC string.
    const char* addr = reinterpret_cast<const char*>(data);
    if (std::strcmp(addr, "/midi/notify/subscribe") == 0) {
        if (mEgress && mEgress->subscribeCallerToMidiNotify() && mMidi)
            ss_midi_emit_ports(mMidi);   // ports snapshot to the new subscriber
        return true;
    }
    if (std::strcmp(addr, "/midi/notify/unsubscribe") == 0) {
        if (mEgress) mEgress->unsubscribeCallerFromMidiNotify();
        return true;
    }

    // Clock-OUT generation is owned by the engine's SuperClock-timed MidiClockOut
    // (start/stop/continue/beat). /midi/clock/tick (one immediate pulse — also
    // how MidiClockOut's generated pulses come back through the dispatch path)
    // and /midi/clock/sync (clock-in tempo source) fall through to the Rust
    // subsystem below.
    if (mClock && std::strncmp(addr, "/midi/clock/", 12) == 0) {
        const char* verb = addr + 12;
        const bool start = std::strcmp(verb, "start") == 0;
        const bool cont  = std::strcmp(verb, "continue") == 0;
        const bool stop  = std::strcmp(verb, "stop") == 0;
        const bool beat  = std::strcmp(verb, "beat") == 0;
        if (start || cont || stop || beat) {
            std::string port = "*";
            double durMs = 0.0;
            try {
                osc::ReceivedMessage msg(osc::ReceivedPacket(
                    reinterpret_cast<const char*>(data),
                    static_cast<osc::osc_bundle_element_size_t>(size)));
                auto it = msg.ArgumentsBegin();
                if (it != msg.ArgumentsEnd() && it->IsString()) { port = it->AsStringUnchecked(); ++it; }
                if (beat && it != msg.ArgumentsEnd()) {
                    if (it->IsFloat())       durMs = it->AsFloatUnchecked();
                    else if (it->IsDouble()) durMs = it->AsDoubleUnchecked();
                    else if (it->IsInt32())  durMs = it->AsInt32Unchecked();
                }
            } catch (...) { return true; }  // malformed — swallow
            if (start)      get_midi_clock_out().onStart(*mClock, port);
            else if (cont)  get_midi_clock_out().onContinue(*mClock, port);
            else if (stop)  get_midi_clock_out().onStop(*mClock, port);
            else            get_midi_clock_out().onBeat(*mClock, port, durMs / 1000.0);
            return true;
        }
    }

    if (mMidi) ss_midi_handle_osc(mMidi, data, size);
    return true;
}

void MidiControl::dispatchOsc(const uint8_t* osc, uint32_t len) {
    if (mMidi) ss_midi_handle_osc(mMidi, osc, len);
}

void MidiControl::refreshDevices() {
    if (mMidi) ss_midi_refresh(mMidi);
}

void MidiControl::emitCb(void* ctx, int32_t kind, const uint8_t* osc, uint32_t len) {
    auto* self = static_cast<MidiControl*>(ctx);
    if (!self->mEgress) return;
    if (kind == SS_MIDI_EMIT_REPLY)
        self->mEgress->reply(osc, len);
    else
        self->mEgress->broadcastMidiNotify(osc, len);
}

void MidiControl::tempoCb(void* ctx, double bpm) {
    auto* self = static_cast<MidiControl*>(ctx);
    if (self->mClock) self->mClock->setBpm(bpm, self->mClock->now());
}

void MidiControl::transportCb(void* ctx, int32_t kind, double beat) {
    auto* self = static_cast<MidiControl*>(ctx);
    if (!self->mClock) return;
    const double now = self->mClock->now();
    switch (kind) {
    case SS_MIDI_TRANSPORT_START:
        self->mClock->forceBeatAtTime(0.0, now, kQuantum);
        self->mClock->setIsPlaying(true, now);
        break;
    case SS_MIDI_TRANSPORT_CONTINUE:
        self->mClock->setIsPlaying(true, now);
        break;
    case SS_MIDI_TRANSPORT_STOP:
        self->mClock->setIsPlaying(false, now);
        break;
    case SS_MIDI_TRANSPORT_POSITION:
        if (beat >= 0.0) self->mClock->forceBeatAtTime(beat, now, kQuantum);
        break;
    default:
        break;
    }
}


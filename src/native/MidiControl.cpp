/*
 * MidiControl.cpp — see MidiControl.h. The Rust subsystem runs midir on its own
 * threads; its callbacks may fire off the audio thread, so everything they touch
 * (the egress ring, SuperClock setters) is already thread-safe.
 */
#include "MidiControl.h"

#include "OscEgress.h"
#include "src/SuperClock.h"
#include "src/timeline_osc.h"
#include "ss_midi.h"
#include "scheduler/MidiClockOut.h"
#include "osc/OscReceivedElements.h"
#include "osc/OscOutboundPacketStream.h"

#include <cstdio>
#include <cstring>
#include <string>

void MidiControl::init(OscEgress* egress, SuperClock* clock) {
    mEgress = egress;
    mClock  = clock;
    // Push /clock/timelines whenever the timeline set changes (add/remove/
    // stale/primary), and re-snapshot any clock-OUT ports following a midi
    // timeline. Fires off the RT thread (MIDI feed / staleness worker).
    if (mClock)
        mClock->setTimelinesChangedCallback([this]() {
            broadcastTimelines();
            get_midi_clock_out().refreshTimelineFollowers(*mClock);
        });
    if (!mMidi) {
        mMidi = ss_midi_create(this, &MidiControl::emitCb, &MidiControl::clockCb,
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

    if (handleClockOutVerb(data, size)) return true;

    if (mMidi) ss_midi_handle_osc(mMidi, data, size);
    return true;
}

// Clock-OUT generation is owned by the engine's SuperClock-timed MidiClockOut:
//   out/bpm <port> <bpm>     — continuous clock at a fixed tempo
//   out/follow <port> <tl>   — continuous clock following "link" | "midi:<handle>"
//   out/off <port>           — stop the port's clock
//   beat <port> <durMs>      — legacy one-beat burst (manual midi_clock_beat)
// /midi/clock/tick (one immediate pulse — also how MidiClockOut's generated
// pulses come back through the dispatch path) and /midi/clock/sync (clock-in
// tempo source) are not clock-OUT verbs and fall through to the Rust subsystem.
bool MidiControl::handleClockOutVerb(const uint8_t* data, uint32_t size) {
    if (!mClock || size < 16) return false;
    const char* addr = reinterpret_cast<const char*>(data);
    if (std::strncmp(addr, "/midi/clock/", 12) != 0) return false;
    const char* verb = addr + 12;
    const bool outBpm    = std::strcmp(verb, "out/bpm") == 0;
    const bool outFollow = std::strcmp(verb, "out/follow") == 0;
    const bool outOff    = std::strcmp(verb, "out/off") == 0;
    const bool beat      = std::strcmp(verb, "beat") == 0;
    if (!(outBpm || outFollow || outOff || beat)) return false;

    std::string port = "*", timeline;
    double num = 0.0;
    try {
        osc::ReceivedMessage msg(osc::ReceivedPacket(
            reinterpret_cast<const char*>(data),
            static_cast<osc::osc_bundle_element_size_t>(size)));
        auto it = msg.ArgumentsBegin();
        if (it != msg.ArgumentsEnd() && it->IsString()) { port = it->AsStringUnchecked(); ++it; }
        if (it != msg.ArgumentsEnd()) {
            if (outFollow && it->IsString()) timeline = it->AsStringUnchecked();
            else if (it->IsFloat())          num = it->AsFloatUnchecked();
            else if (it->IsDouble())         num = it->AsDoubleUnchecked();
            else if (it->IsInt32())          num = it->AsInt32Unchecked();
        }
    } catch (...) { return true; }  // malformed clock-out verb — swallow
    if (outBpm)         get_midi_clock_out().onClockOutTempo(*mClock, port, num);
    else if (outFollow) get_midi_clock_out().onClockOutFollow(*mClock, port, timeline);
    else if (outOff)    get_midi_clock_out().onClockOutOff(port);
    else                get_midi_clock_out().onBeat(*mClock, port, num / 1000.0);
    return true;
}

void MidiControl::dispatchOsc(const uint8_t* osc, uint32_t len) {
    // Deferred events carry the same /midi/* surface as immediate commands —
    // Sonic Pi's midi_clock_beat arrives as /midi/clock/beat wrapped in a
    // timetagged /midi/at — so clock-OUT verbs must be intercepted here exactly
    // as in handleMidiCommand. MidiClockOut's command handlers record intent
    // under a mutex, so this is safe on the MIDI dispatch thread.
    if (handleClockOutVerb(osc, len)) return;
    if (mMidi) ss_midi_handle_osc(mMidi, osc, len);
}

void MidiControl::refreshDevices() {
    if (mMidi) ss_midi_refresh(mMidi);
}

// Hotplug logging. /midi/ports broadcasts fire only on a device change (plus
// one snapshot per new subscriber), so this is flood-safe. Per-event traffic
// (/midi/in/*, /midi/out/*, clock pulses) is deliberately not logged.
// Payload: <nIn:i> [name:s enabled:i]* <nOut:i> [name:s enabled:i]*
static void logMidiPortsChange(const uint8_t* data, uint32_t len) {
    if (std::strcmp(reinterpret_cast<const char*>(data), "/midi/ports") != 0) return;
    std::string ins, outs;
    try {
        osc::ReceivedMessage msg(osc::ReceivedPacket(
            reinterpret_cast<const char*>(data),
            static_cast<osc::osc_bundle_element_size_t>(len)));
        auto it = msg.ArgumentsBegin();
        auto readList = [&](std::string& names) {
            if (it == msg.ArgumentsEnd() || !it->IsInt32()) return;
            const int n = it->AsInt32Unchecked(); ++it;
            for (int i = 0; i < n && it != msg.ArgumentsEnd(); ++i) {
                if (!it->IsString()) return;
                if (!names.empty()) names += ", ";
                names += it->AsStringUnchecked(); ++it;   // name
                if (it != msg.ArgumentsEnd()) ++it;       // enabled flag
            }
        };
        readList(ins);
        readList(outs);
    } catch (...) { return; }
    fprintf(stderr, "[midi] ports: in=[%s] out=[%s]\n", ins.c_str(), outs.c_str());
    fflush(stderr);
}

void MidiControl::emitCb(void* ctx, int32_t kind, const uint8_t* osc, uint32_t len) {
    auto* self = static_cast<MidiControl*>(ctx);
    if (!self->mEgress) return;
    if (kind == SS_MIDI_EMIT_REPLY) {
        self->mEgress->reply(osc, len);
    } else {
        logMidiPortsChange(osc, len);
        self->mEgress->broadcastMidiNotify(osc, len);
    }
}

// One 0xF8 pulse feeds the port's own midi timeline (claimed by normalised
// handle, labelled with the raw OS name) — NOT the Link timeline. claim is
// idempotent, so per-pulse calls just resolve the existing slot; the engine
// anchors the beat on the pulse count.
void MidiControl::clockCb(void* ctx, const uint8_t* norm, uint32_t normLen,
                          const uint8_t* raw, uint32_t rawLen, uint64_t tsUs) {
    auto* self = static_cast<MidiControl*>(ctx);
    if (!self->mClock) return;
    const std::string n(reinterpret_cast<const char*>(norm), normLen);
    const std::string r(reinterpret_cast<const char*>(raw),  rawLen);
    const int id = self->mClock->claimMidiTimeline(n.c_str(), r.c_str());
    if (id > 0) self->mClock->midiTimelinePulse(id, tsUs);
}

void MidiControl::transportCb(void* ctx, const uint8_t* norm, uint32_t normLen,
                              const uint8_t* raw, uint32_t rawLen, int32_t kind, double beat) {
    auto* self = static_cast<MidiControl*>(ctx);
    if (!self->mClock) return;
    const std::string n(reinterpret_cast<const char*>(norm), normLen);
    const std::string r(reinterpret_cast<const char*>(raw),  rawLen);
    const int id = self->mClock->claimMidiTimeline(n.c_str(), r.c_str());
    if (id > 0) self->mClock->setMidiTimelineTransport(id, kind, beat);
}

void MidiControl::broadcastTimelines() {
    if (!mEgress || !mClock) return;
    const auto tls = mClock->listTimelines();
    char buf[2048];
    osc::OutboundPacketStream s(buf, sizeof(buf));
    s << osc::BeginMessage("/clock/timelines");
    appendTimelineRows(s, tls);
    s << osc::EndMessage;
    mEgress->broadcastLinkNotify(reinterpret_cast<const uint8_t*>(s.Data()),
                                 static_cast<uint32_t>(s.Size()));
}


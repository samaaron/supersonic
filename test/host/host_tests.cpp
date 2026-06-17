/*
    SuperSonic
    Copyright (c) 2025 Sam Aaron

    Unit tests for the standalone host's pure logic: the OSC reader and the
    HostScheduler ingest/tick/outbound path. No sockets, no threads, no engine —
    a fake clock (explicit `now`), the scheduler framing due events into a ring,
    and a synchronous drain into capturing delivery callbacks. Standalone assert
    harness; builds independently of the engine.
*/

#include "host/host_scheduler.h"
#include "host/host_outbound.h"
#include "host/osc_reader.h"
#include "OscIngress.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using ss_host::HostScheduler;
using ss_host::OscReader;

static int g_failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

// ── tiny OSC encoder for building test vectors ───────────────────────────────
namespace {
struct Osc {
    std::vector<uint8_t> b;
    void pad() { while (b.size() % 4) b.push_back(0); }
    void str(const char* s) { b.insert(b.end(), s, s + std::strlen(s) + 1); pad(); }
    void i32(int32_t v) { for (int i = 3; i >= 0; --i) b.push_back(uint8_t(v >> (i * 8))); }
    void i64(int64_t v) { for (int i = 7; i >= 0; --i) b.push_back(uint8_t(v >> (i * 8))); }
    void f32(float v) { uint32_t r; std::memcpy(&r, &v, 4); i32(int32_t(r)); }
    void f64(double v) { uint64_t r; std::memcpy(&r, &v, 8); i64(int64_t(r)); }
    void blob(const uint8_t* d, uint32_t n) { i32(int32_t(n)); b.insert(b.end(), d, d + n); pad(); }
};

// /osc/send <s host> <i port> <b inner> — the inner message a /schedule carries.
std::vector<uint8_t> osc_send(const char* host, int32_t port, const std::vector<uint8_t>& inner) {
    Osc o; o.str("/osc/send"); o.str(",sib");
    o.str(host); o.i32(port); o.blob(inner.data(), uint32_t(inner.size()));
    return o.b;
}
// /schedule <i64 when> <b inner-message>
std::vector<uint8_t> schedule(int64_t when, const std::vector<uint8_t>& inner) {
    Osc o; o.str("/schedule"); o.str(",hb");
    o.i64(when); o.blob(inner.data(), uint32_t(inner.size()));
    return o.b;
}
// Convenience wrappers matching how Sonic Pi schedules OSC-out / MIDI-out: a
// /schedule carrying a /osc/send, or a /schedule carrying a /midi/* (raw) blob.
std::vector<uint8_t> osc_at(int64_t when, const char* host, int32_t port,
                            const std::vector<uint8_t>& inner) {
    return schedule(when, osc_send(host, port, inner));
}
std::vector<uint8_t> midi_at(int64_t when, const std::vector<uint8_t>& inner,
                             const char* /*tag*/ = nullptr) {
    return schedule(when, inner);
}
std::vector<uint8_t> sched_flush(const char* tag) {
    Osc o; o.str("/sched/flush"); o.str(",s"); o.str(tag); return o.b;
}
// A /midi/* OSC message (what Sonic Pi schedules; the dispatcher routes it to the
// default MIDI leaf, and ss_midi_handle_osc parses it). Raw MIDI bytes can't ride
// the OscIngress — they're not a valid OSC address.
std::vector<uint8_t> midi_msg(const std::vector<uint8_t>& raw) {
    Osc o; o.str("/midi/raw"); o.str(",b"); o.blob(raw.data(), uint32_t(raw.size()));
    return o.b;
}

// Capture of one OSC send.
struct OscSend { std::string host; int port; std::vector<uint8_t> inner; };
}  // namespace

int main() {
    // 1) OscReader reads /osc/send fields in order.
    {
        std::vector<uint8_t> inner = {0xDE, 0xAD, 0xBE, 0xEF, 0x01};
        auto msg = osc_send("127.0.0.1", 4560, inner);
        OscReader r(msg.data(), msg.size());
        CHECK(r.ok());
        CHECK(std::strcmp(r.address(), "/osc/send") == 0);
        const char* host; CHECK(r.readString(host) && std::strcmp(host, "127.0.0.1") == 0);
        int32_t port; CHECK(r.readInt32(port) && port == 4560);
        const uint8_t* bl; uint32_t bn;
        CHECK(r.readBlob(bl, bn) && bn == inner.size() && std::memcmp(bl, inner.data(), bn) == 0);
    }

    // 2) Malformed packet: ok() false, no read succeeds, no crash.
    {
        uint8_t junk[6] = {0x2f, 0x78, 0x01, 0x02, 0x03, 0x04};  // "/x" then garbage, no typetag
        OscReader r(junk, sizeof junk);
        CHECK(!r.ok());
    }

    std::vector<OscSend> oscSends;
    std::vector<std::vector<uint8_t>> midiSends;
    ss_host::SendOsc recordOsc = [&](const char* host, int port, const uint8_t* inner, uint32_t len) {
        oscSends.push_back({host, port, std::vector<uint8_t>(inner, inner + len)});
    };
    ss_host::SendMidi recordMidi = [&](const uint8_t* inner, uint32_t len) {
        midiSends.push_back(std::vector<uint8_t>(inner, inner + len));
    };

    // The host's OscIngress with recording backends — fired events route here by
    // address, exactly as in the engine (the synth default simply absent).
    ss_host::HostSenders senders{ recordOsc, recordMidi };
    OscIngress ingress;
    ingress.registerRoute("/osc/send", &ss_host::hostOscSendRoute, &senders);
    ingress.registerRoute("/midi/",    &ss_host::hostMidiRoute,    &senders);
    ingress.setDefault(&ss_host::hostUnroutedRoute, nullptr);
    HostScheduler sched(ingress);

    // tick() now dispatches inline through the ingress; pump() is a no-op kept so
    // the per-test call sites read unchanged.
    auto pump = []{};

    // 3) /osc/at fires only when due, with correct host/port/inner.
    {
        std::vector<uint8_t> inner = {1, 2, 3, 4};
        auto m = osc_at(1000, "10.0.0.5", 9000, inner);
        sched.ingest(m.data(), m.size());
        sched.tick(999);
        pump();
        CHECK(oscSends.empty());          // not yet due
        CHECK(sched.pending() == 1);
        sched.tick(1000);
        pump();                 // now due
        CHECK(oscSends.size() == 1);
        CHECK(oscSends[0].host == "10.0.0.5");
        CHECK(oscSends[0].port == 9000);
        CHECK(oscSends[0].inner == inner);
        CHECK(sched.pending() == 0);
    }

    // 4) Time ordering: later-time message enqueued first still fires second.
    {
        oscSends.clear();
        auto late  = osc_at(3000, "h", 1, {0xAA});
        auto early = osc_at(2000, "h", 2, {0xBB});
        sched.ingest(late.data(), late.size());
        sched.ingest(early.data(), early.size());
        sched.tick(5000);
        pump();
        CHECK(oscSends.size() == 2);
        CHECK(oscSends[0].port == 2);     // t=2000 first
        CHECK(oscSends[1].port == 1);     // t=3000 second
    }

    // 5) /sched/flush "default" cancels pending default-tag events before they fire.
    {
        oscSends.clear();
        auto m = osc_at(4000, "h", 7, {0x09});
        sched.ingest(m.data(), m.size());
        auto f = sched_flush("default");
        sched.ingest(f.data(), f.size());
        sched.tick(9000);
        pump();
        CHECK(oscSends.empty());          // flushed
        CHECK(sched.pending() == 0);
    }

    // 6) A /schedule-wrapped /midi/* message routes to the MIDI sender when due.
    {
        midiSends.clear();
        auto note = midi_msg({0x90, 0x40, 0x7f});
        auto m = midi_at(6000, note);
        sched.ingest(m.data(), m.size());
        sched.tick(7000);
        pump();
        CHECK(midiSends.size() == 1);
        CHECK(midiSends[0] == note);
    }

    // 7) Out-of-range port is rejected, not wrapped to a bogus port.
    {
        oscSends.clear();
        auto bad  = osc_at(1000, "h", 70000, {0x01});   // > 65535
        auto good = osc_at(1000, "h", 4560,  {0x02});
        sched.ingest(bad.data(), bad.size());
        sched.ingest(good.data(), good.size());
        sched.tick(2000);
        pump();
        CHECK(oscSends.size() == 1);          // only the valid-port event is sent
        CHECK(oscSends[0].port == 4560);
    }

    // 8) A /midi/* message routes to the MIDI leaf via the "/midi/" prefix, never
    //    the /osc/send route — the ingress matches prefixes, not a leading '/'.
    {
        oscSends.clear();
        midiSends.clear();
        auto midi = midi_msg({0x40, 0x7f, 0x10});
        auto m = midi_at(8000, midi);
        sched.ingest(m.data(), m.size());
        sched.tick(9000);
        pump();
        CHECK(oscSends.empty());              // not misrouted to OSC
        CHECK(midiSends.size() == 1);
        CHECK(midiSends[0] == midi);
    }

    // 9) An address with no backend (synth API on the no-synth host) hits the
    //    default reporter — dropped, not misrouted to OSC or MIDI.
    {
        oscSends.clear();
        midiSends.clear();
        Osc s; s.str("/s_new"); s.str(",si"); s.str("beep"); s.i32(1);
        auto m = schedule(8000, s.b);
        sched.ingest(m.data(), m.size());
        sched.tick(9000);
        pump();
        CHECK(oscSends.empty());
        CHECK(midiSends.empty());
    }

    if (g_failures) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("host tests OK\n");
    return 0;
}

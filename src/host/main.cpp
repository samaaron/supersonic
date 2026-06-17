/*
    SuperSonic
    Copyright (c) 2025 Sam Aaron

    Dual-licensed MIT OR GPL-3.0-or-later (see repo LICENSE).

    Standalone scheduler host: a sample-accurate OSC/MIDI event scheduler with no
    synthesis engine. Receives the control vocabulary (/schedule, /sched/flush)
    over UDP, schedules each event on the generic Scheduler, and delivers due
    events via the Rust OSC and MIDI subsystems. A timer thread drives the
    scheduler tick off the wall clock.

    Usage: supersonic-scheduler [control_port=4560] [loopback=1]
*/

#include "host/clock.h"
#include "host/host_scheduler.h"
#include "host/host_outbound.h"
#include "OscIngress.h"
#include "ss_osc.h"
#ifdef SUPERSONIC_WITH_MIDI
#include "ss_midi.h"
#endif

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace {
std::atomic<bool> g_running{true};
void on_signal(int) { g_running.store(false); }

void ingress_cb(void* ctx, int32_t /*kind*/, const uint8_t* osc, uint32_t len) {
    static_cast<ss_host::HostScheduler*>(ctx)->ingest(osc, len);
}
void osc_emit_noop(void*, int32_t, const uint8_t*, uint32_t) {}

#ifdef SUPERSONIC_WITH_MIDI
void midi_emit_noop(void*, int32_t, const uint8_t*, uint32_t) {}
void midi_clock_noop(void*, const uint8_t*, uint32_t, const uint8_t*, uint32_t, uint64_t) {}
void midi_transport_noop(void*, const uint8_t*, uint32_t, const uint8_t*, uint32_t, int32_t, double) {}
#endif
}  // namespace

int main(int argc, char** argv) {
    int control_port = argc > 1 ? std::atoi(argv[1]) : 4560;
    int loopback     = argc > 2 ? std::atoi(argv[2]) : 1;

    SsOsc* osc = ss_osc_create(nullptr, osc_emit_noop);
    if (!osc) {
        std::fprintf(stderr, "supersonic-scheduler: failed to create OSC subsystem\n");
        return 1;
    }

#ifdef SUPERSONIC_WITH_MIDI
    SsMidi* midi = ss_midi_create(nullptr, midi_emit_noop, midi_clock_noop, midi_transport_noop);
#endif

    ss_host::SendOsc sendOsc =
        [osc](const char* host, int port, const uint8_t* inner, uint32_t len) {
            ss_osc_send(osc, reinterpret_cast<const uint8_t*>(host),
                        static_cast<uint32_t>(std::strlen(host)), port, inner, len);
        };
#ifdef SUPERSONIC_WITH_MIDI
    ss_host::SendMidi sendMidi = [midi](const uint8_t* inner, uint32_t len) {
        ss_midi_handle_osc(midi, inner, len);
    };
#else
    ss_host::SendMidi sendMidi = [](const uint8_t*, uint32_t) {};
#endif

    // Fired events route by address through the OscIngress — the engine's
    // dispatcher — with the host's two leaves; no synth is registered, so the
    // default reports any unrouted address (e.g. synth API) rather than swallow
    // it. tick() ingests synchronously on the tick thread (not RT, no NRT handoff).
    ss_host::HostSenders senders{ sendOsc, sendMidi };
    OscIngress ingressRouter;
    ingressRouter.registerRoute("/osc/send", &ss_host::hostOscSendRoute, &senders);
    ingressRouter.registerRoute("/midi/",    &ss_host::hostMidiRoute,    &senders);
    ingressRouter.setDefault(&ss_host::hostUnroutedRoute, nullptr);
    ss_host::HostScheduler sched(ingressRouter);

    SsOscIngress* ingress = ss_osc_ingress_start(&sched, ingress_cb, control_port, loopback);
    if (!ingress) {
        std::fprintf(stderr, "supersonic-scheduler: failed to bind control port %d\n", control_port);
#ifdef SUPERSONIC_WITH_MIDI
        ss_midi_destroy(midi);
#endif
        ss_osc_destroy(osc);
        return 1;
    }

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
#ifdef SUPERSONIC_WITH_MIDI
    const char* caps = "scheduler + osc + midi";
#else
    const char* caps = "scheduler + osc";
#endif
    std::fprintf(stderr, "supersonic-scheduler: %s on control port %d (%s)\n",
                 caps, control_port, loopback ? "loopback" : "all interfaces");

    // Drive the scheduler at ~1 ms granularity off the wall clock; tick()
    // dispatches whatever just came due through the OscIngress inline.
    while (g_running.load()) {
        sched.tick(ss_host::osc_now());
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    ss_osc_ingress_stop(ingress);
#ifdef SUPERSONIC_WITH_MIDI
    ss_midi_destroy(midi);
#endif
    ss_osc_destroy(osc);
    return 0;
}

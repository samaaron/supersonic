/*
    SuperSonic
    Copyright (c) 2025 Sam Aaron

    Dual-licensed MIT OR GPL-3.0-or-later (see repo LICENSE).

    Host outbound backends, registered on the host's OscIngress so a fired event
    routes exactly as it does in the engine — by address, through the same
    dispatcher — with the host's two leaves (OSC send, MIDI send) in place of the
    synth default. "/osc/send <host:s> <port:i> <inner:b>" sends inner to
    host:port; anything else (a "/midi/*" message) is the default → MIDI sender.
*/

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>

#include "osc_reader.h"

namespace ss_host {

// host bytes are NUL-terminated; inner is the OSC payload to send.
using SendOsc  = std::function<void(const char* host, int port,
                                    const uint8_t* inner, uint32_t len)>;
using SendMidi = std::function<void(const uint8_t* inner, uint32_t len)>;

// The host's two delivery leaves, the routeCtx its OscIngress backends carry.
struct HostSenders { SendOsc osc; SendMidi midi; };

// OscIngress backend: "/osc/send <host> <port> <inner>" → osc leaf.
inline bool hostOscSendRoute(void* routeCtx, const void* /*callCtx*/,
                             const uint8_t* data, std::size_t len) {
    auto* s = static_cast<HostSenders*>(routeCtx);
    OscReader r(data, len);
    if (!r.ok()) return true;
    const char* host; int32_t port; const uint8_t* inner; uint32_t innerLen;
    if (!r.readString(host) || !r.readInt32(port) || !r.readBlob(inner, innerLen))
        return true;
    if (port > 0 && port <= 65535 && innerLen > 0 && s->osc)
        s->osc(host, port, inner, innerLen);
    return true;
}

// OscIngress backend: a "/midi/*" message → MIDI leaf.
inline bool hostMidiRoute(void* routeCtx, const void* /*callCtx*/,
                          const uint8_t* data, std::size_t len) {
    auto* s = static_cast<HostSenders*>(routeCtx);
    if (s->midi) s->midi(data, static_cast<uint32_t>(len));
    return true;
}

// OscIngress default: no backend claims this address (e.g. synth API on the
// no-synth host) — report and drop, mirroring the engine's no-backend log.
inline bool hostUnroutedRoute(void* /*routeCtx*/, const void* /*callCtx*/,
                              const uint8_t* data, std::size_t len) {
    uint32_t a = 0; while (a < len && data[a] != '\0') ++a;
    std::fprintf(stderr, "supersonic-scheduler: no backend for OSC %.*s — dropped\n",
                 static_cast<int>(a), reinterpret_cast<const char*>(data));
    return true;
}

}  // namespace ss_host

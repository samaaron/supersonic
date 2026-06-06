/*
 * osc_debug.h — the one definition of the `/supersonic/debug <text>` OSC packet.
 *
 * Debug log lines ride the egress rings as an ordinary OSC message; the host
 * dispatches the `/supersonic/debug` address to its debug channel. Both writers
 * (the audio thread's emit_debug_osc → OUT ring, and OscEgress::debug → NRT-out
 * ring) build the packet here, and both readers extract the string at the same
 * offset — so the format lives in exactly one place.
 */
#pragma once

#include <cstdint>
#include <cstring>

namespace supersonic {

// Offset of the string argument: address "/supersonic/debug" (17 + NUL = 18,
// padded to 20) + type tag ",s\0\0" (4) = 24.
inline constexpr uint32_t kDebugArgOffset = 24;

// Build "/supersonic/debug <text>" into `pkt` (must hold >= 1024 bytes; `len` is
// clamped to 960 to fit). Returns the packet size in bytes.
inline uint32_t buildDebugOsc(char* pkt, const char* text, uint32_t len) {
    if (len > 960) len = 960;
    uint32_t p = 0;
    auto pad4 = [&]() { while (p & 3u) pkt[p++] = '\0'; };
    static const char kAddr[] = "/supersonic/debug";
    std::memcpy(pkt + p, kAddr, sizeof(kAddr)); p += sizeof(kAddr); pad4();  // incl. NUL
    pkt[p++] = ','; pkt[p++] = 's'; pkt[p++] = '\0'; pad4();                  // type tag ",s"
    std::memcpy(pkt + p, text, len); p += len; pkt[p++] = '\0'; pad4();       // string arg
    return p;
}

}  // namespace supersonic

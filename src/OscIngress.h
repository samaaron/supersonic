/*
 * OscIngress.h — the engine's OSC ingress (owned by the engine, not the transport).
 *
 * A registry of address-prefix -> handler. ingest() classifies a raw OSC packet
 * and routes it to the registered handler; the handler either copies the bytes
 * into a destination ring (audio, tau-inbox, midi-out, …) or handles it inline
 * (control). Bundles and any message claiming no registered prefix go to the
 * default (the audio/scheduler plane).
 *
 * Transports (UDP socket, NIF send_osc, SHM) deliver bytes here and nothing more
 * — they don't know the address-space; the engine does. Adding a subsystem is a
 * single registerRoute() call, never a transport change.
 *
 * Zero-allocation, no_std-friendly, lock-free by construction (the route table is
 * immutable after registration; ingest() is const). Handlers are plain function
 * pointer + context (callable from any thread).
 */
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

class OscIngress {
public:
    // routeCtx is the handler's own state (a ring writer, a subsystem); callCtx
    // is per-ingest()-call context (e.g. the reply address of the message being
    // drained); data/len is borrowed — a handler consumes it synchronously
    // (copy into a ring / handle inline). Returns true iff it owned the packet.
    using Handler = bool (*)(void* routeCtx, void* callCtx, const uint8_t* data, size_t len);

    // The default destination — bundles and unclaimed messages. Optional: the
    // audio-thread drain registers NO default, so unmatched packets fall through
    // to the caller (the inline audio plane) via ingest() returning false.
    void setDefault(Handler h, void* ctx) noexcept { mDefault = { h, ctx }; }

    // Register a control prefix -> handler. Include the trailing delimiter
    // ("/clock/") so it never matches "/linkage". Longest match wins. The prefix
    // is copied, so it need not outlive the call.
    bool registerRoute(const char* prefix, Handler h, void* ctx) noexcept {
        if (prefix == nullptr || mCount >= kMaxRoutes) return false;
        size_t len = 0;
        while (len <= kMaxPrefix && prefix[len] != '\0') ++len;
        if (len == 0 || len > kMaxPrefix) return false;
        Route& r = mRoutes[mCount];
        std::memcpy(r.prefix, prefix, len);
        r.len = len;
        r.dest = { h, ctx };
        ++mCount;
        return true;
    }

    // Classify a raw OSC packet and dispatch it. Never reads past len. Returns
    // true iff a registered route (or the default, if set) consumed it. With no
    // default registered, bundles and unmatched messages return false so the
    // caller handles them (the audio plane). A packet that is not a '/'-led,
    // NUL-terminated OSC address returns false (caller decides).
    bool ingest(const uint8_t* data, size_t len, void* callCtx) const noexcept {
        if (data == nullptr || len < 4) return false;
        if (len >= 8 && std::memcmp(data, "#bundle", 8) == 0) return dispatch(mDefault, callCtx, data, len);
        if (data[0] != '/') return false;
        size_t addr = 0;
        while (addr < len && data[addr] != '\0') ++addr;
        if (addr == len) return false;  // address not NUL-terminated within bounds

        const Dest* best = &mDefault;
        size_t bestLen = 0;
        for (size_t i = 0; i < mCount; ++i) {
            const Route& r = mRoutes[i];
            if (r.len <= bestLen || r.len > addr) continue;
            if (std::memcmp(data, r.prefix, r.len) == 0) { best = &r.dest; bestLen = r.len; }
        }
        return dispatch(*best, callCtx, data, len);
    }

    size_t routeCount() const noexcept { return mCount; }

private:
    struct Dest {
        Handler h   = nullptr;
        void*   ctx = nullptr;
    };
    static bool dispatch(const Dest& d, void* callCtx, const uint8_t* data, size_t len) noexcept {
        return d.h ? d.h(d.ctx, callCtx, data, len) : false;
    }

    static constexpr size_t kMaxRoutes = 16;
    static constexpr size_t kMaxPrefix = 63;
    struct Route {
        char   prefix[kMaxPrefix];
        size_t len = 0;
        Dest   dest;
    };

    Dest   mDefault;
    Route  mRoutes[kMaxRoutes] = {};
    size_t mCount = 0;
};

// The ingress the audio-thread drain classifies through, published by the engine
// at init (native: SupersonicEngine::mIngress; wasm: a file-static in
// audio_processor.cpp). Mirrors g_active_superclock — single publisher, the
// audio thread loads it `acquire` and null-checks. Defined in audio_processor.cpp
// (compiled by both targets).
extern std::atomic<OscIngress*> g_active_ingress;

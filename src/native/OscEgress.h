/*
 * OscEgress.h — deferred egress framing + dispatch (transport-agnostic).
 *
 * Producer calls (reply / broadcast / debug, from EngineControl, Link, device
 * and loader threads) frame the OSC, tagged with a route, into the NRT-out ring
 * under a lock. The NRT gateway drains that ring and dispatches each message to
 * the injected IOscTransport (reply/broadcast/subscribe) — the only thing that
 * knows about sockets, pids or addresses. Debug rides the host's onDebug
 * callback. The engine here deals only in opaque origin tokens.
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

#include "src/shared_memory.h"   // EgressRoute
#include "src/ReplyChannel.h"

class IOscTransport;

class OscEgress {
public:
    // transport: the egress sink (replies/broadcasts/subscriber registry).
    // onDebug: host log channel for /supersonic/debug.
    void init(IOscTransport*                                  transport,
              const std::function<void(const std::string&)>*  onDebug);

    // This egress as a generic backend ReplyChannel: a reply goes out the NRT-out
    // ring keyed by the per-call token. Bound once at registration; the token is
    // the only per-message input — the reply destination travels as call metadata,
    // never as engine state.
    ReplyChannel replyChannel() { return ReplyChannel{ &OscEgress::replyThunk, this }; }

    // ── Producer side: frame into the NRT-out ring (never delivers here) ──────
    // reply/sendToCaller route to `token` (the origin threaded from the call ctx).
    void reply(uint32_t token, const uint8_t* data, uint32_t size);
    void sendToCaller(uint32_t token, const uint8_t* data, uint32_t size);
    void broadcastToTargets(const uint8_t* data, uint32_t size);
    void broadcastLinkNotify(const uint8_t* data, uint32_t size);
    void broadcastMidiNotify(const uint8_t* data, uint32_t size);  // → /midi/notify audience
    void broadcastGamepadNotify(const uint8_t* data, uint32_t size);  // → /gamepad/notify audience
    void broadcastOscNotify(const uint8_t* data, uint32_t size);  // → /osc/notify audience
    void debug(const char* text, uint32_t len);        // → /supersonic/debug
    void sendStateChange(const char* state, const char* reason);
    void sendSetup(int sampleRate, int bufferSize, uint32_t generation);

    // ── Gateway side: deliver (only the NRT gateway calls these) ──────────────
    // Drain handler for one egress frame (OUT or NRT-out), route already peeled
    // by the lanes drain: peel /supersonic/debug to onDebug, run the
    // interceptor, else route by tag to the transport.
    void dispatchEgress(uint32_t originToken, uint32_t route,
                        const uint8_t* osc, uint32_t oscLen);
    // Optional pre-dispatch hook; returns true to swallow the message.
    void setInterceptor(std::function<bool(const uint8_t*, uint32_t)> fn) { mInterceptor = std::move(fn); }
    // Deliver an already-OSC packet to the notify audience and a debug line.
    void deliverBroadcastNotify(const uint8_t* osc, uint32_t size);
    void deliverDebug(const char* text, uint32_t len);

    // ── Subscriber registry — forwarded to the transport, keyed on the origin ─
    // `token` is the caller's origin, threaded from the call ctx.
    bool subscribeCaller(uint32_t token);        // true if newly registered
    void unsubscribeCaller(uint32_t token);
    void clearSubscribers();
    void subscribeNotifyPort(int port);   // explicit reply-port (devices/report)
    bool hasSubscribers() const;
    bool subscribeCallerToLinkNotify(uint32_t token);
    void unsubscribeCallerFromLinkNotify(uint32_t token);
    bool subscribeCallerToMidiNotify(uint32_t token);
    void unsubscribeCallerFromMidiNotify(uint32_t token);
    bool subscribeCallerToGamepadNotify(uint32_t token);
    void unsubscribeCallerFromGamepadNotify(uint32_t token);
    bool subscribeCallerToOscNotify(uint32_t token);
    void unsubscribeCallerFromOscNotify(uint32_t token);

private:
    enum Route : uint32_t {
        REPLY = 0, SEND_TO_CALLER = 1, BROADCAST_NOTIFY = 2, BROADCAST_LINK = 3, BROADCAST_MIDI = 4,
        BROADCAST_GAMEPAD = 5, BROADCAST_OSC = 6
    };
    // Must match the shared on-ring EgressRoute values (shared_memory.h).
    static_assert(uint32_t(REPLY) == uint32_t(EGRESS_REPLY) &&
                  uint32_t(SEND_TO_CALLER) == uint32_t(EGRESS_SEND_TO_CALLER) &&
                  uint32_t(BROADCAST_NOTIFY) == uint32_t(EGRESS_BROADCAST_NOTIFY) &&
                  uint32_t(BROADCAST_LINK) == uint32_t(EGRESS_BROADCAST_LINK) &&
                  uint32_t(BROADCAST_MIDI) == uint32_t(EGRESS_BROADCAST_MIDI) &&
                  uint32_t(BROADCAST_GAMEPAD) == uint32_t(EGRESS_BROADCAST_GAMEPAD) &&
                  uint32_t(BROADCAST_OSC) == uint32_t(EGRESS_BROADCAST_OSC),
                  "OscEgress::Route must match shared_memory.h EgressRoute");
    void frame(Route route, uint32_t token, const uint8_t* osc, uint32_t size);
    static void replyThunk(void* ctx, uint32_t token, const uint8_t* osc, uint32_t len) {
        static_cast<OscEgress*>(ctx)->frame(REPLY, token, osc, len);
    }

    IOscTransport*                                  mTransport = nullptr;
    const std::function<void(const std::string&)>*  mOnDebug   = nullptr;
    std::function<bool(const uint8_t*, uint32_t)>    mInterceptor;  // pre-dispatch swallow hook
};

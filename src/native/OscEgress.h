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

class IOscTransport;

class OscEgress {
public:
    // transport: the egress sink (replies/broadcasts/subscriber registry).
    // onDebug: host log channel for /supersonic/debug. egress*: the NRT-out ring.
    void init(IOscTransport*                                  transport,
              const std::function<void(const std::string&)>*  onDebug,
              uint8_t*               egressBuffer,
              uint32_t               egressBufferSize,
              std::atomic<int32_t>*  egressHead,
              std::atomic<int32_t>*  egressTail,
              std::atomic<int32_t>*  egressSeq,
              std::atomic<int32_t>*  egressLock);

    // Stamp the origin of the packet currently being handled (an opaque token).
    void setOrigin(uint32_t token) { mOriginToken.store(token, std::memory_order_relaxed); }

    // ── Producer side: frame into the NRT-out ring (never delivers here) ──────
    void reply(const uint8_t* data, uint32_t size);
    void sendToCaller(const uint8_t* data, uint32_t size);
    void broadcastToTargets(const uint8_t* data, uint32_t size);
    void broadcastLinkNotify(const uint8_t* data, uint32_t size);
    void broadcastMidiNotify(const uint8_t* data, uint32_t size);  // → /midi/notify audience
    void debug(const char* text, uint32_t len);        // → /supersonic/debug
    void sendStateChange(const char* state, const char* reason);
    void sendSetup(int sampleRate, int bufferSize, uint32_t generation);

    // ── Gateway side: deliver (only the NRT gateway calls these) ──────────────
    // Drain handler for one egress frame (OUT or NRT-out): peel /supersonic/debug
    // to onDebug, run the interceptor, else route by tag to the transport.
    void dispatchEgress(uint32_t originToken, const uint8_t* payload, uint32_t len);
    // Optional pre-dispatch hook; returns true to swallow the message.
    void setInterceptor(std::function<bool(const uint8_t*, uint32_t)> fn) { mInterceptor = std::move(fn); }
    // Deliver an already-OSC packet to the notify audience and a debug line.
    void deliverBroadcastNotify(const uint8_t* osc, uint32_t size);
    void deliverDebug(const char* text, uint32_t len);

    // ── Subscriber registry — forwarded to the transport, keyed on the origin ─
    bool subscribeCaller();        // true if newly registered
    void unsubscribeCaller();
    void clearSubscribers();
    void subscribeNotifyPort(int port);   // explicit reply-port (devices/report)
    bool hasSubscribers() const;
    bool subscribeCallerToLinkNotify();
    void unsubscribeCallerFromLinkNotify();
    bool subscribeCallerToMidiNotify();
    void unsubscribeCallerFromMidiNotify();

private:
    enum Route : uint32_t {
        REPLY = 0, SEND_TO_CALLER = 1, BROADCAST_NOTIFY = 2, BROADCAST_LINK = 3, BROADCAST_MIDI = 4
    };
    // Must match the shared on-ring EgressRoute values (shared_memory.h).
    static_assert(uint32_t(REPLY) == uint32_t(EGRESS_REPLY) &&
                  uint32_t(SEND_TO_CALLER) == uint32_t(EGRESS_SEND_TO_CALLER) &&
                  uint32_t(BROADCAST_NOTIFY) == uint32_t(EGRESS_BROADCAST_NOTIFY) &&
                  uint32_t(BROADCAST_LINK) == uint32_t(EGRESS_BROADCAST_LINK) &&
                  uint32_t(BROADCAST_MIDI) == uint32_t(EGRESS_BROADCAST_MIDI),
                  "OscEgress::Route must match shared_memory.h EgressRoute");
    void frame(Route route, uint32_t token, const uint8_t* osc, uint32_t size);

    IOscTransport*                                  mTransport = nullptr;
    const std::function<void(const std::string&)>*  mOnDebug   = nullptr;
    std::function<bool(const uint8_t*, uint32_t)>    mInterceptor;  // pre-dispatch swallow hook

    std::atomic<uint32_t> mOriginToken{0};

    // NRT-out ring — producers frame here under mEgressLock.
    uint8_t*              mEgressBuffer     = nullptr;
    uint32_t              mEgressBufferSize = 0;
    std::atomic<int32_t>* mEgressHead       = nullptr;
    std::atomic<int32_t>* mEgressTail       = nullptr;
    std::atomic<int32_t>* mEgressSeq        = nullptr;
    std::atomic<int32_t>* mEgressLock       = nullptr;
};

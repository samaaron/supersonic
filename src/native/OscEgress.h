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
    void debug(const char* text, uint32_t len);        // → /supersonic/debug
    void sendStateChange(const char* state, const char* reason);
    void sendSetup(int sampleRate, int bufferSize, uint32_t generation);

    // ── Gateway side: deliver (only the NRT gateway calls these) ──────────────
    // Drain handler for one NRT-out message: reads the route tag and dispatches
    // to the transport (or onDebug for /supersonic/debug).
    void dispatchEgress(uint32_t originToken, const uint8_t* payload, uint32_t len);
    // Deliver an already-OSC packet to the notify audience (the OUT-ring drain's
    // audio-thread replies) and a debug line (audio-thread ss_log).
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

private:
    enum Route : uint32_t { REPLY = 0, SEND_TO_CALLER = 1, BROADCAST_NOTIFY = 2, BROADCAST_LINK = 3 };
    void frame(Route route, uint32_t token, const uint8_t* osc, uint32_t size);

    IOscTransport*                                  mTransport = nullptr;
    const std::function<void(const std::string&)>*  mOnDebug   = nullptr;

    std::atomic<uint32_t> mOriginToken{0};

    // NRT-out ring — producers frame here under mEgressLock.
    uint8_t*              mEgressBuffer     = nullptr;
    uint32_t              mEgressBufferSize = 0;
    std::atomic<int32_t>* mEgressHead       = nullptr;
    std::atomic<int32_t>* mEgressTail       = nullptr;
    std::atomic<int32_t>* mEgressSeq        = nullptr;
    std::atomic<int32_t>* mEgressLock       = nullptr;
};

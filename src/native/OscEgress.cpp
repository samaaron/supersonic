/*
 * OscEgress.cpp — deferred egress: frame on the producer side, dispatch to the
 * injected IOscTransport on the gateway side. No address/socket knowledge here.
 */
#include "OscEgress.h"

#include "IOscTransport.h"
#include "osc_debug.h"
#include "src/lanes/lanes_internal.h"  // ss_egress_nrt_write (NRT egress producer)
#include "osc/OscOutboundPacketStream.h"
#include <cstring>

void OscEgress::init(IOscTransport*                                  transport,
                     const std::function<void(const std::string&)>*  onDebug) {
    mTransport = transport;
    mOnDebug   = onDebug;
}

// ── Producer side: frame `[route][osc]` into the NRT-out ring ─────────────────

void OscEgress::frame(Route route, uint32_t token, const uint8_t* osc, uint32_t size) {
    // The lanes NRT egress producer (src/lanes/lanes.cpp) owns the NRT-out
    // ring location and the producer lock.
    ss_egress_nrt_write(static_cast<uint32_t>(route), token, osc, size);
}

void OscEgress::reply(const uint8_t* data, uint32_t size) {
    frame(REPLY, mOriginToken.load(std::memory_order_relaxed), data, size);
}

void OscEgress::sendToCaller(const uint8_t* data, uint32_t size) {
    frame(SEND_TO_CALLER, mOriginToken.load(std::memory_order_relaxed), data, size);
}

void OscEgress::broadcastToTargets(const uint8_t* data, uint32_t size) {
    frame(BROADCAST_NOTIFY, 0, data, size);
}

void OscEgress::broadcastLinkNotify(const uint8_t* data, uint32_t size) {
    frame(BROADCAST_LINK, 0, data, size);
}

void OscEgress::broadcastMidiNotify(const uint8_t* data, uint32_t size) {
    frame(BROADCAST_MIDI, 0, data, size);
}

void OscEgress::broadcastGamepadNotify(const uint8_t* data, uint32_t size) {
    frame(BROADCAST_GAMEPAD, 0, data, size);
}

void OscEgress::debug(const char* text, uint32_t len) {
    char pkt[1024];
    uint32_t p = supersonic::buildDebugOsc(pkt, text, len);
    frame(BROADCAST_NOTIFY, 0, reinterpret_cast<const uint8_t*>(pkt), p);
}

// ── Gateway side: dispatch one framed message to the transport / onDebug ─────

void OscEgress::dispatchEgress(uint32_t originToken, uint32_t route,
                               const uint8_t* osc, uint32_t oscLen) {
    // Debug log lines go to the host's onDebug channel.
    if (oscLen >= supersonic::kDebugArgOffset &&
        std::memcmp(osc, "/supersonic/debug", 17) == 0) {
        const char* s = reinterpret_cast<const char*>(osc) + supersonic::kDebugArgOffset;
        deliverDebug(s, static_cast<uint32_t>(strnlen(s, oscLen - supersonic::kDebugArgOffset)));
        return;
    }

    // Engine pre-dispatch hook (e.g. /supersonic/buffer/freed).
    if (mInterceptor && mInterceptor(osc, oscLen)) return;

    if (!mTransport) return;
    switch (route) {
        case REPLY:          mTransport->send(originToken, osc, oscLen, /*networkOnly*/ false); break;
        case SEND_TO_CALLER: mTransport->send(originToken, osc, oscLen, /*networkOnly*/ true);  break;
        case BROADCAST_LINK: mTransport->broadcastLink(osc, oscLen); break;
        case BROADCAST_MIDI: mTransport->broadcastMidi(osc, oscLen); break;
        case BROADCAST_GAMEPAD: mTransport->broadcastGamepad(osc, oscLen); break;
        case BROADCAST_NOTIFY:
        default:             mTransport->broadcastNotify(osc, oscLen); break;
    }
}

void OscEgress::deliverBroadcastNotify(const uint8_t* osc, uint32_t size) {
    if (mTransport) mTransport->broadcastNotify(osc, size);
}

void OscEgress::deliverDebug(const char* text, uint32_t len) {
    if (mOnDebug && *mOnDebug) (*mOnDebug)(std::string(text, len));
}

// ── Subscriber registry — forwarded to the transport, keyed on the origin ────

bool OscEgress::subscribeCaller() {
    return mTransport && mTransport->subscribeNotify(mOriginToken.load(std::memory_order_relaxed));
}

void OscEgress::unsubscribeCaller() {
    if (mTransport) mTransport->unsubscribeNotify(mOriginToken.load(std::memory_order_relaxed));
}

void OscEgress::clearSubscribers() {
    if (mTransport) mTransport->clearNotify();
}

void OscEgress::subscribeNotifyPort(int port) {
    if (mTransport) mTransport->subscribeNotifyPort(port);
}

bool OscEgress::hasSubscribers() const {
    return mTransport && mTransport->hasNotifySubscribers();
}

bool OscEgress::subscribeCallerToLinkNotify() {
    return mTransport && mTransport->subscribeLink(mOriginToken.load(std::memory_order_relaxed));
}

void OscEgress::unsubscribeCallerFromLinkNotify() {
    if (mTransport) mTransport->unsubscribeLink(mOriginToken.load(std::memory_order_relaxed));
}

bool OscEgress::subscribeCallerToMidiNotify() {
    return mTransport && mTransport->subscribeMidi(mOriginToken.load(std::memory_order_relaxed));
}

void OscEgress::unsubscribeCallerFromMidiNotify() {
    if (mTransport) mTransport->unsubscribeMidi(mOriginToken.load(std::memory_order_relaxed));
}

bool OscEgress::subscribeCallerToGamepadNotify() {
    return mTransport && mTransport->subscribeGamepad(mOriginToken.load(std::memory_order_relaxed));
}

void OscEgress::unsubscribeCallerFromGamepadNotify() {
    if (mTransport) mTransport->unsubscribeGamepad(mOriginToken.load(std::memory_order_relaxed));
}

// ── Small generic lifecycle broadcasts (gated by an audience) ────────────────

void OscEgress::sendStateChange(const char* state, const char* reason) {
    if (!hasSubscribers()) return;
    char buf[512];
    osc::OutboundPacketStream s(buf, sizeof(buf));
    s << osc::BeginMessage("/supersonic/statechange")
      << state
      << reason
      << osc::EndMessage;
    broadcastToTargets(reinterpret_cast<const uint8_t*>(s.Data()),
                       static_cast<uint32_t>(s.Size()));
}

void OscEgress::sendSetup(int sampleRate, int bufferSize, uint32_t generation) {
    if (!hasSubscribers()) return;
    char buf[256];
    osc::OutboundPacketStream s(buf, sizeof(buf));
    s << osc::BeginMessage("/supersonic/setup")
      << static_cast<osc::int32>(sampleRate)
      << static_cast<osc::int32>(bufferSize)
      << static_cast<osc::int32>(generation)
      << osc::EndMessage;
    broadcastToTargets(reinterpret_cast<const uint8_t*>(s.Data()),
                       static_cast<uint32_t>(s.Size()));
}

/*
 * SubscribeAck.h — correlation-token ack for the notify/subscribe verbs
 */
#pragma once

#include "OscEgress.h"
#include "osc/OscOutboundPacketStream.h"
#include "osc/OscReceivedElements.h"

#include <cstdint>

// The subsystem subscribe verbs (/midi, /gamepad, /osc, /clock +
// /notify/subscribe) are fire-and-forget UDP from the client's point of
// view: a subscribe sent while the server can't receive is silently
// lost, and with it every event the subsystem would ever push. A client
// that needs certainty appends an int32 correlation token (the same
// scheme as the /clock request/reply verbs — see "Correlation tokens"
// in docs/OSC_API.md) and resends until the token comes back on
// "<verb>.reply". Token-less subscribes keep their historical
// no-reply behaviour, so existing clients see no new traffic.
//
// The LAST int32 argument is the token, matching the rpc convention of
// appending it after the verb's own arguments.
inline void ackSubscribeIfTokened(OscEgress* egress, uint32_t callerToken,
                                  const uint8_t* data, uint32_t size,
                                  const char* replyAddr) {
    if (!egress) return;
    int32_t rpcToken = 0;
    bool haveToken = false;
    try {
        osc::ReceivedMessage msg(osc::ReceivedPacket(
            reinterpret_cast<const char*>(data),
            static_cast<osc::osc_bundle_element_size_t>(size)));
        for (auto it = msg.ArgumentsBegin(); it != msg.ArgumentsEnd(); ++it) {
            if (it->IsInt32()) {
                rpcToken = it->AsInt32Unchecked();
                haveToken = true;
            }
        }
    } catch (...) {
        return;  // malformed args — no ack, same as token-less
    }
    if (!haveToken) return;

    char buf[96];
    osc::OutboundPacketStream s(buf, sizeof(buf));
    s << osc::BeginMessage(replyAddr) << rpcToken << osc::EndMessage;
    // reply(), not sendToCaller(): an ack is the direct response to a
    // request, and reply's routing also reaches in-process (embedder)
    // callers — sendToCaller is network-only by design.
    egress->reply(callerToken,
                  reinterpret_cast<const uint8_t*>(s.Data()),
                  static_cast<uint32_t>(s.Size()));
}

/*
 * ShmTransport.cpp — see ShmTransport.h. Reply-ring frames are standard
 * Message-framed OSC (ring/ring.h wire format, sourceId = origin token, no
 * route word): the peer drains them with the stock reader protocol.
 */
#include "ShmTransport.h"

#include "RingBufferWriter.h"

bool ShmTransport::writeReply(const uint8_t* data, uint32_t size) {
    if (!mPlaneSlot) return false;
    ShmPeerPlaneHeader* plane = mPlaneSlot->load(std::memory_order_acquire);
    if (!plane) return false;
    bool ok = RingBufferWriter::write(
        shm_peer_rep_ring(plane), SHM_PEER_REP_RING_SIZE,
        &plane->rep_head, &plane->rep_tail, &plane->rep_sequence,
        &mRepWriteLock,
        data, size, SHM_PEER_ORIGIN_TOKEN);
    if (!ok)
        plane->rep_dropped.fetch_add(1, std::memory_order_relaxed);
    return ok;
}

// networkOnly is moot — the peer is a real external process, not an
// in-process observer.
bool ShmTransport::send(uint32_t token, const uint8_t* data, uint32_t size,
                        bool /*networkOnly*/) {
    // Resolve semantics: only the plane's peer is addressable here (token 0 is
    // the in-process caller; any other value is no peer of ours).
    if (token != SHM_PEER_ORIGIN_TOKEN) return false;
    return writeReply(data, size);
}

void ShmTransport::broadcastNotify(const uint8_t* data, uint32_t size) {
    if (mNotify.load(std::memory_order_relaxed)) writeReply(data, size);
}
void ShmTransport::broadcastLink(const uint8_t* data, uint32_t size) {
    if (mLink.load(std::memory_order_relaxed)) writeReply(data, size);
}
void ShmTransport::broadcastMidi(const uint8_t* data, uint32_t size) {
    if (mMidi.load(std::memory_order_relaxed)) writeReply(data, size);
}
void ShmTransport::broadcastGamepad(const uint8_t* data, uint32_t size) {
    if (mGamepad.load(std::memory_order_relaxed)) writeReply(data, size);
}
void ShmTransport::broadcastOsc(const uint8_t* data, uint32_t size) {
    if (mOsc.load(std::memory_order_relaxed)) writeReply(data, size);
}

// Caller-relative subscription with one possible caller: accept only the
// peer's token (an in-process caller — token 0 — is unaddressable here, the
// same rejection the socket transports apply to portless callers).
bool ShmTransport::subscribeFlag(std::atomic<bool>& flag, uint32_t token) {
    if (token != SHM_PEER_ORIGIN_TOKEN) return false;
    return !flag.exchange(true, std::memory_order_relaxed);
}

bool ShmTransport::subscribeNotify(uint32_t token) { return subscribeFlag(mNotify, token); }
// Registering an explicit localhost reply *port* is a UDP concept (the GUI's
// /supersonic/devices/report path); the SHM peer subscribes caller-relative.
void ShmTransport::subscribeNotifyPort(int /*port*/) {}
void ShmTransport::unsubscribeNotify(uint32_t token) {
    if (token == SHM_PEER_ORIGIN_TOKEN) mNotify.store(false, std::memory_order_relaxed);
}
void ShmTransport::clearNotify() { mNotify.store(false, std::memory_order_relaxed); }

bool ShmTransport::subscribeLink(uint32_t token) { return subscribeFlag(mLink, token); }
void ShmTransport::unsubscribeLink(uint32_t token) {
    if (token == SHM_PEER_ORIGIN_TOKEN) mLink.store(false, std::memory_order_relaxed);
}
bool ShmTransport::subscribeMidi(uint32_t token) { return subscribeFlag(mMidi, token); }
void ShmTransport::unsubscribeMidi(uint32_t token) {
    if (token == SHM_PEER_ORIGIN_TOKEN) mMidi.store(false, std::memory_order_relaxed);
}
bool ShmTransport::subscribeGamepad(uint32_t token) { return subscribeFlag(mGamepad, token); }
void ShmTransport::unsubscribeGamepad(uint32_t token) {
    if (token == SHM_PEER_ORIGIN_TOKEN) mGamepad.store(false, std::memory_order_relaxed);
}
bool ShmTransport::subscribeOsc(uint32_t token) { return subscribeFlag(mOsc, token); }
void ShmTransport::unsubscribeOsc(uint32_t token) {
    if (token == SHM_PEER_ORIGIN_TOKEN) mOsc.store(false, std::memory_order_relaxed);
}

/*
 * SuperSonic
 * Copyright (c) 2026 Sam Aaron
 *
 * Dual-licensed MIT OR GPL-3.0-or-later (see repo LICENSE).
 *
 * ShmTransport.h — the SHM command-plane transport: replies and notify pushes
 * for the segment's one trusted peer (shm_peer_plane.h), written into the
 * plane's reply ring. The counterpart of the gateway's command-ring drain.
 *
 * Single-producer by construction: the NRT gateway is the sole IOscTransport
 * caller, so the reply ring needs no shared lock — the writer-serialisation
 * word RingBufferWriter requires is a process-local member, deliberately not
 * in shared memory (the peer cannot stall the host by stomping it).
 *
 * The host never blocks on peer state: a full reply ring (slow or dead peer)
 * drops the packet and counts it in the plane's rep_dropped. Token semantics
 * mirror the socket transports — send() resolves only SHM_PEER_ORIGIN_TOKEN
 * (the token the gateway drain stamps on every peer command); anything else
 * is undeliverable here. The subscriber audiences collapse to booleans: there
 * is exactly one possible subscriber.
 *
 * Binds to the ENGINE'S plane slot (SupersonicEngine::peerPlaneSlot()) rather
 * than a plane pointer, loading it per call — so it follows the engine's
 * segment lifecycle (init publishes, shutdown nulls) with no re-wiring.
 */
#pragma once

#include <atomic>
#include <cstdint>

#include "IOscTransport.h"
#include "src/shm_peer_plane.h"

class ShmTransport : public IOscTransport {
public:
    // Bind to the engine's published plane slot. Main.cpp calls this from its
    // startTransport hook — after engine.init(), which has already started the
    // egress drain thread that calls send()/writeReply(). The slot pointer is
    // therefore published across threads, so it is atomic (release here pairs
    // with the acquire loads in writeReply/ready).
    void bindPlaneSlot(std::atomic<ShmPeerPlaneHeader*>* slot) {
        mPlaneSlot.store(slot, std::memory_order_release);
    }

    // True once bound to a slot holding a live plane.
    bool ready() const {
        auto* slot = mPlaneSlot.load(std::memory_order_acquire);
        return slot && slot->load(std::memory_order_acquire) != nullptr;
    }

    // ── IOscTransport ──────────────────────────────────────────────────────────
    bool send(uint32_t token, const uint8_t* data, uint32_t size, bool networkOnly) override;
    void broadcastNotify(const uint8_t* data, uint32_t size) override;
    void broadcastLink(const uint8_t* data, uint32_t size) override;
    bool hasNotifySubscribers() const override { return mNotify.load(std::memory_order_relaxed); }
    bool subscribeNotify(uint32_t token) override;
    void subscribeNotifyPort(int port) override;  // UDP-specific — no-op here
    void unsubscribeNotify(uint32_t token) override;
    void clearNotify() override;
    bool subscribeLink(uint32_t token) override;
    void unsubscribeLink(uint32_t token) override;
    void broadcastMidi(const uint8_t* data, uint32_t size) override;
    bool subscribeMidi(uint32_t token) override;
    void unsubscribeMidi(uint32_t token) override;
    void broadcastGamepad(const uint8_t* data, uint32_t size) override;
    bool subscribeGamepad(uint32_t token) override;
    void unsubscribeGamepad(uint32_t token) override;
    void broadcastOsc(const uint8_t* data, uint32_t size) override;
    bool subscribeOsc(uint32_t token) override;
    void unsubscribeOsc(uint32_t token) override;

private:
    // Write one packet into the reply ring; drop + count on full/unbound.
    bool writeReply(const uint8_t* data, uint32_t size);
    bool subscribeFlag(std::atomic<bool>& flag, uint32_t token);

    // Points at the engine's plane slot; bound (main thread) after the egress
    // drain thread is already running, so the pointer itself is atomic.
    std::atomic<std::atomic<ShmPeerPlaneHeader*>*> mPlaneSlot{nullptr};

    // Host-side writer serialisation for the reply ring (see file header).
    std::atomic<int32_t> mRepWriteLock{0};

    // The one-peer subscriber audiences.
    std::atomic<bool> mNotify{false};
    std::atomic<bool> mLink{false};
    std::atomic<bool> mMidi{false};
    std::atomic<bool> mGamepad{false};
    std::atomic<bool> mOsc{false};
};

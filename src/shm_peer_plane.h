/*
    SuperSonic
    Copyright (c) 2026 Sam Aaron

    Dual-licensed MIT OR GPL-3.0-or-later (see repo LICENSE).
*/

/*
 * shm_peer_plane.h — the cross-process OSC command plane appended to the
 * native SHM segment (server_shm.hpp, MAGIC ≥ 0x5C09E007): one trusted
 * external peer (e.g. Sonic Pi's spider) sends commands and receives replies
 * through a pair of message-framed SPSC rings, using the same wire format as
 * every other SuperSonic ring (ring/ring.h; RingBufferWriter / ring_drain).
 *
 *   command ring: peer produces, the host's NRT gateway consumes and feeds
 *                 the ordinary ingest path (so routing, scheduling, and reply
 *                 tokens behave exactly as they do for socket transports).
 *   reply ring:   the NRT gateway produces (ShmTransport — the gateway is the
 *                 sole transport caller, so single-producer holds by
 *                 construction), the peer consumes.
 *
 * Crash-safety is structural rather than recovered:
 *   - A ring's commit point is the producer's head release-store
 *     (RingBufferWriter): a producer that dies mid-write never publishes the
 *     partial frame. With exactly one producer per ring there is no lock to
 *     inherit from a corpse.
 *   - The host consumer (ring_drain) validates every header and treats the
 *     shared cursors as untrusted, so no peer state can make it read out of
 *     bounds or spin.
 *   - The host producer derives every write offset from its own head cursor;
 *     a hostile rep_tail can only cause reply drops, never out-of-bounds
 *     writes. Ring-full replies are dropped and counted (rep_dropped) — the
 *     host never blocks on peer state.
 *   - cmd_write_lock exists only so the peer can reuse the stock
 *     RingBufferWriter; with a single producer it is never contended. A peer
 *     that dies while holding it stalls nobody on the host (the host never
 *     takes it); shm_peer_attach() resets it, which is safe precisely
 *     because the plane is single-peer by contract — at attach time no live
 *     producer exists.
 *
 * Attach protocol (peer side): shm_peer_attach() stamps the pid, bumps the
 * generation, resets the writer lock, and skips any stale replies (they were
 * addressed to a dead process). Last attach wins. The host side never touches
 * the ownership fields and needs no liveness view of the peer: a dead peer
 * costs the host nothing.
 *
 * Commands are stamped with SHM_PEER_ORIGIN_TOKEN by the host drain — the
 * frame's own sourceId is ignored, so origin identity is assigned by the
 * transport exactly as with sockets, never trusted from shared memory.
 */
#pragma once

#include <atomic>
#include <cstdint>

// Ring capacities. The command ring is sized for synthdef payloads (/d_recv
// blobs); frames never wrap, so the largest guaranteed-writable frame is
// bounded by the writer's contiguous-fit check, not by these totals.
constexpr uint32_t SHM_PEER_CMD_RING_SIZE = 256 * 1024;
constexpr uint32_t SHM_PEER_REP_RING_SIZE = 128 * 1024;

// The origin token the host drain stamps on every peer command, and the only
// token ShmTransport resolves (send() to any other token reports
// undeliverable, mirroring the socket transports' resolve semantics).
// Distinctive value ("SHMP") so it reads clearly in logs and ring dumps.
constexpr uint32_t SHM_PEER_ORIGIN_TOKEN = 0x53484D50;

struct alignas(8) ShmPeerPlaneHeader {
    // ── ownership (peer-written; the host never touches these) ──────────────
    std::atomic<uint32_t> owner_pid;    // last attacher; informational
    std::atomic<uint32_t> generation;   // bumped by every attach

    // ── command ring cursors (peer = producer, host = consumer) ─────────────
    std::atomic<int32_t> cmd_head;
    std::atomic<int32_t> cmd_tail;
    std::atomic<int32_t> cmd_sequence;
    std::atomic<int32_t> cmd_write_lock;  // writer-convenience only; see file header

    // ── reply ring cursors (host = producer, peer = consumer) ───────────────
    // The host's writer serialisation lock is process-local (ShmTransport
    // member), deliberately NOT in shared memory: the peer cannot stall the
    // host by stomping a lock word.
    std::atomic<int32_t>  rep_head;
    std::atomic<int32_t>  rep_tail;
    std::atomic<int32_t>  rep_sequence;
    std::atomic<uint32_t> rep_dropped;    // replies dropped (ring full)

    // ── geometry (host-written at segment create; peer reads) ───────────────
    uint32_t cmd_ring_size;
    uint32_t rep_ring_size;

    uint32_t _reserved[4];
};
static_assert(sizeof(ShmPeerPlaneHeader) == 64,
              "ShmPeerPlaneHeader must stay 64 bytes — the rings follow at fixed offsets");

constexpr uint32_t SHM_PEER_PLANE_TOTAL_SIZE =
    static_cast<uint32_t>(sizeof(ShmPeerPlaneHeader))
    + SHM_PEER_CMD_RING_SIZE + SHM_PEER_REP_RING_SIZE;

// Ring bases. Sizes are compile-time constants on the host side — the host
// never trusts the header's geometry fields (they exist for the peer, which
// has no compile-time view of this header's build).
inline uint8_t* shm_peer_cmd_ring(ShmPeerPlaneHeader* h) {
    return reinterpret_cast<uint8_t*>(h + 1);
}
inline uint8_t* shm_peer_rep_ring(ShmPeerPlaneHeader* h) {
    return shm_peer_cmd_ring(h) + SHM_PEER_CMD_RING_SIZE;
}

// Host side, at segment create (the segment arrives zeroed; only the
// geometry needs explicit values).
inline void shm_peer_plane_init(ShmPeerPlaneHeader* h) {
    h->cmd_ring_size = SHM_PEER_CMD_RING_SIZE;
    h->rep_ring_size = SHM_PEER_REP_RING_SIZE;
}

// Peer side: claim the plane. Returns the new generation. See the attach
// protocol in the file header.
inline uint32_t shm_peer_attach(ShmPeerPlaneHeader* h, uint32_t pid) {
    h->owner_pid.store(pid, std::memory_order_relaxed);
    h->cmd_write_lock.store(0, std::memory_order_relaxed);
    // Skip replies addressed to a dead predecessor. Tail is consumer-owned;
    // moving it forward is always safe for the producer.
    h->rep_tail.store(h->rep_head.load(std::memory_order_acquire),
                      std::memory_order_release);
    return h->generation.fetch_add(1, std::memory_order_acq_rel) + 1;
}

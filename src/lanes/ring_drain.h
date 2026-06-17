/*
    SuperSonic
    Copyright (c) 2025 Sam Aaron

    Dual-licensed MIT OR GPL-3.0-or-later (see repo LICENSE).
*/

/*
 * ring_drain.h — Message-framed ring drain: the consumer-side walk (header
 * validation, untrusted-cursor repair, padding markers, sequence-gap
 * tracking, tail resync on corruption). The single C++ implementation of
 * the read side: used by the audio-thread IN drain (process_audio), the
 * lanes egress drains (lanes.cpp) and the native RingReader thread. The JS
 * reader (ring_buffer_core.js) is a separate implementation of the same
 * wire protocol, held equivalent by the ring-wire conformance fixtures.
 * Header-only; standard C++ and ring/ring.h only, no platform
 * dependencies, no allocation, no copying.
 *
 * Wire invariant: frames are contiguous — writers never wrap a frame across
 * the ring boundary (they emit a PADDING_MAGIC marker and restart at offset
 * 0 instead; see RingBufferWriter.h). Delivery is therefore always in
 * place: the callback receives a pointer into the ring, valid only for the
 * duration of the call. A frame that would cross the boundary is treated as
 * corruption, never read past the ring's end.
 *
 * The tail advances only AFTER the callback returns Consume — the consumer
 * owns the frame's ring region for the whole callback, so writers (who
 * measure free space against the tail) cannot reuse it mid-read. Returning
 * Retain leaves the frame at the head of the ring and stops the drain: the
 * next drain call delivers the same frame again (backpressure).
 *
 * Single-consumer per ring: the caller owns the tail and the SsDrainState.
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>

#include "../ring/ring.h"  // Message, MESSAGE_MAGIC, PADDING_MAGIC

// Callback verdict: Consume advances the tail past the frame; Retain leaves
// the frame in the ring and stops the drain (deliver it again next call).
enum class SsDrainVerdict : uint8_t { Consume, Retain };

// Why the walk stopped. Empty/MaxFrames/Retained are normal operation;
// BadCursor/BadMagic/BadLength mean the ring (or its shared-memory cursors)
// was corrupt and the pending region was dropped — callers that own logging
// or status flags act on these.
enum class SsDrainStop : uint8_t {
    Empty,      // no more complete frames
    MaxFrames,  // consumed maxFrames and stopped
    Retained,   // callback kept the head frame in the ring
    BadCursor,  // out-of-range head or tail (tail repaired to head; a bad
                // head is producer state we can't repair, drain skipped)
    BadMagic,   // corrupt header magic — tail resynced to head
    BadLength,  // corrupt header length / frame crossing the ring boundary
                // — tail resynced to head
};

// Optional per-ring counters; any left null is simply not tracked.
// Field order is load-bearing: callers brace-initialise.
struct SsDrainMetrics {
    std::atomic<uint32_t>* received  = nullptr;
    std::atomic<uint32_t>* bytes     = nullptr;
    std::atomic<uint32_t>* corrupted = nullptr;
    std::atomic<uint32_t>* seqGaps   = nullptr;  // counts MISSED frames, not gap events
};

// Consumer-owned state: sequence-gap tracking across drain calls.
struct SsDrainState {
    int32_t lastSeq = -1;
};

// Walk one ring, delivering each frame to
//   onMessage(sourceId, payload, payloadSize, sequence) -> SsDrainVerdict.
// Returns the number of frames consumed. maxFrames of 0 means no bound.
// stopReason (optional) reports why the walk ended.
template <typename OnMsg>
inline uint32_t ss_drain_ring(uint8_t*              buffer,
                              uint32_t              size,
                              std::atomic<int32_t>* headPtr,
                              std::atomic<int32_t>* tailPtr,
                              SsDrainState&         st,
                              const SsDrainMetrics& metrics,
                              uint32_t              maxFrames,
                              OnMsg&&               onMessage,
                              SsDrainStop*          stopReason = nullptr) {
    SsDrainStop localStop = SsDrainStop::Empty;
    SsDrainStop& stop = stopReason ? *stopReason : localStop;
    stop = SsDrainStop::Empty;

    if (!buffer || !headPtr || !tailPtr) return 0;

    uint32_t consumed = 0;
    while (maxFrames == 0 || consumed < maxFrames) {
        int32_t head = headPtr->load(std::memory_order_acquire);
        int32_t tail = tailPtr->load(std::memory_order_relaxed);
        if (head == tail) break;

        // The cursors live in shared memory (cross-process on native) and are
        // not trusted: an out-of-range value would index outside the ring.
        // The tail is reader-owned, so a bad tail is repaired by resyncing to
        // head (dropping pending frames); a bad head is producer state we
        // can't repair, so the drain is skipped.
        if (static_cast<uint32_t>(head) >= size) {
            if (metrics.corrupted) metrics.corrupted->fetch_add(1, std::memory_order_relaxed);
            stop = SsDrainStop::BadCursor;
            break;
        }
        if (static_cast<uint32_t>(tail) >= size) {
            if (metrics.corrupted) metrics.corrupted->fetch_add(1, std::memory_order_relaxed);
            tailPtr->store(head, std::memory_order_release);
            stop = SsDrainStop::BadCursor;
            break;
        }

        uint32_t ut           = static_cast<uint32_t>(tail);
        uint32_t uh           = static_cast<uint32_t>(head);
        uint32_t avail        = (uh - ut + size) % size;
        uint32_t space_to_end = size - ut;

        // Frame offsets are 4-aligned by construction; a tail with less than
        // a magic word to the boundary is untrusted-cursor damage.
        if (space_to_end < 4 || avail < 4) {
            if (metrics.corrupted) metrics.corrupted->fetch_add(1, std::memory_order_relaxed);
            tailPtr->store(head, std::memory_order_release);
            stop = SsDrainStop::BadMagic;
            break;
        }

        uint32_t magic;
        std::memcpy(&magic, buffer + ut, sizeof(magic));

        if (magic == PADDING_MAGIC) {
            // Writer hit end-of-ring and restarted at offset 0; follow it and
            // keep draining — the frame at 0 is already published. A padding
            // marker AT offset 0 can never be legitimately written (the whole
            // ring lies ahead of a writer at 0), so treat it as corruption
            // rather than spinning on it.
            if (ut == 0) {
                if (metrics.corrupted) metrics.corrupted->fetch_add(1, std::memory_order_relaxed);
                tailPtr->store(head, std::memory_order_release);
                stop = SsDrainStop::BadMagic;
                break;
            }
            tailPtr->store(0, std::memory_order_release);
            continue;
        }
        // Anything malformed below head is genuine corruption and the rest of
        // the region is suspect: resync tail to head, dropping pending input.
        // A byte-wise rescan would be unbounded work, and a length below the
        // header size could never advance the tail at all.
        if (magic != MESSAGE_MAGIC) {
            if (metrics.corrupted) metrics.corrupted->fetch_add(1, std::memory_order_relaxed);
            tailPtr->store(head, std::memory_order_release);
            stop = SsDrainStop::BadMagic;
            break;
        }
        if (space_to_end < sizeof(Message)) {
            // A real frame header can't sit closer to the boundary than its
            // own size under the never-wrap convention.
            if (metrics.corrupted) metrics.corrupted->fetch_add(1, std::memory_order_relaxed);
            tailPtr->store(head, std::memory_order_release);
            stop = SsDrainStop::BadLength;
            break;
        }

        Message hdr;
        std::memcpy(&hdr, buffer + ut, sizeof(Message));

        // Length sanity, including the never-wrap invariant: the frame's
        // 4-aligned footprint (header.length is exact; the writer rounds the
        // occupancy up to 4) must lie entirely before the ring boundary —
        // in-place delivery must never read past the ring's end, even if a
        // shared-memory writer publishes a malformed frame.
        uint32_t totalLen   = hdr.length;
        uint32_t footprint  = (totalLen + 3u) & ~3u;
        // avail < footprint is corruption too: writers publish head only after
        // the complete frame, so a frame can never legitimately claim more
        // bytes than are published — waiting for the rest would stall the
        // lane forever on a bit-flipped length.
        if (totalLen < sizeof(Message) || footprint > size ||
            footprint > space_to_end || footprint > avail) {
            if (metrics.corrupted) metrics.corrupted->fetch_add(1, std::memory_order_relaxed);
            tailPtr->store(head, std::memory_order_release);
            stop = SsDrainStop::BadLength;
            break;
        }

        uint32_t payloadSize = totalLen - static_cast<uint32_t>(sizeof(Message));
        const uint8_t* payload = buffer + ut + sizeof(Message);

        if (payloadSize > 0) {
            SsDrainVerdict verdict = onMessage(hdr.sourceId, payload, payloadSize, hdr.sequence);
            if (verdict == SsDrainVerdict::Retain) {
                // Frame stays at the head of the ring; no counters, no gap
                // tracking — the next drain call sees it as brand new.
                stop = SsDrainStop::Retained;
                break;
            }
        }

        tailPtr->store(static_cast<int32_t>((ut + footprint) % size),
                       std::memory_order_release);

        if (metrics.seqGaps) {
            int32_t seq = static_cast<int32_t>(hdr.sequence);
            if (st.lastSeq >= 0) {
                int32_t expected = (st.lastSeq + 1) & 0x7FFFFFFF;
                if (seq != expected) {
                    int32_t gap = static_cast<int32_t>(
                        (static_cast<int64_t>(seq) - expected + 0x80000000LL) & 0x7FFFFFFF);
                    // Sanity bound — a huge "gap" is a counter reset, not loss.
                    if (gap > 0 && gap < 1000)
                        metrics.seqGaps->fetch_add(static_cast<uint32_t>(gap),
                                                   std::memory_order_relaxed);
                }
            }
            st.lastSeq = seq;
        }

        if (metrics.received) metrics.received->fetch_add(1, std::memory_order_relaxed);
        if (metrics.bytes)    metrics.bytes->fetch_add(payloadSize, std::memory_order_relaxed);
        ++consumed;
    }
    if (stop == SsDrainStop::Empty && maxFrames != 0 && consumed >= maxFrames)
        stop = SsDrainStop::MaxFrames;
    return consumed;
}

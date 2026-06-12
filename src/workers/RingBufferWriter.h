/*
 * RingBufferWriter.h — Multi-producer ring buffer write (header-only)
 *
 * Mirrors writeMessageToBuffer in ring_buffer_core.js exactly (the two are
 * held byte-identical by the test/fixtures/ring_wire.txt conformance corpus):
 *  - 16-byte message header: magic(4) + length(4) + sequence(4) + sourceId(4)
 *  - frames NEVER wrap: a frame that doesn't fit before the end of the ring
 *    is preceded by a PADDING_MAGIC marker and restarts at offset 0, so every
 *    frame is contiguous and readers can parse in place
 *  - header length is EXACT (header + payload bytes); the frame footprint —
 *    and so the cursor advance — is that length rounded up to 4 bytes, with
 *    the pad bytes zeroed. Payload sizes round-trip exactly (MIDI messages
 *    are not 4-byte multiples) while offsets stay 4-aligned.
 *  - producers serialise via the write_lock spinlock (compare_exchange)
 *  - returns false when the frame doesn't fit (backpressure, no blocking)
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__)
#include <immintrin.h>
#endif
// src-relative so this header is usable under both the native (CMake) and web
// (emcc -Isrc) include roots.
#include "shared_memory.h"

class RingBufferWriter {
public:
    // Write one message into a ring buffer.
    // buffer_start: pointer to beginning of ring buffer region
    // buffer_size:  size of the ring buffer region (4-byte multiple)
    // head/tail:    atomic head/tail pointers from ControlPointers
    // sequence:     atomic sequence counter
    // write_lock:   atomic spinlock (0=unlocked)
    // data/size:    message payload
    //
    // Returns true if written, false if it doesn't fit. Note the fit test is
    // contiguous: a frame needs aligned_size bytes before the end of the ring
    // OR before the tail at offset 0 — total free space alone is not enough.
    static bool write(
        uint8_t*              buffer_start,
        uint32_t              buffer_size,
        std::atomic<int32_t>* head,
        std::atomic<int32_t>* tail,
        std::atomic<int32_t>* sequence,
        std::atomic<int32_t>* write_lock,
        const void*           data,
        uint32_t              data_size,
        uint32_t              source_id = 0)
    {
        const uint32_t total_size   = static_cast<uint32_t>(sizeof(Message)) + data_size;
        const uint32_t aligned_size = (total_size + 3u) & ~3u;

        // Acquire spinlock
        int32_t expected = 0;
        while (!write_lock->compare_exchange_weak(expected, 1,
                std::memory_order_acquire, std::memory_order_relaxed)) {
            expected = 0;
            #if defined(__x86_64__) || defined(_M_X64) || defined(__i386__)
                _mm_pause();
            #elif defined(__aarch64__) || defined(__arm__)
                __asm__ volatile("yield");
            #endif
        }

        int32_t h = head->load(std::memory_order_relaxed);
        int32_t t = tail->load(std::memory_order_acquire);
        uint32_t uh = static_cast<uint32_t>(h);
        uint32_t ut = static_cast<uint32_t>(t);

        // Total free space (head==tail ambiguity costs one byte).
        uint32_t used  = (uh - ut + buffer_size) % buffer_size;
        uint32_t avail = buffer_size - used - 1;
        if (aligned_size > avail) {
            write_lock->store(0, std::memory_order_release);
            return false;
        }

        // Frames never wrap: if the frame doesn't fit before the end, mark the
        // remainder as padding and restart at offset 0 — which needs that much
        // contiguous room before the tail.
        uint32_t space_to_end = buffer_size - uh;
        if (aligned_size > space_to_end) {
            uint32_t space_at_front = (ut > 0) ? (ut - 1) : 0;
            if (aligned_size > space_at_front) {
                write_lock->store(0, std::memory_order_release);
                return false;
            }
            // Padding marker: magic word, zeros to the end of the ring. When
            // >= 16 bytes remain this doubles as a full zeroed pad header;
            // 4-byte alignment guarantees at least the magic always fits.
            uint32_t pad = PADDING_MAGIC;
            std::memcpy(buffer_start + uh, &pad, sizeof(pad));
            if (space_to_end > sizeof(pad))
                std::memset(buffer_start + uh + sizeof(pad), 0,
                            space_to_end - sizeof(pad));
            uh = 0;
        }

        uint32_t seq = static_cast<uint32_t>(sequence->fetch_add(1, std::memory_order_relaxed));

        // Header: length is the EXACT frame size; readers advance by its
        // 4-byte-aligned footprint.
        Message hdr;
        hdr.magic    = MESSAGE_MAGIC;
        hdr.length   = total_size;
        hdr.sequence = seq;
        hdr.sourceId = source_id;
        std::memcpy(buffer_start + uh, &hdr, sizeof(Message));

        // Payload, then zero the 0-3 alignment pad bytes (determinism — the
        // conformance fixtures compare whole ring images).
        std::memcpy(buffer_start + uh + sizeof(Message), data, data_size);
        if (aligned_size > total_size)
            std::memset(buffer_start + uh + total_size, 0, aligned_size - total_size);

        head->store(static_cast<int32_t>((uh + aligned_size) % buffer_size),
                    std::memory_order_release);
        write_lock->store(0, std::memory_order_release);
        return true;
    }
};

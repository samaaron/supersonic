/*
 * RingBufferWriter.h — Lock-free ring buffer write (header-only)
 *
 * Mirrors ring_buffer_writer.js exactly:
 *  - 16-byte message header: magic(4) + length(4) + sequence(4) + padding(4)
 *  - Uses in_write_lock spinlock via compare_exchange_weak
 *  - Returns false on full buffer (backpressure, no blocking)
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__)
#include <immintrin.h>
#endif
#include "src/shared_memory.h"

class RingBufferWriter {
public:
    // Write one OSC message into the IN ring buffer.
    // buffer_start: pointer to beginning of ring buffer region (IN_BUFFER_START)
    // buffer_size:  size of the ring buffer region (IN_BUFFER_SIZE)
    // head/tail:    atomic head/tail pointers from ControlPointers
    // sequence:     atomic sequence counter (in_sequence)
    // write_lock:   atomic spinlock (in_write_lock, 0=unlocked)
    // data/size:    OSC message payload
    //
    // Returns true if written, false if buffer is full.
    static bool write(
        uint8_t*              buffer_start,
        uint32_t              buffer_size,
        std::atomic<int32_t>* head,
        std::atomic<int32_t>* tail,
        std::atomic<int32_t>* sequence,
        std::atomic<int32_t>* write_lock,
        const void*           data,
        uint32_t              data_size)
    {
        const uint32_t total_size = static_cast<uint32_t>(sizeof(Message)) + data_size;

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

        // Check available space (circular buffer arithmetic)
        uint32_t used = (uh - ut + buffer_size) % buffer_size;
        uint32_t avail = buffer_size - used - 1;

        if (total_size > avail) {
            write_lock->store(0, std::memory_order_release);
            return false;
        }

        uint32_t seq = static_cast<uint32_t>(sequence->fetch_add(1, std::memory_order_relaxed));

        // Write message header
        Message hdr;
        hdr.magic    = MESSAGE_MAGIC;
        hdr.length   = total_size;
        hdr.sequence = seq;
        hdr._padding = 0;

        writeWrapped(buffer_start, buffer_size, uh, &hdr, sizeof(Message));
        uh = (uh + sizeof(Message)) % buffer_size;

        // Write payload
        writeWrapped(buffer_start, buffer_size, uh, data, data_size);
        uh = (uh + data_size) % buffer_size;

        head->store(static_cast<int32_t>(uh), std::memory_order_release);
        write_lock->store(0, std::memory_order_release);
        return true;
    }

private:
    // Wrapping memcpy into circular buffer
    static void writeWrapped(uint8_t* buf, uint32_t buf_size,
                             uint32_t pos, const void* src, uint32_t len)
    {
        const uint8_t* s = static_cast<const uint8_t*>(src);
        uint32_t first = buf_size - pos;
        if (len <= first) {
            std::memcpy(buf + pos, s, len);
        } else {
            std::memcpy(buf + pos, s, first);
            std::memcpy(buf, s + first, len - first);
        }
    }
};

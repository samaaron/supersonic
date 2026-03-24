/*
 * test_ring_buffer_write.cpp — Tests for the OUT/DEBUG ring buffer writer
 *
 * The OUT/DEBUG ring buffers use a contiguous-write design where messages
 * must not wrap around the buffer boundary. When a message doesn't fit
 * before the end, a padding marker is written and the head wraps to 0.
 *
 * This test verifies that the space check correctly accounts for the
 * bytes wasted by padding — a wrap-and-write must not overwrite unread data.
 */
#include <catch2/catch_test_macros.hpp>
#include <atomic>
#include <cstdint>
#include <cstring>
#include "src/shared_memory.h"

// Standalone reproduction of ring_buffer_write from audio_processor.cpp.
// Extracted here so we can test it without the full scsynth namespace globals.
static bool ring_buffer_write_standalone(
    uint8_t* buffer_start,
    uint32_t buffer_size,
    std::atomic<int32_t>* head,
    std::atomic<int32_t>* tail,
    std::atomic<int32_t>* sequence,
    const void* data,
    uint32_t data_size)
{
    Message header;
    header.magic = MESSAGE_MAGIC;
    header.length = sizeof(Message) + data_size;
    header.sequence = static_cast<uint32_t>(sequence->fetch_add(1, std::memory_order_relaxed));
    header._padding = 0;

    int32_t current_head = head->load(std::memory_order_acquire);
    int32_t current_tail = tail->load(std::memory_order_acquire);

    uint32_t available = (buffer_size - 1 - current_head + current_tail) % buffer_size;
    if (available < header.length) {
        return false;
    }

    uint32_t space_to_end = buffer_size - current_head;
    if (header.length > space_to_end) {
        // Re-check space at front after wrap
        uint32_t space_at_front = (current_tail > 0) ? (current_tail - 1) : 0;
        if (space_at_front < header.length) {
            return false;
        }

        if (space_to_end >= sizeof(Message)) {
            Message padding;
            padding.magic = PADDING_MAGIC;
            padding.length = 0;
            padding.sequence = 0;
            padding._padding = 0;
            std::memcpy(buffer_start + current_head, &padding, sizeof(Message));
        } else if (space_to_end > 0) {
            std::memset(buffer_start + current_head, 0, space_to_end);
        }
        current_head = 0;
    }

    std::memcpy(buffer_start + current_head, &header, sizeof(Message));
    std::memcpy(buffer_start + current_head + sizeof(Message), data, data_size);

    int32_t new_head = (current_head + header.length) % buffer_size;
    head->store(new_head, std::memory_order_release);
    return true;
}

TEST_CASE("ring_buffer_write rejects message when wrap wastes space needed by tail", "[RingBuffer]") {
    // Buffer layout: 128 bytes, head near end, tail near start.
    // A message that doesn't fit contiguously will pad+wrap to 0,
    // but there isn't enough room between 0 and tail for the message.
    //
    //   [UNREAD DATA.....][FREE.................][HEAD-->pad][wrap to 0]
    //   0          tail=30                       head=100   end=128
    //
    // Available (circular): (128 - 1 - 100 + 30) % 128 = 57 bytes
    // Message: 16 (header) + 32 (payload) = 48 bytes — fits in 57.
    // But space_to_end = 128 - 100 = 28. Message (48) > 28, so pad+wrap.
    // Space at front (0 to tail-1) = 29 bytes. Message needs 48. Should FAIL.
    //
    // Without the fix: available check (57 >= 48) passes, wraps to 0,
    // writes 48 bytes at position 0, overwriting unread data at 0-29.

    const uint32_t BUF_SIZE = 128;
    uint8_t buffer[BUF_SIZE];
    std::memset(buffer, 0xCC, BUF_SIZE);  // Fill with sentinel

    std::atomic<int32_t> head{100};
    std::atomic<int32_t> tail{30};
    std::atomic<int32_t> seq{0};

    // Write sentinel data in the "unread" region (0-29) to detect overwrite
    const uint8_t SENTINEL = 0xAA;
    std::memset(buffer, SENTINEL, 30);

    // Attempt to write a 32-byte payload (48 bytes total with header)
    uint8_t payload[32];
    std::memset(payload, 0x42, sizeof(payload));

    bool result = ring_buffer_write_standalone(
        buffer, BUF_SIZE, &head, &tail, &seq, payload, sizeof(payload));

    // The write should be REJECTED because after padding the end,
    // there isn't enough space at the front (29 bytes < 48 needed).
    REQUIRE(result == false);

    // Verify unread data was not corrupted
    for (int i = 0; i < 30; i++) {
        INFO("byte " << i << " was overwritten");
        REQUIRE(buffer[i] == SENTINEL);
    }
}

TEST_CASE("ring_buffer_write succeeds when wrap has enough space at front", "[RingBuffer]") {
    // Same setup but tail is further back, leaving enough room at front.
    //
    //   [FREE...........................][UNREAD][HEAD->pad][wrap to 0]
    //   0                          tail=80      head=100   end=128
    //
    // Available: (128 - 1 - 100 + 80) % 128 = 107 bytes
    // Message: 48 bytes. space_to_end = 28. Pads+wraps.
    // Space at front = 79 bytes. 48 <= 79. Should SUCCEED.

    const uint32_t BUF_SIZE = 128;
    uint8_t buffer[BUF_SIZE];
    std::memset(buffer, 0, BUF_SIZE);

    std::atomic<int32_t> head{100};
    std::atomic<int32_t> tail{80};
    std::atomic<int32_t> seq{0};

    uint8_t payload[32];
    std::memset(payload, 0x42, sizeof(payload));

    bool result = ring_buffer_write_standalone(
        buffer, BUF_SIZE, &head, &tail, &seq, payload, sizeof(payload));

    REQUIRE(result == true);

    // Verify message was written at position 0
    Message* msg = reinterpret_cast<Message*>(buffer);
    REQUIRE(msg->magic == MESSAGE_MAGIC);
    REQUIRE(msg->length == sizeof(Message) + 32);
}

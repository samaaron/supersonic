/*
 * test_ring_buffer_write.cpp — Tests for the OUT/egress ring buffer writer.
 *
 * The OUT ring uses a contiguous-write design: a message never wraps the buffer
 * boundary. When it doesn't fit before the end, a PADDING_MAGIC marker fills the
 * tail and the write restarts at 0. The space check must account for the bytes
 * wasted by padding so a wrap-and-write never overwrites unread data.
 *
 * These exercise the REAL `ring_buffer_write` from audio_processor.cpp (declared
 * extern below). It takes its head/tail/sequence/status-flags as parameters, so
 * it can be driven with local atomics — no scsynth globals required.
 */
#include <catch2/catch_test_macros.hpp>
#include <atomic>
#include <cstdint>
#include <cstring>
#include "src/shared_memory.h"

// Defined in audio_processor.cpp (global namespace). Defaults live on the
// declaration there; this extern lists every parameter explicitly.
extern bool ring_buffer_write(
    uint8_t* buffer_start,
    uint32_t buffer_size,
    std::atomic<int32_t>* head,
    std::atomic<int32_t>* tail,
    std::atomic<int32_t>* sequence,
    const void* data,
    uint32_t data_size,
    std::atomic<uint32_t>* status_flags,
    PerformanceMetrics* metrics);

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

    bool result = ring_buffer_write(
        buffer, BUF_SIZE, &head, &tail, &seq, payload, sizeof(payload), nullptr, nullptr);

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

    bool result = ring_buffer_write(
        buffer, BUF_SIZE, &head, &tail, &seq, payload, sizeof(payload), nullptr, nullptr);

    REQUIRE(result == true);

    // Verify message was written at position 0
    Message* msg = reinterpret_cast<Message*>(buffer);
    REQUIRE(msg->magic == MESSAGE_MAGIC);
    REQUIRE(msg->length == sizeof(Message) + 32);
}

TEST_CASE("ring_buffer_write frames header, payload and advancing sequence", "[RingBuffer]") {
    const uint32_t BUF_SIZE = 256;
    uint8_t buffer[BUF_SIZE];
    std::memset(buffer, 0, BUF_SIZE);

    std::atomic<int32_t> head{0};
    std::atomic<int32_t> tail{0};
    std::atomic<int32_t> seq{0};

    const char* payload = "hello";
    const uint32_t len = 5;

    REQUIRE(ring_buffer_write(
        buffer, BUF_SIZE, &head, &tail, &seq, payload, len, nullptr, nullptr));

    // Messages are packed back-to-back without alignment padding, so read each
    // header via memcpy (as the production RingReader does) rather than casting a
    // possibly-misaligned pointer to Message*.
    Message msg;
    std::memcpy(&msg, buffer, sizeof(Message));
    REQUIRE(msg.magic == MESSAGE_MAGIC);
    REQUIRE(msg.length == sizeof(Message) + len);
    REQUIRE(msg.sequence == 0u);
    REQUIRE(std::memcmp(buffer + sizeof(Message), payload, len) == 0);
    REQUIRE(head.load() == static_cast<int32_t>(sizeof(Message) + len));

    // The sequence counter is the caller's — a second write advances it. This
    // message starts at an unaligned offset (sizeof(Message) + 5), so memcpy.
    REQUIRE(ring_buffer_write(
        buffer, BUF_SIZE, &head, &tail, &seq, payload, len, nullptr, nullptr));
    Message msg2;
    std::memcpy(&msg2, buffer + sizeof(Message) + len, sizeof(Message));
    REQUIRE(msg2.sequence == 1u);
}

TEST_CASE("ring_buffer_write writes a PADDING_MAGIC marker when a message wraps", "[RingBuffer]") {
    // head leaves >= sizeof(Message) but < the message length before the end, so
    // a full padding header is written at the tail and the message restarts at 0.
    //   space_to_end = 128 - 108 = 20  (>= 16, full padding header)
    //   available    = (128 - 1 - 108 + 60) % 128 = 79  (>= 24, fits)
    //   space_at_front = 59  (>= 24)
    const uint32_t BUF_SIZE = 128;
    uint8_t buffer[BUF_SIZE];
    std::memset(buffer, 0, BUF_SIZE);

    std::atomic<int32_t> head{108};
    std::atomic<int32_t> tail{60};
    std::atomic<int32_t> seq{0};

    uint8_t payload[8];
    std::memset(payload, 0x7E, sizeof(payload));

    REQUIRE(ring_buffer_write(
        buffer, BUF_SIZE, &head, &tail, &seq, payload, sizeof(payload), nullptr, nullptr));

    // Padding marker left at the old head so the reader skips to 0.
    Message* pad = reinterpret_cast<Message*>(buffer + 108);
    REQUIRE(pad->magic == PADDING_MAGIC);

    // Message itself restarted at position 0.
    Message* msg = reinterpret_cast<Message*>(buffer);
    REQUIRE(msg->magic == MESSAGE_MAGIC);
    REQUIRE(msg->length == sizeof(Message) + sizeof(payload));
    REQUIRE(head.load() == static_cast<int32_t>(sizeof(Message) + sizeof(payload)));
}

TEST_CASE("ring_buffer_write flags STATUS_BUFFER_FULL and counts drops on overflow", "[RingBuffer]") {
    const uint32_t BUF_SIZE = 64;
    uint8_t buffer[BUF_SIZE];
    std::memset(buffer, 0, BUF_SIZE);

    std::atomic<int32_t> head{0};
    std::atomic<int32_t> tail{0};
    std::atomic<int32_t> seq{0};
    std::atomic<uint32_t> status{0};

    uint8_t payload[100];  // 16 + 100 = 116 > 64 — cannot fit
    std::memset(payload, 0x11, sizeof(payload));

    REQUIRE_FALSE(ring_buffer_write(
        buffer, BUF_SIZE, &head, &tail, &seq, payload, sizeof(payload), &status, nullptr));
    REQUIRE((status.load() & STATUS_BUFFER_FULL) != 0u);
    REQUIRE(head.load() == 0);  // nothing written
}

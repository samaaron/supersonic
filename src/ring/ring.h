/*
    SuperSonic
    Copyright (c) 2025 Sam Aaron

    Dual-licensed MIT OR GPL-3.0-or-later (see repo LICENSE).

    Message-framed ring buffer primitives: the 16-byte frame header and the magic
    constants shared by the writer (RingBufferWriter.h) and the reader
    (ring_drain.h). Frames never wrap the ring boundary — a frame that would cross
    the end is preceded by a PADDING_MAGIC marker and restarts at offset 0.
    Mirrors the JS ring (ring_buffer_core.js), held byte-identical by the
    ring-wire conformance fixtures.
*/

#pragma once

#include <cstdint>

// 16-byte frame header; payload follows. Layout matches ring_buffer_core.js.
struct alignas(4) Message {
    uint32_t magic;       // MESSAGE_MAGIC for validation
    uint32_t length;      // total frame size including this header
    uint32_t sequence;    // sequence number for ordering
    uint32_t sourceId;    // writer identity (0 = main thread, 1+ = workers)
    // payload follows (binary data — OSC or text depending on buffer)
};

constexpr uint32_t MESSAGE_MAGIC = 0xDEADBEEF;
constexpr uint32_t PADDING_MAGIC = 0xBADDCAFE;  // end-of-ring pad marker; frame restarts at offset 0

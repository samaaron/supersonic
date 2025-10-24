/*
    SuperSonic - SuperCollider AudioWorklet WebAssembly port
    Copyright (c) 2025 Sam Aaron

    Based on SuperCollider by James McCartney and community
    GPL v3 or later
*/

#ifndef SCSYNTH_SHARED_MEMORY_H
#define SCSYNTH_SHARED_MEMORY_H

#include <atomic>
#include <cstdint>

// Memory layout constants (matching JS side)
// NOTE: These are RELATIVE OFFSETS from the ring buffer base address.
// Actual addresses are calculated as: base_address + offset
// The base address is determined by WASM linker (static buffer in data segment)
// and retrieved via get_ring_buffer_base() at runtime.
#define IN_BUFFER_START    0
#define IN_BUFFER_SIZE     8192
#define OUT_BUFFER_START   8192
#define OUT_BUFFER_SIZE    8192
#define DEBUG_BUFFER_START 16384
#define DEBUG_BUFFER_SIZE  4096
#define CONTROL_START      20480
#define METRICS_START      20512

// Message structure
struct alignas(4) Message {
    uint32_t magic;       // 0xDEADBEEF for validation
    uint32_t length;      // Total message size including header
    uint32_t type;        // Message type (OSC=1, DEBUG=2)
    uint32_t sequence;    // Sequence number for ordering
    // payload follows (OSC binary data for type=1, UTF-8 text for type=2)
};

// Control pointers structure (4-byte aligned for atomics)
struct alignas(4) ControlPointers {
    std::atomic<int32_t> in_head;
    std::atomic<int32_t> in_tail;
    std::atomic<int32_t> out_head;
    std::atomic<int32_t> out_tail;
    std::atomic<int32_t> debug_head;
    std::atomic<int32_t> debug_tail;
    std::atomic<int32_t> sequence;
    std::atomic<uint32_t> status_flags;
};

// Performance metrics structure
struct alignas(4) PerformanceMetrics {
    std::atomic<uint32_t> process_count;
    std::atomic<uint32_t> buffer_overruns;
    std::atomic<uint32_t> messages_processed;
    std::atomic<uint32_t> messages_dropped;
};

// Status flags
enum StatusFlags : uint32_t {
    STATUS_OK = 0,
    STATUS_BUFFER_FULL = 1 << 0,
    STATUS_OVERRUN = 1 << 1,
    STATUS_WASM_ERROR = 1 << 2,
    STATUS_FRAGMENTED_MSG = 1 << 3
};

// Constants
const uint32_t MAX_MESSAGE_SIZE = 8192 - sizeof(Message);
const uint32_t MESSAGE_MAGIC = 0xDEADBEEF;
const uint32_t PADDING_MAGIC = 0xBADDCAFE;  // Marks padding at end of buffer (OSC buffers)
const uint8_t DEBUG_PADDING_MARKER = 0xFF;  // Marks padding at end of debug buffer (skip to position 0)

#endif // SCSYNTH_SHARED_MEMORY_H

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

// ============================================================================
// BUFFER LAYOUT CONFIGURATION
// ============================================================================
// Memory layout constants for shared ring buffer.
// NOTE: These are RELATIVE OFFSETS from the ring buffer base address.
// Actual addresses are calculated as: base_address + offset
// The base address is determined by WASM linker (static buffer in data segment)
// and retrieved via get_ring_buffer_base() at runtime.
//
// TO MODIFY BUFFER SIZES: Change the SIZE constants below.
// All offsets are calculated automatically using constexpr.
// ============================================================================

// User-configurable buffer sizes
constexpr uint32_t IN_BUFFER_SIZE     = 32768; // OSC messages from JS to scsynth
constexpr uint32_t OUT_BUFFER_SIZE    = 8192;  // OSC replies from scsynth to JS
constexpr uint32_t DEBUG_BUFFER_SIZE  = 4096;  // Debug messages from scsynth
constexpr uint32_t CONTROL_SIZE       = 32;    // Atomic control pointers & flags
constexpr uint32_t METRICS_SIZE       = 48;    // Performance metrics

// Auto-calculated offsets (DO NOT MODIFY - computed from sizes above)
constexpr uint32_t IN_BUFFER_START    = 0;
constexpr uint32_t OUT_BUFFER_START   = IN_BUFFER_START + IN_BUFFER_SIZE;
constexpr uint32_t DEBUG_BUFFER_START = OUT_BUFFER_START + OUT_BUFFER_SIZE;
constexpr uint32_t CONTROL_START      = DEBUG_BUFFER_START + DEBUG_BUFFER_SIZE;
constexpr uint32_t METRICS_START      = CONTROL_START + CONTROL_SIZE;

// Total buffer size (for validation)
constexpr uint32_t TOTAL_BUFFER_SIZE  = METRICS_START + METRICS_SIZE;

// Message structure
struct alignas(4) Message {
    uint32_t magic;       // 0xDEADBEEF for validation
    uint32_t length;      // Total message size including header
    uint32_t sequence;    // Sequence number for ordering
    uint32_t _padding;    // Padding to maintain 16-byte size for now
    // payload follows (binary data - OSC or text depending on buffer)
};

// Control pointers structure (4-byte aligned for atomics)
struct alignas(4) ControlPointers {
    std::atomic<int32_t> in_head;
    std::atomic<int32_t> in_tail;
    std::atomic<int32_t> out_head;
    std::atomic<int32_t> out_tail;
    std::atomic<int32_t> debug_head;
    std::atomic<int32_t> debug_tail;
    std::atomic<int32_t> out_sequence;    // Sequence counter for OUT buffer
    std::atomic<int32_t> debug_sequence;  // Sequence counter for DEBUG buffer
    std::atomic<uint32_t> status_flags;
};

// Performance metrics structure
struct alignas(4) PerformanceMetrics {
    std::atomic<uint32_t> process_count;
    std::atomic<uint32_t> buffer_overruns;
    std::atomic<uint32_t> messages_processed;
    std::atomic<uint32_t> messages_dropped;
    std::atomic<uint32_t> scheduler_queue_depth;
    std::atomic<uint32_t> scheduler_queue_max;
    std::atomic<uint32_t> scheduler_queue_dropped;
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
constexpr uint32_t MAX_MESSAGE_SIZE = IN_BUFFER_SIZE - sizeof(Message);
constexpr uint32_t MESSAGE_MAGIC = 0xDEADBEEF;
constexpr uint32_t PADDING_MAGIC = 0xBADDCAFE;  // Marks padding at end of buffer (OSC buffers)
constexpr uint8_t DEBUG_PADDING_MARKER = 0xFF;  // Marks padding at end of debug buffer (skip to position 0)

// ============================================================================
// BUFFER LAYOUT EXPORT (for JavaScript)
// ============================================================================
// This structure is exported to JavaScript via get_buffer_layout()
// JavaScript can read these values at initialization time to stay in sync
// with the C++ memory layout.
struct BufferLayout {
    uint32_t in_buffer_start;
    uint32_t in_buffer_size;
    uint32_t out_buffer_start;
    uint32_t out_buffer_size;
    uint32_t debug_buffer_start;
    uint32_t debug_buffer_size;
    uint32_t control_start;
    uint32_t control_size;
    uint32_t metrics_start;
    uint32_t metrics_size;
    uint32_t total_buffer_size;
    uint32_t max_message_size;
    uint32_t message_magic;
    uint32_t padding_magic;
    uint8_t debug_padding_marker;
    uint8_t _padding[3];  // Align to 4 bytes
};

// Compile-time constant for the buffer layout
constexpr BufferLayout BUFFER_LAYOUT = {
    IN_BUFFER_START,
    IN_BUFFER_SIZE,
    OUT_BUFFER_START,
    OUT_BUFFER_SIZE,
    DEBUG_BUFFER_START,
    DEBUG_BUFFER_SIZE,
    CONTROL_START,
    CONTROL_SIZE,
    METRICS_START,
    METRICS_SIZE,
    TOTAL_BUFFER_SIZE,
    MAX_MESSAGE_SIZE,
    MESSAGE_MAGIC,
    PADDING_MAGIC,
    DEBUG_PADDING_MARKER,
    {0, 0, 0}  // padding
};

#endif // SCSYNTH_SHARED_MEMORY_H

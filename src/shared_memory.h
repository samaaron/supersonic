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
// Balanced to prevent message drops in audioworklet
constexpr uint32_t IN_BUFFER_SIZE     = 786432; // 768KB - OSC messages from JS to scsynth (large for SynthDefs)
constexpr uint32_t OUT_BUFFER_SIZE    = 131072; // 128KB - OSC replies from scsynth to JS (prevent drops)
constexpr uint32_t DEBUG_BUFFER_SIZE  = 65536;  // 64KB - Debug messages from scsynth
constexpr uint32_t CONTROL_SIZE       = 40;    // Atomic control pointers & flags (36 bytes + 4 padding for 8-byte alignment)
constexpr uint32_t METRICS_SIZE       = 128;   // Performance metrics: 26 fields + 6 padding = 32 * 4 bytes = 128 bytes
constexpr uint32_t NTP_START_TIME_SIZE = 8;    // NTP time when AudioContext started (double, 8-byte aligned, write-once)
constexpr uint32_t DRIFT_OFFSET_SIZE = 4;      // Drift offset in milliseconds (int32, atomic)
constexpr uint32_t GLOBAL_OFFSET_SIZE = 4;     // Global timing offset in milliseconds (int32, atomic) - for multi-system sync (Ableton Link, NTP, etc.)

// Auto-calculated offsets (DO NOT MODIFY - computed from sizes above)
constexpr uint32_t IN_BUFFER_START    = 0;
constexpr uint32_t OUT_BUFFER_START   = IN_BUFFER_START + IN_BUFFER_SIZE;
constexpr uint32_t DEBUG_BUFFER_START = OUT_BUFFER_START + OUT_BUFFER_SIZE;
constexpr uint32_t CONTROL_START      = DEBUG_BUFFER_START + DEBUG_BUFFER_SIZE;
constexpr uint32_t METRICS_START      = CONTROL_START + CONTROL_SIZE;
constexpr uint32_t NTP_START_TIME_START = METRICS_START + METRICS_SIZE;
constexpr uint32_t DRIFT_OFFSET_START = NTP_START_TIME_START + NTP_START_TIME_SIZE;
constexpr uint32_t GLOBAL_OFFSET_START = DRIFT_OFFSET_START + DRIFT_OFFSET_SIZE;

// Total buffer size (for validation)
constexpr uint32_t TOTAL_BUFFER_SIZE  = GLOBAL_OFFSET_START + GLOBAL_OFFSET_SIZE;

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
// Layout: [0-5] Worklet, [6-16] OSC Out, [17-20] OSC In, [21-24] Debug, [25] Main thread, [26-31] padding
struct alignas(4) PerformanceMetrics {
    // Worklet metrics (offsets 0-5, written by WASM)
    std::atomic<uint32_t> process_count;
    std::atomic<uint32_t> messages_processed;
    std::atomic<uint32_t> messages_dropped;
    std::atomic<uint32_t> scheduler_queue_depth;
    std::atomic<uint32_t> scheduler_queue_max;
    std::atomic<uint32_t> scheduler_queue_dropped;

    // OSC Out (prescheduler) metrics (offsets 6-16, written by osc_out_prescheduler_worker.js)
    std::atomic<uint32_t> osc_out_events_pending;
    std::atomic<uint32_t> osc_out_max_events_pending;
    std::atomic<uint32_t> osc_out_bundles_written;
    std::atomic<uint32_t> osc_out_retries_succeeded;
    std::atomic<uint32_t> osc_out_retries_failed;
    std::atomic<uint32_t> osc_out_bundles_scheduled;
    std::atomic<uint32_t> osc_out_events_cancelled;
    std::atomic<uint32_t> osc_out_total_dispatches;
    std::atomic<uint32_t> osc_out_messages_retried;
    std::atomic<uint32_t> osc_out_retry_queue_size;
    std::atomic<uint32_t> osc_out_retry_queue_max;

    // OSC In metrics (offsets 17-20, written by osc_in_worker.js)
    std::atomic<uint32_t> osc_in_messages_received;
    std::atomic<uint32_t> osc_in_dropped_messages;
    std::atomic<uint32_t> osc_in_wakeups;
    std::atomic<uint32_t> osc_in_timeouts;

    // Debug metrics (offsets 21-24, written by debug_worker.js)
    std::atomic<uint32_t> debug_messages_received;
    std::atomic<uint32_t> debug_wakeups;
    std::atomic<uint32_t> debug_timeouts;
    std::atomic<uint32_t> debug_bytes_read;

    // Main thread metrics (offset 25, written by supersonic.js via Atomics)
    std::atomic<uint32_t> messages_sent;      // OSC messages sent to scsynth

    // Padding to ensure 8-byte alignment for subsequent Float64Array fields (offsets 26-31)
    uint32_t _padding[6];
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
    uint32_t ntp_start_time_start;
    uint32_t ntp_start_time_size;
    uint32_t drift_offset_start;
    uint32_t drift_offset_size;
    uint32_t global_offset_start;
    uint32_t global_offset_size;
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
    NTP_START_TIME_START,
    NTP_START_TIME_SIZE,
    DRIFT_OFFSET_START,
    DRIFT_OFFSET_SIZE,
    GLOBAL_OFFSET_START,
    GLOBAL_OFFSET_SIZE,
    TOTAL_BUFFER_SIZE,
    MAX_MESSAGE_SIZE,
    MESSAGE_MAGIC,
    PADDING_MAGIC,
    DEBUG_PADDING_MARKER,
    {0, 0, 0}  // padding
};

#endif // SCSYNTH_SHARED_MEMORY_H

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
constexpr uint32_t CONTROL_SIZE       = 48;    // Atomic control pointers & flags (11 fields × 4 bytes + 4 padding for 8-byte alignment)
constexpr uint32_t METRICS_SIZE       = 128;   // Performance metrics: 22 fields + 10 padding = 32 * 4 bytes = 128 bytes
constexpr uint32_t NTP_START_TIME_SIZE = 8;    // NTP time when AudioContext started (double, 8-byte aligned, write-once)
constexpr uint32_t DRIFT_OFFSET_SIZE = 4;      // Drift offset in milliseconds (int32, atomic)
constexpr uint32_t GLOBAL_OFFSET_SIZE = 4;     // Global timing offset in milliseconds (int32, atomic) - for multi-system sync (Ableton Link, NTP, etc.)

// Node tree configuration (for observing synth/group hierarchy via polling)
constexpr uint32_t NODE_TREE_MAX_NODES = 1024; // Max nodes in tree
constexpr uint32_t NODE_TREE_HEADER_SIZE = 8;  // node_count (4) + version (4)
constexpr uint32_t NODE_TREE_DEF_NAME_SIZE = 32; // Max synthdef name length (including null terminator)
constexpr uint32_t NODE_TREE_ENTRY_SIZE = 56;  // 6 x int32 (24) + def_name (32) = 56 bytes per entry
constexpr uint32_t NODE_TREE_SIZE = NODE_TREE_HEADER_SIZE + (NODE_TREE_MAX_NODES * NODE_TREE_ENTRY_SIZE); // ~57KB

// Audio capture configuration (for testing - captures audio output to SharedArrayBuffer)
// 1 second at 48kHz stereo = 96000 samples * 4 bytes = ~375KB
constexpr uint32_t AUDIO_CAPTURE_SAMPLE_RATE = 48000;
constexpr uint32_t AUDIO_CAPTURE_CHANNELS = 2;
constexpr uint32_t AUDIO_CAPTURE_SECONDS = 1;
constexpr uint32_t AUDIO_CAPTURE_FRAMES = AUDIO_CAPTURE_SAMPLE_RATE * AUDIO_CAPTURE_SECONDS;
constexpr uint32_t AUDIO_CAPTURE_HEADER_SIZE = 16; // enabled (4) + head (4) + sample_rate (4) + channels (4)
constexpr uint32_t AUDIO_CAPTURE_DATA_SIZE = AUDIO_CAPTURE_FRAMES * AUDIO_CAPTURE_CHANNELS * sizeof(float);
constexpr uint32_t AUDIO_CAPTURE_SIZE = AUDIO_CAPTURE_HEADER_SIZE + AUDIO_CAPTURE_DATA_SIZE;

// Auto-calculated offsets (DO NOT MODIFY - computed from sizes above)
constexpr uint32_t IN_BUFFER_START    = 0;
constexpr uint32_t OUT_BUFFER_START   = IN_BUFFER_START + IN_BUFFER_SIZE;
constexpr uint32_t DEBUG_BUFFER_START = OUT_BUFFER_START + OUT_BUFFER_SIZE;
constexpr uint32_t CONTROL_START      = DEBUG_BUFFER_START + DEBUG_BUFFER_SIZE;
constexpr uint32_t METRICS_START      = CONTROL_START + CONTROL_SIZE;
constexpr uint32_t NTP_START_TIME_START = METRICS_START + METRICS_SIZE;
constexpr uint32_t DRIFT_OFFSET_START = NTP_START_TIME_START + NTP_START_TIME_SIZE;
constexpr uint32_t GLOBAL_OFFSET_START = DRIFT_OFFSET_START + DRIFT_OFFSET_SIZE;
constexpr uint32_t NODE_TREE_START = GLOBAL_OFFSET_START + GLOBAL_OFFSET_SIZE;
constexpr uint32_t AUDIO_CAPTURE_START = NODE_TREE_START + NODE_TREE_SIZE;

// Total buffer size (for validation)
constexpr uint32_t TOTAL_BUFFER_SIZE  = AUDIO_CAPTURE_START + AUDIO_CAPTURE_SIZE;

// Message structure
struct alignas(4) Message {
    uint32_t magic;       // 0xDEADBEEF for validation
    uint32_t length;      // Total message size including header
    uint32_t sequence;    // Sequence number for ordering
    uint32_t _padding;    // Padding to maintain 16-byte size for now
    // payload follows (binary data - OSC or text depending on buffer)
};

// Control pointers structure (4-byte aligned for atomics, padded to 48 bytes for 8-byte alignment)
struct alignas(4) ControlPointers {
    std::atomic<int32_t> in_head;
    std::atomic<int32_t> in_tail;
    std::atomic<int32_t> out_head;
    std::atomic<int32_t> out_tail;
    std::atomic<int32_t> debug_head;
    std::atomic<int32_t> debug_tail;
    std::atomic<int32_t> in_sequence;     // Sequence counter for IN buffer (shared between main thread & worker)
    std::atomic<int32_t> out_sequence;    // Sequence counter for OUT buffer
    std::atomic<int32_t> debug_sequence;  // Sequence counter for DEBUG buffer
    std::atomic<uint32_t> status_flags;
    std::atomic<int32_t> in_write_lock;   // Spinlock for IN buffer writes (0=unlocked, 1=locked)
    int32_t _padding;                     // Padding to maintain 8-byte alignment for subsequent Float64
};

// Performance metrics structure
// Layout: [0-5] Worklet, [6-16] OSC Out, [17-19] OSC In, [20-21] Debug, [22-23] Main thread, [24] Gap detection, [25] Direct writes, [26-31] padding
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

    // OSC In metrics (offsets 17-19, written by osc_in_worker.js)
    std::atomic<uint32_t> osc_in_messages_received;
    std::atomic<uint32_t> osc_in_dropped_messages;
    std::atomic<uint32_t> osc_in_bytes_received;  // Total bytes read from OUT buffer

    // Debug metrics (offsets 20-21, written by debug_worker.js)
    std::atomic<uint32_t> debug_messages_received;
    std::atomic<uint32_t> debug_bytes_received;   // Total bytes read from DEBUG buffer

    // Main thread metrics (offsets 22-23, written by supersonic.js via Atomics)
    std::atomic<uint32_t> messages_sent;      // OSC messages sent to scsynth
    std::atomic<uint32_t> bytes_sent;         // Total bytes written to IN buffer

    // Sequence gap detection (offset 24, written by WASM)
    std::atomic<uint32_t> messages_sequence_gaps;  // Count of detected sequence gaps (missing messages)

    // Direct write metrics (offset 25, written by supersonic.js main thread)
    std::atomic<uint32_t> direct_writes;  // Messages that bypassed prescheduler worker

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

// Node tree header (at NODE_TREE_START offset in ring_buffer_storage)
// Written by WASM, read by JS for polling
struct alignas(4) NodeTreeHeader {
    std::atomic<uint32_t> node_count;  // Number of active nodes
    std::atomic<uint32_t> version;     // Incremented on each change (for change detection)
};

// Node entry in the tree (56 bytes = 6 x int32 + 32-byte def_name)
// Array follows NodeTreeHeader at NODE_TREE_START + NODE_TREE_HEADER_SIZE
struct alignas(4) NodeEntry {
    int32_t id;         // Node ID (-1 = empty slot)
    int32_t parent_id;  // Parent group ID (-1 for root)
    int32_t is_group;   // 1 = group, 0 = synth
    int32_t prev_id;    // Previous sibling (-1 if first)
    int32_t next_id;    // Next sibling (-1 if last)
    int32_t head_id;    // For groups: first child (-1 if empty or if synth)
    char def_name[NODE_TREE_DEF_NAME_SIZE]; // Synthdef name for synths, "group" for groups
};

// Audio capture header (at AUDIO_CAPTURE_START offset in ring_buffer_storage)
// Written by WASM when capture is enabled, read by JS to retrieve captured audio
struct alignas(4) AudioCaptureHeader {
    std::atomic<uint32_t> enabled;      // 0 = disabled, 1 = enabled (JS writes to start/stop)
    std::atomic<uint32_t> head;         // Write position in frames (WASM writes)
    uint32_t sample_rate;               // Actual sample rate (set by WASM on init)
    uint32_t channels;                  // Number of channels (2 for stereo)
    // Audio data follows: float[AUDIO_CAPTURE_FRAMES * AUDIO_CAPTURE_CHANNELS]
};

// Constants
constexpr uint32_t MAX_MESSAGE_SIZE = IN_BUFFER_SIZE - sizeof(Message);
constexpr uint32_t MESSAGE_MAGIC = 0xDEADBEEF;
constexpr uint32_t PADDING_MAGIC = 0xBADDCAFE;  // Marks padding at end of buffer (OSC buffers)
constexpr uint8_t DEBUG_PADDING_MARKER = 0xFF;  // Marks padding at end of debug buffer (skip to position 0)

// Scheduler configuration - can be overridden via -D flags at compile time
// These must match the values in BundleScheduler.h
#ifndef SCHEDULER_SLOT_SIZE
#define SCHEDULER_SLOT_SIZE 1024
#endif

#ifndef SCHEDULER_SLOT_COUNT
#define SCHEDULER_SLOT_COUNT 512
#endif

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
    uint32_t node_tree_start;
    uint32_t node_tree_size;
    uint32_t node_tree_header_size;
    uint32_t node_tree_entry_size;
    uint32_t node_tree_def_name_size;
    uint32_t node_tree_max_nodes;
    uint32_t audio_capture_start;
    uint32_t audio_capture_size;
    uint32_t audio_capture_header_size;
    uint32_t audio_capture_frames;
    uint32_t audio_capture_channels;
    uint32_t audio_capture_sample_rate;
    uint32_t total_buffer_size;
    uint32_t max_message_size;
    uint32_t message_magic;
    uint32_t padding_magic;
    uint32_t scheduler_slot_size;
    uint32_t scheduler_slot_count;
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
    NODE_TREE_START,
    NODE_TREE_SIZE,
    NODE_TREE_HEADER_SIZE,
    NODE_TREE_ENTRY_SIZE,
    NODE_TREE_DEF_NAME_SIZE,
    NODE_TREE_MAX_NODES,
    AUDIO_CAPTURE_START,
    AUDIO_CAPTURE_SIZE,
    AUDIO_CAPTURE_HEADER_SIZE,
    AUDIO_CAPTURE_FRAMES,
    AUDIO_CAPTURE_CHANNELS,
    AUDIO_CAPTURE_SAMPLE_RATE,
    TOTAL_BUFFER_SIZE,
    MAX_MESSAGE_SIZE,
    MESSAGE_MAGIC,
    PADDING_MAGIC,
    SCHEDULER_SLOT_SIZE,
    SCHEDULER_SLOT_COUNT,
    DEBUG_PADDING_MARKER,
    {0, 0, 0}  // padding
};

#endif // SCSYNTH_SHARED_MEMORY_H

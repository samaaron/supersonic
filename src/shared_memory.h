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
constexpr uint32_t METRICS_SIZE       = 184;   // Performance metrics: 46 fields * 4 bytes = 184 bytes
constexpr uint32_t NTP_START_TIME_SIZE = 8;    // NTP time when AudioContext started (double, 8-byte aligned, write-once)
constexpr uint32_t DRIFT_OFFSET_SIZE = 4;      // Drift offset in milliseconds (int32, atomic)
constexpr uint32_t GLOBAL_OFFSET_SIZE = 4;     // Global timing offset in milliseconds (int32, atomic) - for multi-system sync (Ableton Link, NTP, etc.)

// Node tree mirror configuration (for observing synth/group hierarchy via polling)
// This is a MIRROR of the actual scsynth node tree - the real tree can exceed this limit,
// but only this many nodes will be visible to JavaScript. Audio continues working regardless.
// Can be overridden at build time with -DNODE_TREE_MIRROR_MAX_NODES=N
#ifndef NODE_TREE_MIRROR_MAX_NODES
#define NODE_TREE_MIRROR_MAX_NODES 1024
#endif
constexpr uint32_t NODE_TREE_HEADER_SIZE = 16; // node_count (4) + version (4) + dropped_count (4) + padding (4) for 8-byte alignment
constexpr uint32_t NODE_TREE_DEF_NAME_SIZE = 32; // Max synthdef name length (including null terminator)
constexpr uint32_t NODE_TREE_ENTRY_SIZE = 56;  // 6 x int32 (24) + def_name (32) = 56 bytes per entry
constexpr uint32_t NODE_TREE_SIZE = NODE_TREE_HEADER_SIZE + (NODE_TREE_MIRROR_MAX_NODES * NODE_TREE_ENTRY_SIZE); // ~57KB

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
constexpr uint32_t NODE_TREE_START = METRICS_START + METRICS_SIZE;  // Contiguous with METRICS for efficient postMessage copying
constexpr uint32_t NTP_START_TIME_START = NODE_TREE_START + NODE_TREE_SIZE;
constexpr uint32_t DRIFT_OFFSET_START = NTP_START_TIME_START + NTP_START_TIME_SIZE;
constexpr uint32_t GLOBAL_OFFSET_START = DRIFT_OFFSET_START + DRIFT_OFFSET_SIZE;
constexpr uint32_t AUDIO_CAPTURE_START = GLOBAL_OFFSET_START + GLOBAL_OFFSET_SIZE;

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
// Layout designed for contiguous memcpy operations:
// - [0-8]   scsynth (WASM + JS worklet writes)
// - [9-23]  Prescheduler (JS prescheduler worker - all contiguous for single memcpy overlay)
// - [24-25] OSC Out (JS main thread)
// - [26-29] OSC In (JS osc_in_worker)
// - [30-31] Debug (JS debug_worker)
// - [32-34] Ring buffer usage (WASM writes)
// - [35-37] Ring buffer peak usage (WASM writes)
// - [38-41] Bypass category metrics (JS main thread / PM transport)
// - [42-44] scsynth late timing diagnostics (WASM writes)
// - [45]    padding
struct alignas(4) PerformanceMetrics {
    // scsynth metrics [0-8] (offsets 0-6,8 written by WASM, 7 by JS worklet)
    std::atomic<uint32_t> process_count;           // 0: Audio process() callbacks
    std::atomic<uint32_t> messages_processed;      // 1: OSC messages processed
    std::atomic<uint32_t> messages_dropped;        // 2: Messages dropped
    std::atomic<uint32_t> scheduler_queue_depth;   // 3: Current scheduler depth
    std::atomic<uint32_t> scheduler_queue_max;     // 4: Peak scheduler depth
    std::atomic<uint32_t> scheduler_queue_dropped; // 5: Scheduler overflow drops
    std::atomic<uint32_t> messages_sequence_gaps;  // 6: Sequence gaps detected (WASM)
    std::atomic<uint32_t> wasm_errors;             // 7: WASM execution errors (JS worklet)
    std::atomic<uint32_t> scheduler_lates;         // 8: Bundles executed after scheduled time (WASM)

    // Prescheduler metrics [9-23] (written by JS prescheduler worker - all contiguous)
    std::atomic<uint32_t> prescheduler_pending;           // 9
    std::atomic<uint32_t> prescheduler_pending_peak;      // 10
    std::atomic<uint32_t> prescheduler_bundles_scheduled; // 11
    std::atomic<uint32_t> prescheduler_dispatched;        // 12
    std::atomic<uint32_t> prescheduler_events_cancelled;  // 13
    std::atomic<uint32_t> prescheduler_min_headroom_ms;   // 14: All-time min headroom before execution
    std::atomic<uint32_t> prescheduler_lates;             // 15: Bundles dispatched after execution time
    std::atomic<uint32_t> prescheduler_retries_succeeded; // 16
    std::atomic<uint32_t> prescheduler_retries_failed;    // 17
    std::atomic<uint32_t> prescheduler_retry_queue_size;  // 18
    std::atomic<uint32_t> prescheduler_retry_queue_peak;  // 19
    std::atomic<uint32_t> prescheduler_messages_retried;  // 20
    std::atomic<uint32_t> prescheduler_total_dispatches;  // 21
    std::atomic<uint32_t> prescheduler_bypassed;          // 22
    std::atomic<int32_t> prescheduler_max_late_ms;        // 23: Maximum lateness at prescheduler (ms)

    // OSC Out metrics [24-25] (written by JS main thread)
    std::atomic<uint32_t> osc_out_messages_sent;   // 24
    std::atomic<uint32_t> osc_out_bytes_sent;      // 25

    // OSC In metrics [26-29] (written by JS osc_in_worker)
    std::atomic<uint32_t> osc_in_messages_received; // 26
    std::atomic<uint32_t> osc_in_bytes_received;    // 27
    std::atomic<uint32_t> osc_in_dropped_messages;  // 28
    std::atomic<uint32_t> osc_in_corrupted;         // 29: Ring buffer message corruption detected

    // Debug metrics [30-31] (written by JS debug_worker)
    std::atomic<uint32_t> debug_messages_received;  // 30
    std::atomic<uint32_t> debug_bytes_received;     // 31

    // Ring buffer usage [32-34] (written by WASM during process())
    std::atomic<uint32_t> in_buffer_used_bytes;     // 32: Bytes used in IN buffer
    std::atomic<uint32_t> out_buffer_used_bytes;    // 33: Bytes used in OUT buffer
    std::atomic<uint32_t> debug_buffer_used_bytes;  // 34: Bytes used in DEBUG buffer

    // Ring buffer peak usage [35-37] (written by WASM during process())
    std::atomic<uint32_t> in_buffer_peak_bytes;     // 35: Peak bytes used in IN buffer
    std::atomic<uint32_t> out_buffer_peak_bytes;    // 36: Peak bytes used in OUT buffer
    std::atomic<uint32_t> debug_buffer_peak_bytes;  // 37: Peak bytes used in DEBUG buffer

    // Bypass category metrics [38-41] (written by JS main thread / PM transport)
    std::atomic<uint32_t> bypass_non_bundle;        // 38: Plain OSC messages (not bundles)
    std::atomic<uint32_t> bypass_immediate;         // 39: Bundles with timetag 0 or 1
    std::atomic<uint32_t> bypass_near_future;       // 40: Within lookahead window but not late
    std::atomic<uint32_t> bypass_late;              // 41: Past their scheduled time

    // scsynth late timing diagnostics [42-44] (written by WASM during process())
    std::atomic<int32_t> scheduler_max_late_ms;     // 42: Maximum lateness observed (ms)
    std::atomic<int32_t> scheduler_last_late_ms;    // 43: Most recent late magnitude (ms)
    std::atomic<uint32_t> scheduler_last_late_tick; // 44: Process count when last late occurred

    // Padding [45]
    uint32_t _padding[1];
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
    std::atomic<uint32_t> node_count;    // Number of active nodes in mirror tree
    std::atomic<uint32_t> version;       // Incremented on each change (for change detection)
    std::atomic<uint32_t> dropped_count; // Nodes not mirrored due to overflow (actual tree has more)
    uint32_t _padding;                   // Padding for 8-byte alignment of subsequent Float64 fields
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
    // NODE_TREE is now contiguous with METRICS for efficient postMessage copying
    uint32_t node_tree_start;
    uint32_t node_tree_size;
    uint32_t node_tree_header_size;
    uint32_t node_tree_entry_size;
    uint32_t node_tree_def_name_size;
    uint32_t node_tree_max_nodes;
    uint32_t ntp_start_time_start;
    uint32_t ntp_start_time_size;
    uint32_t drift_offset_start;
    uint32_t drift_offset_size;
    uint32_t global_offset_start;
    uint32_t global_offset_size;
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
    // NODE_TREE now contiguous with METRICS
    NODE_TREE_START,
    NODE_TREE_SIZE,
    NODE_TREE_HEADER_SIZE,
    NODE_TREE_ENTRY_SIZE,
    NODE_TREE_DEF_NAME_SIZE,
    NODE_TREE_MIRROR_MAX_NODES,
    NTP_START_TIME_START,
    NTP_START_TIME_SIZE,
    DRIFT_OFFSET_START,
    DRIFT_OFFSET_SIZE,
    GLOBAL_OFFSET_START,
    GLOBAL_OFFSET_SIZE,
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

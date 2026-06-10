/*
    SuperSonic - SuperCollider AudioWorklet WebAssembly port
    Copyright (c) 2025 Sam Aaron

    Based on SuperCollider by James McCartney and community
    GPL v3 or later
*/

#ifndef SCSYNTH_SHARED_MEMORY_H
#define SCSYNTH_SHARED_MEMORY_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "memory_profile.h"
#include "scsynth/common/shm_audio_buffer.hpp"

namespace supersonic {

// Double ↔ uint64 bit-pattern conversion. Centralised so the C++ mirror of
// SuperClockState (and any future SAB struct storing a double in a 64-bit
// atomic) has one bit-cast spelling. Compiles to the same code as
// std::bit_cast on any recent compiler; uses memcpy so it works under
// C++17 (the emcc default here) as well as C++20.
inline uint64_t doubleToBits(double v) {
    uint64_t bits;
    std::memcpy(&bits, &v, sizeof(double));
    return bits;
}

inline double bitsToDouble(uint64_t bits) {
    double v;
    std::memcpy(&v, &bits, sizeof(double));
    return v;
}

}  // namespace supersonic

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

// User-configurable buffer sizes.
// Balanced to prevent message drops in the audioworklet; sized per device via
// memory_profile.h (defaults: IN 768KB, OUT 128KB, NRT-out 64KB).
constexpr uint32_t IN_BUFFER_SIZE     = SUPERSONIC_IN_BUFFER_SIZE;    // OSC messages from host to scsynth (large for SynthDefs)
constexpr uint32_t OUT_BUFFER_SIZE    = SUPERSONIC_OUT_BUFFER_SIZE;   // OSC replies from scsynth to host (prevent drops)
constexpr uint32_t NRT_OUT_BUFFER_SIZE  = SUPERSONIC_NRT_OUT_BUFFER_SIZE; // NRT-thread egress ring (replies, notifications, debug)
constexpr uint32_t CONTROL_SIZE       = 48;    // Atomic control pointers & flags (11 fields × 4 bytes + 4 padding for 8-byte alignment)
constexpr uint32_t METRICS_SIZE       = 208;   // Performance metrics: 52 fields * 4 bytes = 208 bytes (multiple of 8)
constexpr uint32_t NTP_START_TIME_SIZE = 8;    // NTP time when AudioContext started (double, 8-byte aligned, write-once)
constexpr uint32_t DRIFT_OFFSET_SIZE = 4;      // Drift offset in microseconds (int32, atomic)
constexpr uint32_t GLOBAL_OFFSET_SIZE = 4;     // Global timing offset in milliseconds (int32, atomic) - for multi-system sync (Ableton Link, NTP, etc.)
constexpr uint32_t SUPERCLOCK_STATE_SIZE = 32; // SuperClock session state: 3 atomic uint64 + 1 atomic uint32 + 4 padding = 32 bytes

// Node tree mirror configuration (for observing synth/group hierarchy via polling)
// This is a MIRROR of the actual scsynth node tree - the real tree can exceed this limit,
// but only this many nodes will be visible to JavaScript. Audio continues working regardless.
// Sized per device via memory_profile.h (default 1024); override at build time
// with -DNODE_TREE_MIRROR_MAX_NODES=N or a device profile.
constexpr uint32_t NODE_TREE_HEADER_SIZE = 16; // node_count (4) + version (4) + dropped_count (4) + padding (4) for 8-byte alignment
constexpr uint32_t NODE_TREE_DEF_NAME_SIZE = 32; // Max synthdef name length (including null terminator)
constexpr uint32_t NODE_TREE_ENTRY_SIZE = 72;  // 6 x int32 (24) + def_name (32) + uuid_hi (8) + uuid_lo (8) = 72 bytes per entry
constexpr uint32_t NODE_TREE_SIZE = NODE_TREE_HEADER_SIZE + (NODE_TREE_MIRROR_MAX_NODES * NODE_TREE_ENTRY_SIZE); // ~57KB

// Audio buffer multi-slot ring. Struct, writer, and reader live in
// scsynth/common/shm_audio_buffer.hpp. Slot 0 carries the master output
// mix; slots 1..N-1 are written by AudioOut2 UGens. Per-slot ring
// duration is controlled by SUPERSONIC_SHM_AUDIO_SECONDS (1s in
// production, larger in test builds so captures don't wrap).

// Auto-calculated offsets (DO NOT MODIFY - computed from sizes above)
constexpr uint32_t IN_BUFFER_START    = 0;
constexpr uint32_t OUT_BUFFER_START   = IN_BUFFER_START + IN_BUFFER_SIZE;
constexpr uint32_t NRT_OUT_BUFFER_START = OUT_BUFFER_START + OUT_BUFFER_SIZE;
constexpr uint32_t CONTROL_START      = NRT_OUT_BUFFER_START + NRT_OUT_BUFFER_SIZE;
constexpr uint32_t METRICS_START      = CONTROL_START + CONTROL_SIZE;
constexpr uint32_t NODE_TREE_START = METRICS_START + METRICS_SIZE;  // Contiguous with METRICS for efficient postMessage copying
constexpr uint32_t NTP_START_TIME_START = NODE_TREE_START + NODE_TREE_SIZE;
constexpr uint32_t DRIFT_OFFSET_START = NTP_START_TIME_START + NTP_START_TIME_SIZE;
constexpr uint32_t GLOBAL_OFFSET_START = DRIFT_OFFSET_START + DRIFT_OFFSET_SIZE;
constexpr uint32_t SUPERCLOCK_STATE_START = GLOBAL_OFFSET_START + GLOBAL_OFFSET_SIZE;
// shm_audio_buffer is alignas(16); round up so slots stay aligned as
// preceding region sizes change.
constexpr uint32_t SHM_AUDIO_START       =
    (SUPERCLOCK_STATE_START + SUPERCLOCK_STATE_SIZE + 15u) & ~15u;
static_assert(SHM_AUDIO_START % 16 == 0,
              "SHM_AUDIO_START must be 16-byte aligned for shm_audio_buffer");
constexpr uint32_t NODE_ID_COUNTER_SIZE  = 4;     // Int32, atomic — for nextNodeId() range allocation
constexpr uint32_t NODE_ID_COUNTER_START = SHM_AUDIO_START + SHM_AUDIO_TOTAL_SIZE;

// World options (native only) — 18 x uint32 written by initialiseWorld(), read by init_memory().
// MUST live outside the IN ring buffer (offsets 0..786431) to survive OSC traffic.
constexpr uint32_t WORLD_OPTIONS_SIZE  = 18 * sizeof(uint32_t);  // 72 bytes
constexpr uint32_t WORLD_OPTIONS_START = NODE_ID_COUNTER_START + NODE_ID_COUNTER_SIZE;

// Scope buffers — per-bus audio scope data for visualisation.
// Uses the same triple-buffer algorithm as native shm_scope_buffer.hpp
// but with fixed layout in the SAB (no TLSF pool, no relative_ptr).
//
// Each scope slot: header (16B) + 3 triple-buffer regions of audio data.
// ScopeOut2 UGen writes here; main thread reads via getScope(n).
// In PM mode, scope snapshots are included in the heartbeat postMessage.
// SHM_SCOPE_MAX_SCOPES / SHM_SCOPE_FRAMES_PER_SCOPE are sized per device via
// memory_profile.h (defaults 32 / 1024).
constexpr uint32_t SHM_SCOPE_CHANNELS = 2;  // Always stereo (ScopeOut2 writes 2 channels)
constexpr uint32_t SHM_SCOPE_HEADER_SIZE = 32;  // Global header (16-aligned)
constexpr uint32_t SHM_SCOPE_SLOT_HEADER_SIZE = 16;  // Per-slot metadata
constexpr uint32_t SHM_SCOPE_REGION_SIZE = SHM_SCOPE_FRAMES_PER_SCOPE * SHM_SCOPE_CHANNELS * sizeof(float);  // One triple-buffer region
constexpr uint32_t SCOPE_SLOT_DATA_SIZE = 3 * SHM_SCOPE_REGION_SIZE;  // Triple buffer (3 regions)
constexpr uint32_t SHM_SCOPE_SLOT_SIZE = SHM_SCOPE_SLOT_HEADER_SIZE + SCOPE_SLOT_DATA_SIZE;  // ~24KB per slot
constexpr uint32_t SHM_SCOPE_TOTAL_SIZE = SHM_SCOPE_HEADER_SIZE + (SHM_SCOPE_MAX_SCOPES * SHM_SCOPE_SLOT_SIZE);

constexpr uint32_t SHM_SCOPE_START = WORLD_OPTIONS_START + WORLD_OPTIONS_SIZE;

// Scope header layout (at SHM_SCOPE_START):
//   [0..3]   u32  maxScopes
//   [4..7]   u32  activeCount
//   [8..11]  u32  framesPerScope
//   [12..15] u32  version (increments on scope add/remove)
//   [16..31] reserved
//
// Per-scope slot (at SHM_SCOPE_START + SHM_SCOPE_HEADER_SIZE + index * SHM_SCOPE_SLOT_SIZE):
//   [0..3]   u32  state (0=free, 1=active)
//   [4..7]   u32  channels
//   [8..11]  i32  stage (atomic: triple-buffer swap index 0/1/2)
//   [12..15] u32  _in (writer's current triple-buffer region index)
//   [16..]   float data[3][framesPerScope * channels]  — triple-buffered audio

// Native-only live engine stats (loaded synthdef count, allocated sample
// buffers + their bytes). Appended at the very END of the layout on purpose:
// adding it shifts NO existing offset, so the WASM metric layout / JS offset
// map is completely untouched (the web build never reads this region — it has
// its own JS-context equivalents). The native engine writes it; the GUI reads
// it via the self-describing segment header.
constexpr uint32_t NATIVE_STATS_SIZE  = 16;  // u32 x4: synthdefs, buffers, buffer_bytes, reserved
constexpr uint32_t NATIVE_STATS_START = SHM_SCOPE_START + SHM_SCOPE_TOTAL_SIZE;
// Field byte offsets within the native-stats region.
constexpr uint32_t NATIVE_STAT_SYNTHDEFS    = 0;
constexpr uint32_t NATIVE_STAT_BUFFERS      = 4;
constexpr uint32_t NATIVE_STAT_BUFFER_BYTES = 8;

// Total buffer size (for validation)
constexpr uint32_t TOTAL_BUFFER_SIZE  = NATIVE_STATS_START + NATIVE_STATS_SIZE;

// Message structure
struct alignas(4) Message {
    uint32_t magic;       // 0xDEADBEEF for validation
    uint32_t length;      // Total message size including header
    uint32_t sequence;    // Sequence number for ordering
    uint32_t sourceId;    // Writer identity (0 = main thread, 1+ = workers). Matches JS ring_buffer_core.js header layout.
    // payload follows (binary data - OSC or text depending on buffer)
};

// Egress framing: every frame on both egress rings is
// Message{sourceId = origin token} + [route:u32][osc].
enum EgressRoute : uint32_t {
    EGRESS_REPLY            = 0,  // reply to the origin token
    EGRESS_SEND_TO_CALLER   = 1,  // reply to the origin token, network peers only
    EGRESS_BROADCAST_NOTIFY = 2,  // fan out to all notify subscribers
    EGRESS_BROADCAST_LINK   = 3,  // fan out to all Link subscribers
    EGRESS_BROADCAST_MIDI   = 4,  // fan out to all MIDI-notify subscribers
    EGRESS_BROADCAST_GAMEPAD = 5, // fan out to all gamepad-notify subscribers
};
constexpr uint32_t EGRESS_ROUTE_SIZE = sizeof(uint32_t);  // leading route word

// Control pointers structure (4-byte aligned for atomics, padded to 48 bytes for 8-byte alignment)
struct alignas(4) ControlPointers {
    std::atomic<int32_t> in_head;
    std::atomic<int32_t> in_tail;
    std::atomic<int32_t> out_head;
    std::atomic<int32_t> out_tail;
    std::atomic<int32_t> nrt_out_head;
    std::atomic<int32_t> nrt_out_tail;
    std::atomic<int32_t> in_sequence;     // Sequence counter for IN buffer (shared between main thread & worker)
    std::atomic<int32_t> out_sequence;    // Sequence counter for OUT buffer
    std::atomic<int32_t> nrt_out_sequence;  // Sequence counter for the NRT-out buffer
    std::atomic<uint32_t> status_flags;
    std::atomic<int32_t> in_write_lock;   // Spinlock for IN buffer writes (0=unlocked, 1=locked)
    int32_t _padding;                     // Padding to maintain 8-byte alignment for subsequent Float64
};

// Performance metrics structure
// Layout designed for contiguous memcpy operations:
// - [0-8]   scsynth (WASM + JS worklet writes)
// - [9-10]  OSC Out (JS main thread)
// - [11-14] OSC In (JS osc_in_worker)
// - [15-16] Debug (JS debug_worker)
// - [17-19] Ring buffer usage (WASM writes)
// - [20-22] Ring buffer peak usage (WASM writes)
// - [23-25] scsynth late timing diagnostics (WASM writes)
// - [26]    Ring buffer direct write failures (OscChannel SAB mode, JS-only)
// - [27-38] Link (native-only)
// - [39-45] System info: version + audio config (shared C++, write-once)
// - [46-49] SuperClock readouts: tempo/beat/phase/playing (shared C++, per block)
struct alignas(4) PerformanceMetrics {
    // The same struct is written from native (binary + NIF) and web (WASM/JS).
    // Each group below identifies its writers by runtime. "shared C++" means
    // the writer is in shared source (audio_processor.cpp / scsynth core) and
    // runs on all three runtimes. Fields tagged JS-only stay 0 on native;
    // fields tagged native-only stay 0 on web.

    // scsynth core metrics [0-8]
    // Writers: shared C++ in audio_processor.cpp (runs on all runtimes).
    // Exception: wasm_errors is JS-only (worklet-side WASM exec errors).
    // Exception: messages_sequence_gaps also written by ReplyReader.cpp (native).
    std::atomic<uint32_t> process_count;           // 0: Audio process() callbacks
    std::atomic<uint32_t> messages_processed;      // 1: OSC messages processed
    std::atomic<uint32_t> messages_dropped;        // 2: Messages dropped
    std::atomic<uint32_t> scheduler_queue_depth;   // 3: Current scheduler depth
    std::atomic<uint32_t> scheduler_queue_max;     // 4: Peak scheduler depth
    std::atomic<uint32_t> scheduler_queue_dropped; // 5: Scheduler overflow drops
    std::atomic<uint32_t> messages_sequence_gaps;  // 6: Sequence gaps detected
    std::atomic<uint32_t> wasm_errors;             // 7: WASM execution errors (JS-only)
    std::atomic<uint32_t> scheduler_lates;         // 8: Bundles executed after scheduled time

    // OSC Out metrics [9-10]
    // Writers: native — OscUdpServer.cpp (handlePacket); web — sab_transport.js.
    std::atomic<uint32_t> osc_out_messages_sent;   // 9
    std::atomic<uint32_t> osc_out_bytes_sent;      // 10

    // OSC In metrics [11-14]
    // Writers: native — src/workers/ReplyReader.cpp; web — js/workers/osc_in_worker.js.
    // osc_in_dropped_messages is JS-only (no native writer).
    std::atomic<uint32_t> osc_in_messages_received; // 11
    std::atomic<uint32_t> osc_in_bytes_received;    // 12
    std::atomic<uint32_t> osc_in_dropped_messages;  // 13 (JS-only)
    std::atomic<uint32_t> osc_in_corrupted;         // 14: Ring buffer message corruption

    // Debug metrics [15-16]
    // Writers: native — src/workers/DebugReader.cpp; web — debug_worker.
    std::atomic<uint32_t> debug_messages_received;  // 15
    std::atomic<uint32_t> debug_bytes_received;     // 16

    // Ring buffer usage [17-19]
    // Writers: shared C++ during process() (audio_processor.cpp / scsynth core).
    std::atomic<uint32_t> in_buffer_used_bytes;     // 17: Bytes used in IN buffer
    std::atomic<uint32_t> out_buffer_used_bytes;    // 18: Bytes used in OUT buffer
    std::atomic<uint32_t> nrt_out_buffer_used_bytes;  // 19: Bytes used in NRT-out buffer

    // Ring buffer peak usage [20-22]
    // Writers: shared C++ during process() (audio_processor.cpp / scsynth core).
    std::atomic<uint32_t> in_buffer_peak_bytes;     // 20: Peak bytes used in IN buffer
    std::atomic<uint32_t> out_buffer_peak_bytes;    // 21: Peak bytes used in OUT buffer
    std::atomic<uint32_t> nrt_out_buffer_peak_bytes;  // 22: Peak bytes used in NRT-out buffer

    // scsynth late timing diagnostics [23-25]
    // Writers: shared C++ during process() (audio_processor.cpp).
    std::atomic<int32_t> scheduler_max_late_ms;     // 23: Maximum lateness observed (ms)
    std::atomic<int32_t> scheduler_last_late_ms;    // 24: Most recent late magnitude (ms)
    std::atomic<uint32_t> scheduler_last_late_tick; // 25: Process count when last late occurred

    // Ring buffer direct write failures [26]
    // Writer: JS-only — OscChannel in SAB mode increments when an optimistic
    // direct ring write fails and the bundle is dropped.
    // C++ side reserves the slot but doesn't read or write it.
    std::atomic<uint32_t> ring_buffer_direct_write_fails; // 26

    // ─── Link (native-only; 0 on WASM) ──────────────────────────────────
    // Clock readouts mirrored from SuperClock each block; floats stored as
    // fixed-point (decoded by the panel's display formats).
    std::atomic<uint32_t> link_peers;              // 27: connected Link peers
    std::atomic<uint32_t> link_tempo_mbpm;         // 28: tempo, milli-BPM (bpm * 1000)
    std::atomic<uint32_t> link_beat_centi;         // 29: beat position * 100
    std::atomic<uint32_t> link_phase_centi;        // 30: phase within quantum * 100
    std::atomic<uint32_t> link_playing;            // 31: transport 0/1

    // ─── Link Audio stream health (native-only; 0 on WASM) ──────────────
    std::atomic<uint32_t> link_audio_in_channels;  // 32: active received channels
    std::atomic<uint32_t> link_audio_stream_rate;  // 33: received stream sample rate (Hz)
    std::atomic<uint32_t> link_audio_underruns;    // 34: receiver queue underrun events
    std::atomic<uint32_t> link_audio_buffered_ms;  // 35: receiver queue depth (ms)
    std::atomic<int32_t>  link_audio_drift_ppm;    // 36: read-rate deviation from 1.0 (ppm, signed)
    std::atomic<uint32_t> link_audio_publish;      // 37: publishing enabled 0/1
    std::atomic<uint32_t> link_audio_sinks;        // 38: active output sinks

    // ─── System info (cross-platform; written by shared C++ on every runtime) ─
    // Engine identity + audio connection details. Written once at init by
    // init_memory() (audio_processor.cpp). Constant for the session.
    std::atomic<uint32_t> supersonic_version_major; // 39: SUPERSONIC_VERSION_MAJOR
    std::atomic<uint32_t> supersonic_version_minor; // 40: SUPERSONIC_VERSION_MINOR
    std::atomic<uint32_t> supersonic_version_patch; // 41: SUPERSONIC_VERSION_PATCH
    std::atomic<uint32_t> audio_sample_rate;        // 42: output sample rate (Hz)
    std::atomic<uint32_t> audio_block_size;         // 43: block size (frames; 128 on web)
    std::atomic<uint32_t> audio_output_channels;    // 44: output bus channels
    std::atomic<uint32_t> audio_input_channels;     // 45: input bus channels

    // ─── SuperClock readouts (cross-platform; written per block) ─────────────
    // Mirrored from SuperClock each block by publishClockMetrics() — sourced
    // from the SuperClockState SAB mirror, so live on web and native alike
    // (independent of Link). Floats stored as fixed-point (see web schema /
    // panel display formats). Supersedes the native-only link_* clock slots.
    std::atomic<uint32_t> clock_tempo_mbpm;         // 46: tempo, milli-BPM (bpm * 1000)
    std::atomic<uint32_t> clock_beat_centi;         // 47: beat position * 100
    std::atomic<uint32_t> clock_phase_centi;        // 48: phase within quantum * 100
    std::atomic<uint32_t> clock_playing;            // 49: transport 0/1

    // Reserved padding so METRICS_SIZE stays a multiple of 8 bytes — the
    // regions that follow in the arena (NTP time, SuperClockState) are
    // 8-byte-aligned and read via Float64/BigInt64 views, which require it.
    // Two words are needed to keep the 51-meaningful-field struct (0-49 plus
    // _metrics_reserved at 50) padded to a multiple of 8.
    std::atomic<uint32_t> _metrics_reserved;        // 50: reserved (alignment)
    std::atomic<uint32_t> _metrics_reserved2;       // 51: reserved (alignment pad)
};

// SuperClock session state. Has its own SAB region because it's engine
// state (load-bearing on WASM for JS↔worklet transport), not observability
// — separating it from PerformanceMetrics keeps that struct honest as a
// dashboard-only surface. Native builds don't use this region; their
// SuperClock owns the same struct as a private member.
//
// Each field is a single 64-bit atomic (doubles stored as IEEE 754 bit-
// pattern). Single-atomic-per-field is enough on its own — no seqlock,
// because no current reader needs multi-field coherence.
struct alignas(8) SuperClockState {
    std::atomic<uint64_t> bpm;                  // 0-7:  BPM as IEEE 754 bit-pattern
    std::atomic<uint64_t> beat_origin_ntp;      // 8-15: NTP seconds as bit-pattern
    std::atomic<uint64_t> is_playing_at_ntp;    // 16-23: NTP seconds as bit-pattern
    std::atomic<uint32_t> is_playing;           // 24-27: 0 = stopped, 1 = playing
    std::atomic<uint32_t> flags;                // 28-31: bit-packed session flags

    static void initDefaults(SuperClockState& s) {
        s.bpm.store(supersonic::doubleToBits(120.0), std::memory_order_relaxed);
        s.beat_origin_ntp.store(0u,                  std::memory_order_relaxed);
        s.is_playing_at_ntp.store(0u,                std::memory_order_relaxed);
        s.is_playing.store(0u,                       std::memory_order_relaxed);
        s.flags.store(0u,                            std::memory_order_relaxed);
    }
};

// Bit positions inside SuperClockState::flags. Single atomic uint32 so
// readers can snapshot all flags in one load; writers use fetch_or /
// fetch_and to mutate individual bits without stomping siblings.
constexpr uint32_t SC_FLAG_LINK_ENABLED         = 1u << 0;
constexpr uint32_t SC_FLAG_START_STOP_SYNC      = 1u << 1;
constexpr uint32_t SC_FLAG_LINK_AUDIO_PUBLISH   = 1u << 2;
static_assert(sizeof(SuperClockState) == SUPERCLOCK_STATE_SIZE,
              "SuperClockState size must match SUPERCLOCK_STATE_SIZE");

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

// Node entry in the tree (72 bytes = 6 x int32 + 32-byte def_name + 2 x uint64 UUID)
// Array follows NodeTreeHeader at NODE_TREE_START + NODE_TREE_HEADER_SIZE
struct alignas(8) NodeEntry {
    int32_t id;         // Node ID (-1 = empty slot)
    int32_t parent_id;  // Parent group ID (-1 for root)
    int32_t is_group;   // 1 = group, 0 = synth
    int32_t prev_id;    // Previous sibling (-1 if first)
    int32_t next_id;    // Next sibling (-1 if last)
    int32_t head_id;    // For groups: first child (-1 if empty or if synth)
    char def_name[NODE_TREE_DEF_NAME_SIZE]; // Synthdef name for synths, "group" for groups
    uint64_t uuid_hi;   // Upper 8 bytes of UUID (0 if node was created with int32 ID)
    uint64_t uuid_lo;   // Lower 8 bytes of UUID (0 if node was created with int32 ID)
};

// Constants
constexpr uint32_t MAX_MESSAGE_SIZE = IN_BUFFER_SIZE - sizeof(Message);
constexpr uint32_t MESSAGE_MAGIC = 0xDEADBEEF;
constexpr uint32_t PADDING_MAGIC = 0xBADDCAFE;  // Marks padding at end of buffer (OSC buffers)
constexpr uint8_t RING_PADDING_MARKER = 0xFF;  // Byte marking ring-buffer padding (skip to position 0 on wrap)

// Scheduler configuration is sized per device via memory_profile.h
// (defaults: SCHEDULER_DATA_POOL_SIZE 512KB, SCHEDULER_SLOT_COUNT 512) and is
// shared with scheduler/BundleScheduler.h, which includes the same profile.

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
    uint32_t nrt_out_buffer_start;
    uint32_t nrt_out_buffer_size;
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
    uint32_t superclock_state_start;
    uint32_t superclock_state_size;
    uint32_t shm_audio_start;
    uint32_t shm_audio_total_size;
    uint32_t shm_audio_header_size;
    uint32_t shm_audio_frames;
    uint32_t shm_audio_channels;
    uint32_t shm_audio_sample_rate;
    uint32_t node_id_counter_start;
    uint32_t node_id_counter_size;
    uint32_t world_options_start;
    uint32_t world_options_size;
    uint32_t shm_scope_start;
    uint32_t shm_scope_total_size;
    uint32_t shm_scope_header_size;
    uint32_t shm_scope_slot_size;
    uint32_t shm_scope_slot_header_size;
    uint32_t shm_scope_region_size;
    uint32_t shm_scope_max_scopes;
    uint32_t shm_scope_frames_per_scope;
    uint32_t shm_scope_channels;
    uint32_t total_buffer_size;
    uint32_t max_message_size;
    uint32_t message_magic;
    uint32_t padding_magic;
    uint32_t scheduler_data_pool_size;
    uint32_t scheduler_slot_count;
    uint8_t ring_padding_marker;
    uint8_t _padding[3];  // Align to 4 bytes
};

// Compile-time constant for the buffer layout
constexpr BufferLayout BUFFER_LAYOUT = {
    IN_BUFFER_START,
    IN_BUFFER_SIZE,
    OUT_BUFFER_START,
    OUT_BUFFER_SIZE,
    NRT_OUT_BUFFER_START,
    NRT_OUT_BUFFER_SIZE,
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
    SUPERCLOCK_STATE_START,
    SUPERCLOCK_STATE_SIZE,
    SHM_AUDIO_START,
    SHM_AUDIO_TOTAL_SIZE,
    SHM_AUDIO_HEADER_SIZE,
    SHM_AUDIO_FRAMES,
    SHM_AUDIO_CHANNELS,
    SHM_AUDIO_SAMPLE_RATE,
    NODE_ID_COUNTER_START,
    NODE_ID_COUNTER_SIZE,
    WORLD_OPTIONS_START,
    WORLD_OPTIONS_SIZE,
    SHM_SCOPE_START,
    SHM_SCOPE_TOTAL_SIZE,
    SHM_SCOPE_HEADER_SIZE,
    SHM_SCOPE_SLOT_SIZE,
    SHM_SCOPE_SLOT_HEADER_SIZE,
    SHM_SCOPE_REGION_SIZE,
    SHM_SCOPE_MAX_SCOPES,
    SHM_SCOPE_FRAMES_PER_SCOPE,
    SHM_SCOPE_CHANNELS,
    TOTAL_BUFFER_SIZE,
    MAX_MESSAGE_SIZE,
    MESSAGE_MAGIC,
    PADDING_MAGIC,
    SCHEDULER_DATA_POOL_SIZE,
    SCHEDULER_SLOT_COUNT,
    RING_PADDING_MARKER,
    {0, 0, 0}  // padding
};

// ─── SAB layout cross-language assertions ──────────────────────────────────
// JS reads these structs via hand-mirrored offset constants in
// js/lib/*. Encode the expected indices here so the build fails if a
// C++ field moves without the JS mirror following. One-directional —
// editing the JS file alone won't trip these.
//
// offsetof on classes containing std::atomic<integral> is conditionally
// supported by C++17/20 but accepted by clang/gcc/MSVC; suppress the
// pedantic warning around the block.

#if defined(__clang__) || defined(__GNUC__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Winvalid-offsetof"
#endif

#define SS_ASSERT_OFFSET(StructName, field, expectedBytes, jsRef)            \
    static_assert(offsetof(StructName, field) == (expectedBytes),            \
                  #StructName "::" #field " offset drifted from " jsRef)

#define SS_ASSERT_METRIC(field, jsIdx)                                       \
    SS_ASSERT_OFFSET(PerformanceMetrics, field,                              \
                     (jsIdx) * sizeof(uint32_t),                             \
                     "js/lib/metrics_offsets.js")

// SuperClockState ↔ js/lib/superclock_protocol.js
SS_ASSERT_OFFSET(SuperClockState, bpm,               0,
                 "js/lib/superclock_protocol.js SC_BPM_I64");
SS_ASSERT_OFFSET(SuperClockState, beat_origin_ntp,   8,
                 "js/lib/superclock_protocol.js SC_BEAT_ORIGIN_NTP_I64");
SS_ASSERT_OFFSET(SuperClockState, is_playing_at_ntp, 16,
                 "js/lib/superclock_protocol.js SC_IS_PLAYING_AT_NTP_I64");
SS_ASSERT_OFFSET(SuperClockState, is_playing,        24,
                 "js/lib/superclock_protocol.js SC_IS_PLAYING_I32");
SS_ASSERT_OFFSET(SuperClockState, flags,             28,
                 "js/lib/superclock_protocol.js SC_FLAGS_I32");

// ControlPointers ↔ js/lib/control_offsets.js (JS exports byte offsets directly)
SS_ASSERT_OFFSET(ControlPointers, in_head,        0,  "js/lib/control_offsets.js IN_HEAD");
SS_ASSERT_OFFSET(ControlPointers, in_tail,        4,  "js/lib/control_offsets.js IN_TAIL");
SS_ASSERT_OFFSET(ControlPointers, out_head,       8,  "js/lib/control_offsets.js OUT_HEAD");
SS_ASSERT_OFFSET(ControlPointers, out_tail,       12, "js/lib/control_offsets.js OUT_TAIL");
SS_ASSERT_OFFSET(ControlPointers, nrt_out_head,     16, "js/lib/control_offsets.js NRT_OUT_HEAD");
SS_ASSERT_OFFSET(ControlPointers, nrt_out_tail,     20, "js/lib/control_offsets.js NRT_OUT_TAIL");
SS_ASSERT_OFFSET(ControlPointers, in_sequence,    24, "js/lib/control_offsets.js IN_SEQUENCE");
SS_ASSERT_OFFSET(ControlPointers, out_sequence,   28, "js/lib/control_offsets.js OUT_SEQUENCE");
SS_ASSERT_OFFSET(ControlPointers, nrt_out_sequence, 32, "js/lib/control_offsets.js NRT_OUT_SEQUENCE");
SS_ASSERT_OFFSET(ControlPointers, status_flags,   36, "js/lib/control_offsets.js STATUS_FLAGS");
SS_ASSERT_OFFSET(ControlPointers, in_write_lock,  40, "js/lib/control_offsets.js IN_WRITE_LOCK");
SS_ASSERT_OFFSET(ControlPointers, _padding,       44, "js/lib/control_offsets.js IN_LOG_TAIL");

// PerformanceMetrics ↔ js/lib/metrics_offsets.js (all fields uint32; JS uses array index)
SS_ASSERT_METRIC(process_count,                   0);
SS_ASSERT_METRIC(messages_processed,              1);
SS_ASSERT_METRIC(messages_dropped,                2);
SS_ASSERT_METRIC(scheduler_queue_depth,           3);
SS_ASSERT_METRIC(scheduler_queue_max,             4);
SS_ASSERT_METRIC(scheduler_queue_dropped,         5);
SS_ASSERT_METRIC(messages_sequence_gaps,          6);
SS_ASSERT_METRIC(wasm_errors,                     7);
SS_ASSERT_METRIC(scheduler_lates,                 8);
SS_ASSERT_METRIC(osc_out_messages_sent,           9);
SS_ASSERT_METRIC(osc_out_bytes_sent,              10);
SS_ASSERT_METRIC(osc_in_messages_received,        11);
SS_ASSERT_METRIC(osc_in_bytes_received,           12);
SS_ASSERT_METRIC(osc_in_dropped_messages,         13);
SS_ASSERT_METRIC(osc_in_corrupted,                14);
SS_ASSERT_METRIC(debug_messages_received,         15);
SS_ASSERT_METRIC(debug_bytes_received,            16);
SS_ASSERT_METRIC(in_buffer_used_bytes,            17);
SS_ASSERT_METRIC(out_buffer_used_bytes,           18);
SS_ASSERT_METRIC(nrt_out_buffer_used_bytes,         19);
SS_ASSERT_METRIC(in_buffer_peak_bytes,            20);
SS_ASSERT_METRIC(out_buffer_peak_bytes,           21);
SS_ASSERT_METRIC(nrt_out_buffer_peak_bytes,         22);
SS_ASSERT_METRIC(scheduler_max_late_ms,           23);
SS_ASSERT_METRIC(scheduler_last_late_ms,          24);
SS_ASSERT_METRIC(scheduler_last_late_tick,        25);
SS_ASSERT_METRIC(ring_buffer_direct_write_fails,  26);
// Link [27-38] is native-only and intentionally unasserted (the web merged
// array never reads those slots). The cross-platform system-info block [39-49]
// is written by shared C++ on every runtime and IS asserted against the JS
// mirror in js/lib/metrics_offsets.js.
SS_ASSERT_METRIC(supersonic_version_major,        39);
SS_ASSERT_METRIC(supersonic_version_minor,        40);
SS_ASSERT_METRIC(supersonic_version_patch,        41);
SS_ASSERT_METRIC(audio_sample_rate,               42);
SS_ASSERT_METRIC(audio_block_size,                43);
SS_ASSERT_METRIC(audio_output_channels,           44);
SS_ASSERT_METRIC(audio_input_channels,            45);
SS_ASSERT_METRIC(clock_tempo_mbpm,                46);
SS_ASSERT_METRIC(clock_beat_centi,                47);
SS_ASSERT_METRIC(clock_phase_centi,               48);
SS_ASSERT_METRIC(clock_playing,                   49);
SS_ASSERT_METRIC(_metrics_reserved,               50);

// METRICS_SIZE must cover the whole struct and stay a multiple of 8: the arena
// regions that follow (NTP time, SuperClockState) are 8-byte aligned and read
// via Float64/BigInt64 views. _metrics_reserved exists solely to satisfy this;
// these asserts make removing it (or any odd field count) a build error.
static_assert(sizeof(PerformanceMetrics) == METRICS_SIZE,
              "METRICS_SIZE must equal sizeof(PerformanceMetrics)");
static_assert(METRICS_SIZE % 8 == 0,
              "METRICS_SIZE must be a multiple of 8 to keep following regions 8-byte aligned");

#undef SS_ASSERT_METRIC
#undef SS_ASSERT_OFFSET

#if defined(__clang__) || defined(__GNUC__)
#  pragma GCC diagnostic pop
#endif

#endif // SCSYNTH_SHARED_MEMORY_H

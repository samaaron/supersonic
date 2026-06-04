// SPDX-License-Identifier: MIT OR GPL-3.0-or-later
// Copyright (c) 2025 Sam Aaron

/**
 * Shared metrics offset constants for SuperSonic.
 *
 * These offsets correspond to the PerformanceMetrics struct in shared_memory.h.
 * All values are Uint32 array indices (not byte offsets).
 *
 * Layout designed for contiguous memcpy operations:
 * - [0-8]   scsynth (WASM + worklet writes)
 * - [9-23]  Prescheduler (prescheduler worker - all contiguous for single memcpy overlay)
 * - [24-25] OSC Out (main thread)
 * - [26-29] OSC In (osc_in_worker)
 * - [30-31] Debug (debug_worker)
 * - [32-34] Ring buffer usage (WASM writes)
 * - [35-37] Ring buffer peak usage (WASM writes)
 * - [38-41] Bypass category metrics (main thread / PM transport)
 * - [42-44] scsynth late timing diagnostics (WASM writes)
 * - [45]    Ring buffer direct write failures (OscChannel SAB mode)
 * - [46-57] Link (native-only; always 0 on web)
 * - [58-64] System info: version + audio config (shared C++, write-once)
 * - [65-68] SuperClock readouts: tempo/beat/phase/playing (shared C++, per block)
 * - [69+]   Context metrics (JS main-thread only; above the SAB region)
 */

// =============================================================================
// scsynth metrics [0-8] (written by WASM and JS worklet)
// =============================================================================
export const SCSYNTH_PROCESS_COUNT = 0;        // Audio process() callback count
export const SCSYNTH_MESSAGES_PROCESSED = 1;   // OSC messages processed by scsynth
export const SCSYNTH_MESSAGES_DROPPED = 2;     // Messages dropped (various reasons)
export const SCSYNTH_SCHEDULER_DEPTH = 3;      // Current scheduler queue depth
export const SCSYNTH_SCHEDULER_PEAK_DEPTH = 4; // Peak scheduler queue depth
export const SCSYNTH_SCHEDULER_DROPPED = 5;    // Messages dropped due to scheduler overflow
export const SCSYNTH_SEQUENCE_GAPS = 6;        // Detected sequence gaps (missing messages)
export const SCSYNTH_WASM_ERRORS = 7;          // WASM execution errors (written by JS worklet)
export const SCSYNTH_SCHEDULER_LATES = 8;      // Bundles executed after their scheduled time (WASM)

// =============================================================================
// Prescheduler metrics [9-23] (written by osc_out_prescheduler_worker.js)
// ALL CONTIGUOUS for single memcpy overlay in postMessage mode
// =============================================================================
export const PRESCHEDULER_PENDING = 9;           // Events waiting to be dispatched
export const PRESCHEDULER_PENDING_PEAK = 10;     // Peak pending events
export const PRESCHEDULER_BUNDLES_SCHEDULED = 11; // Bundles scheduled (timed)
export const PRESCHEDULER_DISPATCHED = 12;       // Events dispatched to worklet
export const PRESCHEDULER_EVENTS_CANCELLED = 13; // Events cancelled before dispatch
export const PRESCHEDULER_MIN_HEADROOM_MS = 14;  // All-time min headroom before execution
export const PRESCHEDULER_LATES = 15;            // Bundles dispatched after their execution time
export const PRESCHEDULER_RETRIES_SUCCEEDED = 16; // Buffer-full retries that succeeded
export const PRESCHEDULER_RETRIES_FAILED = 17;   // Buffer-full retries that failed
export const PRESCHEDULER_RETRY_QUEUE_SIZE = 18; // Current retry queue size
export const PRESCHEDULER_RETRY_QUEUE_PEAK = 19; // Peak retry queue size
export const PRESCHEDULER_MESSAGES_RETRIED = 20; // Total messages that needed retry
export const PRESCHEDULER_TOTAL_DISPATCHES = 21; // Total dispatch attempts
export const PRESCHEDULER_BYPASSED = 22;         // Messages that bypassed prescheduler
export const PRESCHEDULER_MAX_LATE_MS = 23;      // Maximum lateness at prescheduler (ms)

// Prescheduler range constants for overlay
export const PRESCHEDULER_START = 9;
export const PRESCHEDULER_COUNT = 15;  // 9-23 inclusive

// =============================================================================
// OSC Out metrics [24-25] (written by supersonic.js main thread)
// =============================================================================
export const OSC_OUT_MESSAGES_SENT = 24;  // OSC messages sent to scsynth
export const OSC_OUT_BYTES_SENT = 25;     // Total bytes sent

// =============================================================================
// OSC In metrics [26-29] (written by osc_in_worker.js)
// =============================================================================
export const OSC_IN_MESSAGES_RECEIVED = 26; // OSC messages received from scsynth
export const OSC_IN_BYTES_RECEIVED = 27;    // Total bytes received
export const OSC_IN_DROPPED_MESSAGES = 28;  // Messages dropped (sequence gaps/corruption)
export const OSC_IN_CORRUPTED = 29;         // Ring buffer message corruption detected

// =============================================================================
// Debug metrics [30-31] (written by debug_worker.js)
// =============================================================================
export const DEBUG_MESSAGES_RECEIVED = 30;  // Debug messages from scsynth
export const DEBUG_BYTES_RECEIVED = 31;     // Total debug bytes

// =============================================================================
// Ring buffer usage [32-34] (written by WASM during process())
// =============================================================================
export const IN_BUFFER_USED_BYTES = 32;     // Bytes used in IN buffer
export const OUT_BUFFER_USED_BYTES = 33;    // Bytes used in OUT buffer
export const DEBUG_BUFFER_USED_BYTES = 34;  // Bytes used in DEBUG buffer

// =============================================================================
// Ring buffer peak usage [35-37] (written by WASM during process())
// =============================================================================
export const IN_BUFFER_PEAK_BYTES = 35;     // Peak bytes used in IN buffer
export const OUT_BUFFER_PEAK_BYTES = 36;    // Peak bytes used in OUT buffer
export const DEBUG_BUFFER_PEAK_BYTES = 37;  // Peak bytes used in DEBUG buffer

// =============================================================================
// Bypass category metrics [38-41] (written by supersonic.js main thread / PM transport)
// =============================================================================
export const BYPASS_NON_BUNDLE = 38;    // Plain OSC messages (not bundles)
export const BYPASS_IMMEDIATE = 39;     // Bundles with timetag 0 or 1
export const BYPASS_NEAR_FUTURE = 40;   // Within bypass lookahead threshold but not late
export const BYPASS_LATE = 41;          // Past their scheduled time (diffSeconds < 0)

// =============================================================================
// scsynth late timing diagnostics [42-44] (written by WASM during process())
// =============================================================================
export const SCSYNTH_SCHEDULER_MAX_LATE_MS = 42;    // Maximum lateness observed (ms)
export const SCSYNTH_SCHEDULER_LAST_LATE_MS = 43;   // Most recent late magnitude (ms)
export const SCSYNTH_SCHEDULER_LAST_LATE_TICK = 44; // Process count when last late occurred

// =============================================================================
// Ring buffer direct write failures [45] (written by OscChannel in SAB mode)
// =============================================================================
export const RING_BUFFER_DIRECT_WRITE_FAILS = 45;   // SAB mode only: optimistic direct writes that failed (delivered via prescheduler)

// =============================================================================
// Link [46-57] — native-only; the web build never writes these (always 0).
// Mirror of PerformanceMetrics in shared_memory.h.
// =============================================================================
export const LINK_PEERS              = 46;  // connected Link peers
export const LINK_TEMPO_MBPM         = 47;  // tempo, milli-BPM (bpm * 1000)
export const LINK_BEAT_CENTI         = 48;  // beat position * 100
export const LINK_PHASE_CENTI        = 49;  // phase within quantum * 100
export const LINK_PLAYING            = 50;  // transport 0/1
export const LINK_AUDIO_IN_CHANNELS  = 51;  // active received channels
export const LINK_AUDIO_STREAM_RATE  = 52;  // received stream sample rate (Hz)
export const LINK_AUDIO_UNDERRUNS    = 53;  // receiver queue underrun events
export const LINK_AUDIO_BUFFERED_MS  = 54;  // receiver queue depth (ms)
export const LINK_AUDIO_DRIFT_PPM    = 55;  // read-rate deviation from 1.0 (ppm, signed)
export const LINK_AUDIO_PUBLISH      = 56;  // publishing enabled 0/1
export const LINK_AUDIO_SINKS        = 57;  // active output sinks

// =============================================================================
// System info [58-64] — cross-platform; written by shared C++ (init_memory),
// so these are real values on web AND native. Mirror of PerformanceMetrics
// in shared_memory.h; asserted via SS_ASSERT_METRIC.
// =============================================================================
export const SUPERSONIC_VERSION_MAJOR = 58;  // SUPERSONIC_VERSION_MAJOR
export const SUPERSONIC_VERSION_MINOR = 59;  // SUPERSONIC_VERSION_MINOR
export const SUPERSONIC_VERSION_PATCH = 60;  // SUPERSONIC_VERSION_PATCH
export const AUDIO_SAMPLE_RATE        = 61;  // output sample rate (Hz)
export const AUDIO_BLOCK_SIZE         = 62;  // block size (frames; 128 on web)
export const AUDIO_OUTPUT_CHANNELS    = 63;  // output bus channels
export const AUDIO_INPUT_CHANNELS     = 64;  // input bus channels

// =============================================================================
// SuperClock readouts [65-68] — cross-platform; written per block by
// publishClockMetrics() (SuperClock.cpp). Live on web and native alike.
// =============================================================================
export const CLOCK_TEMPO_MBPM  = 65;  // tempo, milli-BPM (bpm * 1000)
export const CLOCK_BEAT_CENTI  = 66;  // beat position * 100
export const CLOCK_PHASE_CENTI = 67;  // phase within quantum * 100
export const CLOCK_PLAYING     = 68;  // transport 0/1

// Slot 69 is reserved padding in the C++ struct so METRICS_SIZE stays a
// multiple of 8 (the following arena regions are 8-byte aligned). The merged
// array reuses index 69 for the first context metric (CTX_DRIFT_OFFSET_MS),
// which is written after the SAB copy, mirroring how context reuses the
// native-only Link slots.
export const METRICS_RESERVED  = 69;

// Number of metric slots in the SAB/snapshot buffer == METRICS_SIZE / 4.
export const SAB_METRICS_COUNT = METRICS_RESERVED + 1;  // 70

// =============================================================================
// Context metrics [69-81] (written by main thread into merged array only)
// These are NOT in the C++ PerformanceMetrics struct or SAB — MetricsReader
// writes them into the local merged Uint32Array. They start at index 69,
// reusing the reserved padding slot (the last meaningful SAB metric is
// CLOCK_PLAYING at 68); context is written after the SAB copy so it wins.
// =============================================================================
export const CTX_DRIFT_OFFSET_MS = 69;             // Clock drift (int32, ms)
export const CTX_CLOCK_OFFSET_MS = 70;             // Clock offset for multi-system sync (int32, ms)
export const CTX_AUDIO_CONTEXT_STATE = 71;          // Enum: 0=unknown,1=running,2=suspended,3=closed,4=interrupted
export const CTX_BUFFER_POOL_USED_BYTES = 72;       // Buffer pool bytes used
export const CTX_BUFFER_POOL_AVAILABLE_BYTES = 73;  // Buffer pool bytes available
export const CTX_BUFFER_POOL_ALLOCATIONS = 74;      // Total buffer allocations
export const CTX_LOADED_SYNTH_DEFS = 75;            // Number of loaded synthdefs
export const CTX_SCSYNTH_SCHEDULER_CAPACITY = 76;   // Static from bufferConstants
export const CTX_PRESCHEDULER_CAPACITY = 77;        // Static from config
export const CTX_IN_BUFFER_CAPACITY = 78;           // Static from bufferConstants
export const CTX_OUT_BUFFER_CAPACITY = 79;          // Static from bufferConstants
export const CTX_DEBUG_BUFFER_CAPACITY = 80;        // Static from bufferConstants
export const CTX_MODE = 81;                         // Enum: 0=sab, 1=postMessage

// =============================================================================
// Audio diagnostics [82-88] (written by main thread into merged array only)
// =============================================================================
export const CTX_GLITCH_COUNT = 82;          // Chrome only: playbackStats.fallbackFramesEvents
export const CTX_GLITCH_DURATION_MS = 83;    // Chrome only: playbackStats.fallbackFramesDuration * 1000 (ms int)
export const CTX_AVERAGE_LATENCY_US = 84;    // Chrome only: playbackStats.averageLatency * 1_000_000 (us int)
export const CTX_MAX_LATENCY_US = 85;        // Chrome only: playbackStats.maximumLatency * 1_000_000 (us int)
export const CTX_AUDIO_HEALTH_PCT = 86;      // Cross-browser: audio health percentage 0-100
export const CTX_TOTAL_FRAMES_DURATION_MS = 87; // Chrome only: playbackStats.totalFramesDuration * 1000 (ms int)
export const CTX_HAS_PLAYBACK_STATS = 88;    // 1 if Chrome playbackStats available, 0 otherwise

// =============================================================================
// Buffer pool growth metrics [89-92] (written by main thread into merged array only)
// =============================================================================
export const CTX_BUFFER_POOL_TOTAL_CAPACITY = 89;  // Current committed capacity across all pools (bytes)
export const CTX_BUFFER_POOL_MAX_CAPACITY = 90;    // Hard ceiling from maxBufferMemory config (bytes)
export const CTX_BUFFER_POOL_GROWTH_COUNT = 91;    // Number of times the pool has grown (counter)
export const CTX_BUFFER_POOL_POOL_COUNT = 92;      // Number of pool segments (1 = no growth yet)

// Merged array size (slots 0-68 from SAB/snapshot, 69-81 context, 82-88 audio, 89-92 buffer growth)
export const MERGED_ARRAY_SIZE = 93;

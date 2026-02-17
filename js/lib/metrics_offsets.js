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

// Number of metric slots in the SAB/snapshot buffer (indices 0 through RING_BUFFER_DIRECT_WRITE_FAILS)
export const SAB_METRICS_COUNT = RING_BUFFER_DIRECT_WRITE_FAILS + 1;  // 46

// =============================================================================
// Context metrics [46-58] (written by main thread into merged array only)
// These are NOT in the C++ PerformanceMetrics struct or SAB —
// MetricsReader writes them into the local merged Uint32Array.
// =============================================================================
export const CTX_DRIFT_OFFSET_MS = 46;             // Clock drift (int32, ms)
export const CTX_GLOBAL_OFFSET_MS = 47;            // Global timing offset (int32, ms)
export const CTX_AUDIO_CONTEXT_STATE = 48;          // Enum: 0=unknown,1=running,2=suspended,3=closed,4=interrupted
export const CTX_BUFFER_POOL_USED_BYTES = 49;       // Buffer pool bytes used
export const CTX_BUFFER_POOL_AVAILABLE_BYTES = 50;  // Buffer pool bytes available
export const CTX_BUFFER_POOL_ALLOCATIONS = 51;      // Total buffer allocations
export const CTX_LOADED_SYNTH_DEFS = 52;            // Number of loaded synthdefs
export const CTX_SCSYNTH_SCHEDULER_CAPACITY = 53;   // Static from bufferConstants
export const CTX_PRESCHEDULER_CAPACITY = 54;        // Static from config
export const CTX_IN_BUFFER_CAPACITY = 55;           // Static from bufferConstants
export const CTX_OUT_BUFFER_CAPACITY = 56;          // Static from bufferConstants
export const CTX_DEBUG_BUFFER_CAPACITY = 57;        // Static from bufferConstants
export const CTX_MODE = 58;                         // Enum: 0=sab, 1=postMessage

// Merged array size (slots 0-45 from SAB/snapshot, 46-58 context, 59-63 reserved)
export const MERGED_ARRAY_SIZE = 64;

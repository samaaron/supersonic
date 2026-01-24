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
 * - [9-22]  Prescheduler (prescheduler worker - all contiguous for single memcpy overlay)
 * - [23-24] OSC Out (main thread)
 * - [25-28] OSC In (osc_in_worker)
 * - [29-30] Debug (debug_worker)
 * - [31-33] Ring buffer usage (WASM writes)
 * - [34-36] Ring buffer peak usage (WASM writes)
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
// Prescheduler metrics [9-22] (written by osc_out_prescheduler_worker.js)
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

// Prescheduler range constants for overlay
export const PRESCHEDULER_START = 9;
export const PRESCHEDULER_COUNT = 14;  // 9-22 inclusive

// =============================================================================
// OSC Out metrics [23-24] (written by supersonic.js main thread)
// =============================================================================
export const OSC_OUT_MESSAGES_SENT = 23;  // OSC messages sent to scsynth
export const OSC_OUT_BYTES_SENT = 24;     // Total bytes sent

// =============================================================================
// OSC In metrics [25-28] (written by osc_in_worker.js)
// =============================================================================
export const OSC_IN_MESSAGES_RECEIVED = 25; // OSC messages received from scsynth
export const OSC_IN_BYTES_RECEIVED = 26;    // Total bytes received
export const OSC_IN_DROPPED_MESSAGES = 27;  // Messages dropped (sequence gaps/corruption)
export const OSC_IN_CORRUPTED = 28;         // Ring buffer message corruption detected

// =============================================================================
// Debug metrics [29-30] (written by debug_worker.js)
// =============================================================================
export const DEBUG_MESSAGES_RECEIVED = 29;  // Debug messages from scsynth
export const DEBUG_BYTES_RECEIVED = 30;     // Total debug bytes

// =============================================================================
// Ring buffer usage [31-33] (written by WASM during process())
// =============================================================================
export const IN_BUFFER_USED_BYTES = 31;     // Bytes used in IN buffer
export const OUT_BUFFER_USED_BYTES = 32;    // Bytes used in OUT buffer
export const DEBUG_BUFFER_USED_BYTES = 33;  // Bytes used in DEBUG buffer

// =============================================================================
// Ring buffer peak usage [34-36] (written by WASM during process())
// =============================================================================
export const IN_BUFFER_PEAK_BYTES = 34;     // Peak bytes used in IN buffer
export const OUT_BUFFER_PEAK_BYTES = 35;    // Peak bytes used in OUT buffer
export const DEBUG_BUFFER_PEAK_BYTES = 36;  // Peak bytes used in DEBUG buffer

// =============================================================================
// Bypass category metrics [37-40] (written by supersonic.js main thread / PM transport)
// =============================================================================
export const BYPASS_NON_BUNDLE = 37;    // Plain OSC messages (not bundles)
export const BYPASS_IMMEDIATE = 38;     // Bundles with timetag 0 or 1
export const BYPASS_NEAR_FUTURE = 39;   // Within 200ms but not late (diffSeconds >= 0 and < 0.2)
export const BYPASS_LATE = 40;          // Past their scheduled time (diffSeconds < 0)

// [41] padding

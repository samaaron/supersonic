// SPDX-License-Identifier: MIT OR GPL-3.0-or-later
// Copyright (c) 2025 Sam Aaron

/**
 * Shared metrics offset constants for SuperSonic.
 *
 * These offsets correspond to the PerformanceMetrics struct in shared_memory.h.
 * All values are Uint32 array indices (not byte offsets).
 *
 * Layout: [0-5] Worklet, [6-16] OSC Out, [17-19] OSC In, [20-21] Debug, [22-23] Main thread, [24] Gap detection, [25] Direct writes, [26-31] padding
 */

// Worklet metrics (written by WASM)
export const PROCESS_COUNT = 0;
export const MESSAGES_PROCESSED = 1;
export const MESSAGES_DROPPED = 2;
export const SCHEDULER_QUEUE_DEPTH = 3;
export const SCHEDULER_QUEUE_MAX = 4;
export const SCHEDULER_QUEUE_DROPPED = 5;

// OSC Out / Prescheduler metrics (written by osc_out_prescheduler_worker.js)
export const PRESCHEDULER_PENDING = 6;
export const PRESCHEDULER_PEAK = 7;
export const PRESCHEDULER_SENT = 8;
export const RETRIES_SUCCEEDED = 9;
export const RETRIES_FAILED = 10;
export const BUNDLES_SCHEDULED = 11;
export const EVENTS_CANCELLED = 12;
export const TOTAL_DISPATCHES = 13;
export const MESSAGES_RETRIED = 14;
export const RETRY_QUEUE_SIZE = 15;
export const RETRY_QUEUE_MAX = 16;

// OSC In metrics (written by osc_in_worker.js)
export const OSC_IN_MESSAGES_RECEIVED = 17;
export const OSC_IN_DROPPED_MESSAGES = 18;
export const OSC_IN_BYTES_RECEIVED = 19;

// Debug metrics (written by debug_worker.js)
export const DEBUG_MESSAGES_RECEIVED = 20;
export const DEBUG_BYTES_RECEIVED = 21;

// Main thread metrics (written by supersonic.js via Atomics)
export const MESSAGES_SENT = 22;
export const BYTES_SENT = 23;

// Gap detection metrics (written by WASM)
export const SEQUENCE_GAPS = 24;

// Direct write metrics (written by supersonic.js main thread)
export const DIRECT_WRITES = 25;  // Messages that bypassed prescheduler worker

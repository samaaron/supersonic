// SPDX-License-Identifier: MIT OR GPL-3.0-or-later
// Copyright (c) 2025 Sam Aaron

/**
 * Type definitions for SuperSonic runtime metrics.
 *
 * All metrics are stored in SharedArrayBuffer and read synchronously at 10Hz.
 * Written by: WASM worklet, OSC workers, and main thread (via Atomics).
 *
 * Primarily used for debugging and monitoring system health - tracking buffer
 * usage, message throughput, and detecting overruns or dropped messages.
 *
 * @module metrics_types
 */

/**
 * Metrics snapshot delivered to onMetricsUpdate callback.
 * All values are read synchronously from SharedArrayBuffer.
 *
 * @typedef {Object} SuperSonicMetrics
 *
 * --- scsynth (written by WASM, offsets 0-8) ---
 * @property {number} scsynthProcessCount - Audio process() calls (cumulative)
 * @property {number} scsynthMessagesProcessed - Messages processed by scsynth from IN buffer
 * @property {number} scsynthMessagesDropped - Messages dropped by scsynth scheduler (queue full)
 * @property {number} scsynthSchedulerDepth - Current scsynth scheduler queue depth
 * @property {number} scsynthSchedulerPeakDepth - Peak scsynth scheduler queue depth (high water mark)
 * @property {number} scsynthSchedulerCapacity - Maximum scsynth scheduler queue size (compile-time constant)
 * @property {number} scsynthSchedulerDropped - Messages dropped from scsynth scheduler queue
 * @property {number} scsynthSequenceGaps - Sequence gaps detected (indicates lost messages)
 * @property {number} scsynthSchedulerLates - Bundles executed after their scheduled time
 *
 * --- Prescheduler (written by osc_out_prescheduler_worker.js, offsets 9-22) ---
 * @property {number} preschedulerPending - Events waiting in prescheduler queue
 * @property {number} preschedulerPendingPeak - Peak prescheduler queue depth
 * @property {number} preschedulerDispatched - Bundles successfully written to IN buffer
 * @property {number} preschedulerRetriesSucceeded - Ring buffer writes that succeeded after retry
 * @property {number} preschedulerRetriesFailed - Ring buffer writes that failed after max retries
 * @property {number} preschedulerBundlesScheduled - Total bundles added to prescheduler
 * @property {number} preschedulerEventsCancelled - Bundles cancelled before dispatch
 * @property {number} preschedulerTotalDispatches - Dispatch cycles executed
 * @property {number} preschedulerMessagesRetried - Total retry attempts (includes multiple per message)
 * @property {number} preschedulerRetryQueueSize - Current retry queue size
 * @property {number} preschedulerRetryQueuePeak - Peak retry queue size
 * @property {number} preschedulerBypassed - Messages that bypassed prescheduler (direct ring buffer writes, aggregate)
 * @property {number} bypassNonBundle - Plain OSC messages (not bundles) that bypassed prescheduler
 * @property {number} bypassImmediate - Bundles with timetag 0 or 1 that bypassed prescheduler
 * @property {number} bypassNearFuture - Bundles within bypass lookahead threshold that bypassed prescheduler
 * @property {number} bypassLate - Bundles past their scheduled time that bypassed prescheduler
 * @property {number} preschedulerCapacity - Maximum pending events allowed in prescheduler
 * @property {number} preschedulerMinHeadroomMs - All-time minimum headroom before execution
 * @property {number} preschedulerLates - Bundles dispatched after their scheduled execution time
 *
 * --- OSC In Worker (written by osc_in_worker.js, offsets 25-28) ---
 * @property {number} oscInMessagesReceived - OSC replies received from scsynth (OUT buffer → JS)
 * @property {number} oscInMessagesDropped - OSC replies lost, detected via sequence gaps or corruption
 * @property {number} oscInBytesReceived - Total bytes received from scsynth (OUT buffer → JS)
 *
 * --- Debug Worker (written by debug_worker.js, offsets 29-30) ---
 * @property {number} debugMessagesReceived - Debug messages received from scsynth
 * @property {number} debugBytesReceived - Total bytes received from scsynth (DEBUG buffer → JS)
 *
 * --- Main Thread (written by supersonic.js, offsets 23-24) ---
 * @property {number} oscOutMessagesSent - OSC messages sent to scsynth (JS → IN buffer)
 * @property {number} oscOutBytesSent - Total bytes sent to scsynth (JS → IN buffer)
 *
 * --- Ring Buffer Usage (written by WASM during process(), offsets 31-33) ---
 * @property {number} inBufferUsedBytes - Raw bytes used in IN buffer (JS → scsynth)
 * @property {number} outBufferUsedBytes - Raw bytes used in OUT buffer (scsynth → JS)
 * @property {number} debugBufferUsedBytes - Raw bytes used in DEBUG buffer (scsynth → JS)
 * @property {Object} inBufferUsed - IN buffer usage with percentage (derived from raw bytes)
 * @property {number} inBufferUsed.bytes - Bytes currently in IN buffer
 * @property {number} inBufferUsed.percentage - Percentage of IN buffer used
 * @property {number} inBufferUsed.capacity - Total IN buffer capacity in bytes
 * @property {Object} outBufferUsed - OUT buffer usage with percentage (derived from raw bytes)
 * @property {number} outBufferUsed.bytes - Bytes currently in OUT buffer
 * @property {number} outBufferUsed.percentage - Percentage of OUT buffer used
 * @property {number} outBufferUsed.capacity - Total OUT buffer capacity in bytes
 * @property {Object} debugBufferUsed - Debug buffer usage with percentage (derived from raw bytes)
 * @property {number} debugBufferUsed.bytes - Bytes currently in debug buffer
 * @property {number} debugBufferUsed.percentage - Percentage of debug buffer used
 * @property {number} debugBufferUsed.capacity - Total debug buffer capacity in bytes
 *
 * --- Timing ---
 * @property {number} driftOffsetMs - Drift between AudioContext and performance.now() (milliseconds)
 *
 * --- Error Metrics ---
 * @property {number} scsynthWasmErrors - Count of WASM execution errors in audio worklet
 * @property {number} oscInCorrupted - Ring buffer message corruption detected (invalid framing)
 */

export {};

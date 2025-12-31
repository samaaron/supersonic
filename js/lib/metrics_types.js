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
 * @see SuperSonic#onMetricsUpdate
 * @module metrics_types
 */

/**
 * Metrics snapshot delivered to onMetricsUpdate callback.
 * All values are read synchronously from SharedArrayBuffer.
 *
 * @typedef {Object} SuperSonicMetrics
 *
 * --- Worklet (written by WASM, offsets 0-5) ---
 * @property {number} workletProcessCount - Audio process() calls (cumulative)
 * @property {number} workletMessagesProcessed - Messages processed by scsynth from IN buffer
 * @property {number} workletMessagesDropped - Messages dropped by scsynth scheduler (queue full)
 * @property {number} workletSchedulerDepth - Current scsynth scheduler queue depth
 * @property {number} workletSchedulerMax - Peak scsynth scheduler queue depth
 * @property {number} workletSchedulerDropped - Messages dropped from scsynth scheduler queue
 * @property {number} workletSequenceGaps - Sequence gaps detected (indicates lost messages)
 *
 * --- Prescheduler (written by osc_out_prescheduler_worker.js, offsets 6-16) ---
 * @property {number} preschedulerPending - Events waiting in prescheduler queue
 * @property {number} preschedulerPeak - Peak prescheduler queue depth
 * @property {number} preschedulerSent - Bundles successfully written to IN buffer
 * @property {number} preschedulerRetriesSucceeded - Ring buffer writes that succeeded after retry
 * @property {number} preschedulerRetriesFailed - Ring buffer writes that failed after max retries
 * @property {number} preschedulerBundlesScheduled - Total bundles added to prescheduler
 * @property {number} preschedulerEventsCancelled - Bundles cancelled before dispatch
 * @property {number} preschedulerTotalDispatches - Dispatch cycles executed
 * @property {number} preschedulerMessagesRetried - Total retry attempts (includes multiple per message)
 * @property {number} preschedulerRetryQueueSize - Current retry queue size
 * @property {number} preschedulerRetryQueueMax - Peak retry queue size
 * @property {number} preschedulerBypassed - Messages that bypassed prescheduler (direct ring buffer writes)
 *
 * --- OSC In Worker (written by osc_in_worker.js, offsets 17-19) ---
 * @property {number} oscInMessagesReceived - OSC replies received from scsynth (OUT buffer → JS)
 * @property {number} oscInMessagesDropped - OSC replies lost, detected via sequence gaps or corruption
 * @property {number} oscInBytesReceived - Total bytes received from scsynth (OUT buffer → JS)
 *
 * --- Debug Worker (written by debug_worker.js, offsets 20-21) ---
 * @property {number} debugMessagesReceived - Debug messages received from scsynth
 * @property {number} debugBytesReceived - Total bytes received from scsynth (DEBUG buffer → JS)
 *
 * --- Main Thread (written by supersonic.js, offsets 22-23) ---
 * @property {number} mainMessagesSent - OSC messages sent to scsynth (JS → IN buffer)
 * @property {number} mainBytesSent - Total bytes sent to scsynth (JS → IN buffer)
 *
 * --- Ring Buffer Usage (calculated from head/tail pointers) ---
 * @property {Object} inBufferUsed - IN buffer usage (JS → scsynth)
 * @property {number} inBufferUsed.bytes - Bytes currently in IN buffer
 * @property {number} inBufferUsed.percentage - Percentage of IN buffer used
 * @property {Object} outBufferUsed - OUT buffer usage (scsynth → JS)
 * @property {number} outBufferUsed.bytes - Bytes currently in OUT buffer
 * @property {number} outBufferUsed.percentage - Percentage of OUT buffer used
 * @property {Object} debugBufferUsed - Debug buffer usage (scsynth → JS)
 * @property {number} debugBufferUsed.bytes - Bytes currently in debug buffer
 * @property {number} debugBufferUsed.percentage - Percentage of debug buffer used
 *
 * --- Timing ---
 * @property {number} driftOffsetMs - Drift between AudioContext and performance.now() (milliseconds)
 */

export {};

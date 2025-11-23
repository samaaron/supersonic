/*
    SuperSonic - SuperCollider AudioWorklet WebAssembly port
    Copyright (c) 2025 Sam Aaron

    Based on SuperCollider by James McCartney and community
    GPL v3 or later
*/

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
 * --- Main Thread (written by supersonic.js, offsets 22-23) ---
 * @property {number} messagesSent - OSC messages sent to scsynth (JS → IN buffer)
 * @property {number} bytesSent - Total bytes sent to scsynth (JS → IN buffer)
 *
 * --- Worklet (written by WASM, offsets 0-5) ---
 * @property {number} processCount - Audio process() calls (cumulative)
 * @property {number} messagesProcessed - Messages processed by scsynth from IN buffer
 * @property {number} messagesDropped - Messages dropped by scsynth scheduler (queue full)
 * @property {number} schedulerQueueDepth - Current scsynth scheduler queue depth
 * @property {number} schedulerQueueMax - Peak scsynth scheduler queue depth
 * @property {number} schedulerQueueDropped - Messages dropped from scsynth scheduler queue
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
 * --- Prescheduler (written by osc_out_prescheduler_worker.js, offsets 6-16) ---
 * @property {number} preschedulerPending - Events waiting in prescheduler queue
 * @property {number} preschedulerPeak - Peak prescheduler queue depth
 * @property {number} preschedulerSent - Bundles successfully written to IN buffer
 * @property {number} retriesSucceeded - Ring buffer writes that succeeded after retry
 * @property {number} retriesFailed - Ring buffer writes that failed after max retries
 * @property {number} bundlesScheduled - Total bundles added to prescheduler
 * @property {number} eventsCancelled - Bundles cancelled before dispatch
 * @property {number} totalDispatches - Dispatch cycles executed
 * @property {number} messagesRetried - Total retry attempts (includes multiple per message)
 * @property {number} retryQueueSize - Current retry queue size
 * @property {number} retryQueueMax - Peak retry queue size
 *
 * --- OSC In Worker (written by osc_in_worker.js, offsets 17-19) ---
 * @property {number} oscInMessagesReceived - OSC replies received from scsynth (OUT buffer → JS)
 * @property {number} oscInDroppedMessages - OSC replies lost, detected via sequence gaps or corruption
 * @property {number} oscInBytesReceived - Total bytes received from scsynth (OUT buffer → JS)
 *
 * --- Debug Worker (written by debug_worker.js, offsets 20-21) ---
 * @property {number} debugMessagesReceived - Debug messages received from scsynth
 * @property {number} debugBytesReceived - Total bytes received from scsynth (DEBUG buffer → JS)
 *
 * --- Timing ---
 * @property {number} driftOffsetMs - Drift between AudioContext and performance.now() (milliseconds)
 */

export {};

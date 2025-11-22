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
 * @property {number} messagesSent - OSC messages sent to scsynth
 *
 * @property {number} processCount - Audio process() calls
 * @property {number} messagesProcessed - Messages processed by scsynth
 * @property {number} messagesDropped - Messages dropped by scsynth
 * @property {number} schedulerQueueDepth - Current scheduler queue depth
 * @property {number} schedulerQueueMax - Maximum scheduler queue depth reached
 * @property {number} schedulerQueueDropped - Messages dropped from scheduler queue
 *
 * @property {Object} inBufferUsed - Input buffer usage statistics
 * @property {number} inBufferUsed.bytes - Bytes used in input buffer
 * @property {number} inBufferUsed.percentage - Percentage of input buffer used
 * @property {Object} outBufferUsed - Output buffer usage statistics
 * @property {number} outBufferUsed.bytes - Bytes used in output buffer
 * @property {number} outBufferUsed.percentage - Percentage of output buffer used
 * @property {Object} debugBufferUsed - Debug buffer usage statistics
 * @property {number} debugBufferUsed.bytes - Bytes used in debug buffer
 * @property {number} debugBufferUsed.percentage - Percentage of debug buffer used
 *
 * @property {number} preschedulerPending - Current pending events in queue
 * @property {number} preschedulerPeak - Peak pending events (high water mark)
 * @property {number} preschedulerSent - Total bundles written to ring buffer
 * @property {number} retriesSucceeded - Successful retry attempts
 * @property {number} retriesFailed - Failed retry attempts (gave up)
 * @property {number} bundlesScheduled - Total bundles scheduled
 * @property {number} eventsCancelled - Total events cancelled
 * @property {number} totalDispatches - Total dispatch cycles executed
 * @property {number} messagesRetried - Total retry attempts (all)
 * @property {number} retryQueueSize - Current retry queue size
 * @property {number} retryQueueMax - Peak retry queue size
 *
 * @property {number} oscInMessagesReceived - OSC In messages received
 * @property {number} oscInDroppedMessages - OSC In dropped messages
 * @property {number} oscInWakeups - OSC In worker wakeups
 * @property {number} oscInTimeouts - OSC In worker timeouts
 *
 * @property {number} debugMessagesReceived - Debug messages received
 * @property {number} debugWakeups - Debug worker wakeups
 * @property {number} debugTimeouts - Debug worker timeouts
 * @property {number} debugBytesRead - Debug bytes read
 */

export {};

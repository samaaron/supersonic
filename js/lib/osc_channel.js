// SPDX-License-Identifier: MIT OR GPL-3.0-or-later
// Copyright (c) 2025 Sam Aaron

import { writeToRingBuffer } from './ring_buffer_writer.js';
import * as MetricsOffsets from './metrics_offsets.js';
import { calculateInControlIndices } from './control_offsets.js';
import {
    classifyOscMessage,
    shouldBypass,
    getCurrentNTPFromPerformance,
    DEFAULT_BYPASS_LOOKAHEAD_S,
} from './osc_classifier.js';

/**
 * OscChannel - Unified dispatch for sending OSC to the audio worklet
 *
 * Handles classification, routing, and metrics for all OSC messages:
 * - nonBundle/immediate/nearFuture/late → direct to worklet (bypass)
 * - farFuture → prescheduler for proper scheduling
 *
 * Works in both SAB and postMessage modes. Can be transferred to Web Workers
 * for direct communication with the AudioWorklet.
 */
export class OscChannel {
    #mode;
    #directPort;         // postMessage mode: MessagePort to worklet
    #sabConfig;          // SAB mode: { sharedBuffer, ringBufferBase, bufferConstants, controlIndices }
    #views;              // SAB mode: { atomicView, dataView, uint8View }
    #preschedulerPort;   // MessagePort to prescheduler (both modes)
    #metricsView;        // SAB mode: Int32Array view into metrics region
    #bypassLookaheadS;   // Threshold for bypass vs prescheduler routing
    #sourceId;           // Numeric source ID (0 = main thread, 1+ = workers)
    #blocking;           // Whether this channel can block with Atomics.wait()
    #getCurrentNTP;      // Function returning current NTP time in seconds

    // Node ID allocation (range-based)
    #nodeIdView;         // SAB mode: Int32Array view for atomic counter
    #nodeIdFrom;         // Start of current range (inclusive)
    #nodeIdTo;           // End of current range (exclusive)
    #nextNodeId;         // Next ID to return within range
    #nodeIdRangeSize;    // Number of IDs per range allocation
    #nodeIdSource;       // PM mode (main thread): function to claim a range directly
    #nodeIdPort;         // PM mode (worker): MessagePort for requesting ranges from main thread
    #pendingNodeIdRange; // PM mode (worker): pre-fetched next range
    #transferNodeIdPort; // PM mode: port to include in transferList (created by transferable getter)

    // Local metrics counters
    // SAB mode: used for tracking, then written atomically to shared memory
    // PM mode: accumulated locally, reported via getMetrics()
    #localMetrics = {
        messagesSent: 0,
        bytesSent: 0,
        nonBundle: 0,
        immediate: 0,
        nearFuture: 0,
        late: 0,
        bypassed: 0,
    };

    /**
     * Private constructor - use static factory methods
     */
    constructor(mode, config) {
        this.#mode = mode;
        this.#preschedulerPort = config.preschedulerPort || null;
        this.#bypassLookaheadS = config.bypassLookaheadS ?? DEFAULT_BYPASS_LOOKAHEAD_S;
        this.#sourceId = config.sourceId ?? 0;
        this.#blocking = config.blocking ?? (this.#sourceId !== 0);
        this.#getCurrentNTP = config.getCurrentNTP ?? getCurrentNTPFromPerformance;
        this.#nodeIdRangeSize = 1000;

        if (mode === 'postMessage') {
            this.#directPort = config.port;
        } else {
            this.#sabConfig = {
                sharedBuffer: config.sharedBuffer,
                ringBufferBase: config.ringBufferBase,
                bufferConstants: config.bufferConstants,
                controlIndices: config.controlIndices,
            };
            this.#initViews();

            // Create metrics view at correct offset within SharedArrayBuffer
            if (config.sharedBuffer && config.bufferConstants) {
                const metricsBase = config.ringBufferBase + config.bufferConstants.METRICS_START;
                this.#metricsView = new Int32Array(
                    config.sharedBuffer,
                    metricsBase,
                    config.bufferConstants.METRICS_SIZE / 4
                );
            }

            // Create node ID counter view for atomic range allocation
            if (config.sharedBuffer && config.bufferConstants?.NODE_ID_COUNTER_START !== undefined) {
                const counterBase = config.ringBufferBase + config.bufferConstants.NODE_ID_COUNTER_START;
                this.#nodeIdView = new Int32Array(config.sharedBuffer, counterBase, 1);
                this.#claimNodeIdRange();
            }
        }

        // PM mode: accept a direct range source function (main thread)
        if (config.nodeIdSource) {
            this.#nodeIdSource = config.nodeIdSource;
            this.#claimNodeIdRange();
        }

        // Accept a pre-assigned range (for PM worker channels via fromTransferable)
        if (config.nodeIdRange) {
            this.#nodeIdFrom = config.nodeIdRange.from;
            this.#nodeIdTo = config.nodeIdRange.to;
            this.#nextNodeId = config.nodeIdRange.from;
        }

        // PM mode (worker): MessagePort for requesting more ranges from main thread
        if (config.nodeIdPort) {
            this.#nodeIdPort = config.nodeIdPort;
            this.#nodeIdPort.onmessage = (e) => {
                if (e.data.type === 'nodeIdRange') {
                    this.#pendingNodeIdRange = { from: e.data.from, to: e.data.to };
                }
            };
            // Pre-request the first extra range
            this.#requestNodeIdRange();
        }
    }

    /**
     * Initialize typed array views for SAB mode
     */
    #initViews() {
        const sab = this.#sabConfig.sharedBuffer;
        this.#views = {
            atomicView: new Int32Array(sab),
            dataView: new DataView(sab),
            uint8View: new Uint8Array(sab),
        };
    }

    // =========================================================================
    // Classification
    // =========================================================================

    /**
     * Classify OSC data for routing
     * @param {Uint8Array} oscData
     * @returns {'nonBundle' | 'immediate' | 'nearFuture' | 'late' | 'farFuture'}
     */
    classify(oscData) {
        return classifyOscMessage(oscData, {
            getCurrentNTP: this.#getCurrentNTP,
            bypassLookaheadS: this.#bypassLookaheadS,
        });
    }

    // =========================================================================
    // Metrics
    // =========================================================================

    /**
     * Record a successful send - updates message count, byte count, and category metrics
     * @param {number} byteCount - Size of the message in bytes
     * @param {string} [category] - Bypass category (if applicable)
     */
    #recordSend(byteCount, category = null) {
        if (this.#mode === 'sab' && this.#metricsView) {
            // SAB mode: atomic increment to shared memory
            Atomics.add(this.#metricsView, MetricsOffsets.OSC_OUT_MESSAGES_SENT, 1);
            Atomics.add(this.#metricsView, MetricsOffsets.OSC_OUT_BYTES_SENT, byteCount);

            // Also record category if this was a bypass send
            if (category) {
                const offsetMap = {
                    nonBundle: MetricsOffsets.BYPASS_NON_BUNDLE,
                    immediate: MetricsOffsets.BYPASS_IMMEDIATE,
                    nearFuture: MetricsOffsets.BYPASS_NEAR_FUTURE,
                    late: MetricsOffsets.BYPASS_LATE,
                };
                const offset = offsetMap[category];
                if (offset !== undefined) {
                    Atomics.add(this.#metricsView, offset, 1);
                    Atomics.add(this.#metricsView, MetricsOffsets.PRESCHEDULER_BYPASSED, 1);
                }
            }
        } else {
            // PM mode: local counters
            this.#localMetrics.messagesSent++;
            this.#localMetrics.bytesSent += byteCount;

            if (category && category in this.#localMetrics) {
                this.#localMetrics[category]++;
                this.#localMetrics.bypassed++;
            }
        }
    }

    /**
     * Get and reset local metrics (for periodic reporting)
     * @returns {Object} Metrics snapshot
     */
    getAndResetMetrics() {
        const snapshot = { ...this.#localMetrics };
        this.#localMetrics = {
            messagesSent: 0,
            bytesSent: 0,
            nonBundle: 0,
            immediate: 0,
            nearFuture: 0,
            late: 0,
            bypassed: 0,
        };
        return snapshot;
    }

    /**
     * Get current metrics snapshot.
     * SAB mode: reads aggregated metrics from shared memory
     * PM mode: returns local metrics (aggregated via heartbeat)
     */
    getMetrics() {
        if (this.#mode === 'sab' && this.#metricsView) {
            return {
                messagesSent: Atomics.load(this.#metricsView, MetricsOffsets.OSC_OUT_MESSAGES_SENT),
                bytesSent: Atomics.load(this.#metricsView, MetricsOffsets.OSC_OUT_BYTES_SENT),
                nonBundle: Atomics.load(this.#metricsView, MetricsOffsets.BYPASS_NON_BUNDLE),
                immediate: Atomics.load(this.#metricsView, MetricsOffsets.BYPASS_IMMEDIATE),
                nearFuture: Atomics.load(this.#metricsView, MetricsOffsets.BYPASS_NEAR_FUTURE),
                late: Atomics.load(this.#metricsView, MetricsOffsets.BYPASS_LATE),
                bypassed: Atomics.load(this.#metricsView, MetricsOffsets.PRESCHEDULER_BYPASSED),
            };
        }
        return { ...this.#localMetrics };
    }

    // =========================================================================
    // Sending
    // =========================================================================

    /**
     * Send directly to worklet (bypass path)
     * @param {Uint8Array} oscData
     * @param {string} [bypassCategory] - Category for metrics (PM mode only)
     * @param {boolean} [allowFallback=true] - Allow fallback to prescheduler on SAB contention (main thread only)
     */
    #sendDirect(oscData, bypassCategory = null, allowFallback = true) {
        if (this.#mode === 'postMessage') {
            if (!this.#directPort) return false;
            // Include bypass category and sourceId so worklet can track metrics and log source
            this.#directPort.postMessage({ type: 'osc', oscData, bypassCategory, sourceId: this.#sourceId });
            return true;
        } else {
            // SAB mode - write to ring buffer with sourceId in header
            // Logging is handled by osc_out_log_sab_worker reading from ring buffer
            const canBlock = this.#blocking;

            const success = writeToRingBuffer({
                atomicView: this.#views.atomicView,
                dataView: this.#views.dataView,
                uint8View: this.#views.uint8View,
                bufferConstants: this.#sabConfig.bufferConstants,
                ringBufferBase: this.#sabConfig.ringBufferBase,
                controlIndices: this.#sabConfig.controlIndices,
                oscMessage: oscData,
                sourceId: this.#sourceId,
                // Non-blocking: try once, will fall back to prescheduler
                // Blocking: use Atomics.wait() for guaranteed delivery
                maxSpins: canBlock ? 10 : 0,
                useWait: canBlock,
            });

            if (!success) {
                // Non-blocking: fall back to prescheduler for guaranteed delivery
                // The prescheduler is a worker and can use Atomics.wait()
                if (!canBlock && allowFallback && this.#preschedulerPort) {
                    if (this.#metricsView) {
                        // Track when optimistic direct write falls back to prescheduler (not an error - message still delivered)
                        Atomics.add(this.#metricsView, MetricsOffsets.RING_BUFFER_DIRECT_WRITE_FAILS, 1);
                    }
                    this.#preschedulerPort.postMessage({
                        type: 'directDispatch',
                        oscData,
                        sourceId: this.#sourceId,
                    });
                    return true;  // Message will be delivered via prescheduler
                }
            }
            return success;
        }
    }

    /**
     * Send to prescheduler (far-future path)
     * Logging happens at dispatch time when message reaches the ring buffer.
     */
    #sendToPrescheduler(oscData) {
        if (!this.#preschedulerPort) {
            // Fallback: send direct if no prescheduler port
            // This shouldn't happen in normal usage
            console.error('[OscChannel] No prescheduler port, sending direct');
            return this.#sendDirect(oscData);
        }
        this.#preschedulerPort.postMessage({ type: 'osc', oscData, sourceId: this.#sourceId });
        return true;
    }

    /**
     * Send OSC message with automatic routing
     *
     * - nonBundle/immediate/nearFuture/late → direct to worklet
     * - farFuture → prescheduler for proper scheduling
     *
     * In SAB mode, the main thread uses an optimistic direct write path. If the
     * ring buffer lock is held by another writer, the message is routed through
     * the prescheduler instead. This is not an error - the message is still
     * delivered. Workers don't need this fallback as they use Atomics.wait()
     * for guaranteed lock acquisition.
     *
     * @param {Uint8Array} oscData - OSC message bytes
     * @returns {boolean} true if sent successfully
     */
    send(oscData) {
        const category = this.classify(oscData);

        if (!shouldBypass(category)) {
            // Far-future: route to prescheduler
            const success = this.#sendToPrescheduler(oscData);
            if (success) {
                // Record send without bypass category (prescheduler will track its own metrics)
                this.#recordSend(oscData.length, null);
            }
            return success;
        } else {
            // Bypass: send direct to worklet
            const success = this.#sendDirect(oscData, category);
            if (success) {
                // Record send with bypass category
                this.#recordSend(oscData.length, category);
            }
            return success;
        }
    }

    /**
     * Send directly without classification (for callers who already know routing)
     * Does not increment metrics.
     * @param {Uint8Array} oscData
     * @returns {boolean}
     */
    sendDirect(oscData) {
        return this.#sendDirect(oscData);
    }

    /**
     * Send to prescheduler without classification (for callers who already know routing)
     * @param {Uint8Array} oscData
     * @returns {boolean}
     */
    sendToPrescheduler(oscData) {
        return this.#sendToPrescheduler(oscData);
    }

    // =========================================================================
    // Node ID Allocation
    // =========================================================================

    /**
     * Get the next unique node ID.
     *
     * SAB mode: single atomic increment — always correct, no batching needed.
     * PM mode: range-based allocation with async pre-fetching from main thread.
     *
     * @returns {number} A unique node ID (>= 1000)
     */
    nextNodeId() {
        // SAB mode: direct atomic increment, no ranges
        if (this.#nodeIdView) {
            return Atomics.add(this.#nodeIdView, 0, 1);
        }

        // PM mode: range-based allocation
        if (this.#nextNodeId >= this.#nodeIdTo) {
            this.#claimNodeIdRange();
        }
        const id = this.#nextNodeId++;
        // Pre-request next range when 1000 IDs remain (PM worker only)
        if (this.#nodeIdPort && !this.#pendingNodeIdRange &&
            (this.#nodeIdTo - this.#nextNodeId) <= 1000) {
            this.#requestNodeIdRange();
        }
        return id;
    }

    /**
     * Claim a new range of node IDs (PM mode only).
     * Main thread: use the direct source function.
     * Worker: use pre-fetched range from main thread.
     */
    #claimNodeIdRange() {
        if (this.#nodeIdSource) {
            // PM mode (main thread): direct range allocation
            const range = this.#nodeIdSource(this.#nodeIdRangeSize);
            this.#nodeIdFrom = range.from;
            this.#nodeIdTo = range.to;
            this.#nextNodeId = range.from;
        } else if (this.#pendingNodeIdRange) {
            // PM mode (worker): use pre-fetched range
            this.#nodeIdFrom = this.#pendingNodeIdRange.from;
            this.#nodeIdTo = this.#pendingNodeIdRange.to;
            this.#nextNodeId = this.#pendingNodeIdRange.from;
            this.#pendingNodeIdRange = null;
            // Pre-request the next range
            this.#requestNodeIdRange();
        } else if (this.#nodeIdPort) {
            // PM mode (worker): pre-fetched range hasn't arrived yet.
            // This only happens if IDs are consumed faster than the postMessage
            // round-trip — i.e. a tight synchronous loop of >9000 IDs without
            // yielding to the event loop. Request again and warn.
            console.warn('[OscChannel] nextNodeId() range exhausted before async refill arrived. IDs may not be unique. Yield to the event loop between large batches of nextNodeId() calls.');
            this.#requestNodeIdRange();
        }
    }

    /**
     * Request a new node ID range from the main thread via MessagePort.
     * Used by PM mode worker channels.
     */
    #requestNodeIdRange() {
        if (this.#nodeIdPort) {
            this.#nodeIdPort.postMessage({ type: 'requestNodeIdRange' });
        }
    }

    // =========================================================================
    // Properties
    // =========================================================================

    /**
     * Set the NTP time source for classification.
     * Use in AudioWorklet where performance.timeOrigin is unavailable.
     * @param {Function} fn - Returns current NTP time in seconds
     */
    set getCurrentNTP(fn) {
        this.#getCurrentNTP = fn;
    }

    /**
     * Get the transport mode
     * @returns {'sab' | 'postMessage'}
     */
    get mode() {
        return this.#mode;
    }

    /**
     * Get data needed to transfer this channel to a worker
     * Use with: worker.postMessage({ channel: oscChannel.transferable }, oscChannel.transferList)
     * @returns {Object} Serializable config object
     */
    get transferable() {
        const base = {
            mode: this.#mode,
            preschedulerPort: this.#preschedulerPort,
            bypassLookaheadS: this.#bypassLookaheadS,
            sourceId: this.#sourceId,
            blocking: this.#blocking,
        };

        if (this.#mode === 'postMessage') {
            // Claim a large initial range for the worker channel.
            // PM workers can't claim ranges synchronously (no SAB),
            // so we give them a generous initial allocation.
            // They can request more async via nodeIdPort.
            const workerRangeSize = this.#nodeIdRangeSize * 10;
            let nodeIdRange;
            let nodeIdPort;
            if (this.#nodeIdSource) {
                const range = this.#nodeIdSource(workerRangeSize);
                nodeIdRange = { from: range.from, to: range.to };

                // Create a MessageChannel for the worker to request more ranges
                const nodeIdChannel = new MessageChannel();
                const source = this.#nodeIdSource;
                const rangeSize = this.#nodeIdRangeSize;
                nodeIdChannel.port1.onmessage = (e) => {
                    if (e.data.type === 'requestNodeIdRange') {
                        const r = source(rangeSize);
                        nodeIdChannel.port1.postMessage({ type: 'nodeIdRange', from: r.from, to: r.to });
                    }
                };
                nodeIdPort = nodeIdChannel.port2;
                this.#transferNodeIdPort = nodeIdPort;
            }
            return {
                ...base,
                port: this.#directPort,
                nodeIdRange,
                nodeIdPort,
            };
        } else {
            return {
                ...base,
                sharedBuffer: this.#sabConfig.sharedBuffer,
                ringBufferBase: this.#sabConfig.ringBufferBase,
                bufferConstants: this.#sabConfig.bufferConstants,
                controlIndices: this.#sabConfig.controlIndices,
            };
        }
    }

    /**
     * Get the list of transferable objects for postMessage
     * @returns {Array} Array of transferable objects
     */
    get transferList() {
        const list = [];
        if (this.#mode === 'postMessage' && this.#directPort) {
            list.push(this.#directPort);
        }
        if (this.#preschedulerPort) {
            list.push(this.#preschedulerPort);
        }
        if (this.#transferNodeIdPort) {
            list.push(this.#transferNodeIdPort);
            this.#transferNodeIdPort = null;
        }
        return list;
    }

    /**
     * Close the channel
     */
    close() {
        if (this.#mode === 'postMessage' && this.#directPort) {
            this.#directPort.close();
            this.#directPort = null;
        }
        if (this.#preschedulerPort) {
            this.#preschedulerPort.close();
            this.#preschedulerPort = null;
        }
    }

    // =========================================================================
    // Static Factory Methods
    // =========================================================================

    /**
     * Create a postMessage-backed OscChannel
     * @private
     * @param {Object} config
     * @param {MessagePort} config.port - MessagePort connected to the worklet
     * @param {MessagePort} config.preschedulerPort - MessagePort to prescheduler
     * @param {number} [config.bypassLookaheadS=0.2] - Threshold for bypass routing (seconds)
     * @param {number} [config.sourceId=0] - Source ID (0 = main, 1+ = workers)
     * @param {boolean} [config.blocking] - Whether to use Atomics.wait() (default: true for sourceId !== 0)
     * @returns {OscChannel}
     */
    static createPostMessage(config) {
        // Support old API: createPostMessage(port)
        if (config instanceof MessagePort) {
            return new OscChannel('postMessage', { port: config });
        }
        return new OscChannel('postMessage', config);
    }

    /**
     * Create a SAB-backed OscChannel
     * @private
     * @param {Object} config
     * @param {SharedArrayBuffer} config.sharedBuffer
     * @param {number} config.ringBufferBase
     * @param {Object} config.bufferConstants
     * @param {Object} [config.controlIndices] - If not provided, will be calculated
     * @param {MessagePort} config.preschedulerPort - MessagePort to prescheduler
     * @param {number} [config.bypassLookaheadS=0.2] - Threshold for bypass routing (seconds)
     * @param {number} [config.sourceId=0] - Source ID (0 = main, 1+ = workers)
     * @param {boolean} [config.blocking] - Whether to use Atomics.wait() (default: true for sourceId !== 0)
     * @returns {OscChannel}
     */
    static createSAB(config) {
        // Calculate control indices if not provided
        let controlIndices = config.controlIndices;
        if (!controlIndices) {
            controlIndices = calculateInControlIndices(
                config.ringBufferBase,
                config.bufferConstants.CONTROL_START
            );
        }

        return new OscChannel('sab', {
            sharedBuffer: config.sharedBuffer,
            ringBufferBase: config.ringBufferBase,
            bufferConstants: config.bufferConstants,
            controlIndices,
            preschedulerPort: config.preschedulerPort,
            bypassLookaheadS: config.bypassLookaheadS,
            sourceId: config.sourceId,
            blocking: config.blocking,
        });
    }

    /**
     * Reconstruct an OscChannel from transferred data
     * Use in worker: const channel = OscChannel.fromTransferable(event.data.channel)
     * @param {Object} data - Data from transferable getter
     * @returns {OscChannel}
     */
    static fromTransferable(data) {
        if (data.mode === 'postMessage') {
            return new OscChannel('postMessage', {
                port: data.port,
                preschedulerPort: data.preschedulerPort,
                bypassLookaheadS: data.bypassLookaheadS,
                sourceId: data.sourceId,
                blocking: data.blocking,
                nodeIdRange: data.nodeIdRange,
                nodeIdPort: data.nodeIdPort,
            });
        } else {
            return new OscChannel('sab', {
                sharedBuffer: data.sharedBuffer,
                ringBufferBase: data.ringBufferBase,
                bufferConstants: data.bufferConstants,
                controlIndices: data.controlIndices,
                preschedulerPort: data.preschedulerPort,
                bypassLookaheadS: data.bypassLookaheadS,
                sourceId: data.sourceId,
                blocking: data.blocking,
            });
        }
    }
}

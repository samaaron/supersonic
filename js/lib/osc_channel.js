// SPDX-License-Identifier: MIT OR GPL-3.0-or-later
// Copyright (c) 2025 Sam Aaron

import { writeToRingBuffer, readMessagesFromBuffer } from './ring_buffer_core.js';
import * as MetricsOffsets from './metrics_offsets.js';
import { calculateInControlIndices } from './control_offsets.js';

/**
 * OscChannel - sends OSC to the audio worklet.
 *
 * A dumb transport: it frames the OSC bytes onto the IN ring (SAB mode) or
 * postMessages them to the worklet (postMessage mode). All classification and
 * scheduling happens on the audio thread (the engine's OscIngress + scheduler);
 * the producer never classifies.
 *
 * Works in both SAB and postMessage modes. Can be transferred to Web Workers
 * for direct communication with the AudioWorklet.
 */
export class OscChannel {
    #mode;
    #directPort;         // postMessage mode: MessagePort to worklet
    #sabConfig;          // SAB mode: { sharedBuffer, ringBufferBase, bufferConstants, controlIndices }
    #ring;               // SAB mode: cached IN-ring handle passed to writeToRingBuffer
    #metricsView;        // SAB mode: Int32Array view into metrics region
    #sourceId;           // Numeric source ID (0 = main thread, 1+ = workers)
    #blocking;           // Whether this channel can block with Atomics.wait()

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
    };

    /**
     * Private constructor - use static factory methods
     */
    constructor(mode, config) {
        this.#mode = mode;
        this.#sourceId = config.sourceId ?? 0;
        this.#blocking = config.blocking ?? (this.#sourceId !== 0);
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
        // Immutable IN-ring handle, assembled once and reused on every send.
        this.#ring = {
            atomicView: new Int32Array(sab),
            dataView: new DataView(sab),
            uint8View: new Uint8Array(sab),
            bufferConstants: this.#sabConfig.bufferConstants,
            ringBufferBase: this.#sabConfig.ringBufferBase,
            controlIndices: this.#sabConfig.controlIndices,
        };
    }

    // =========================================================================
    // Metrics
    // =========================================================================

    /**
     * Record a successful send — message + byte counts.
     * @param {number} byteCount - Size of the message in bytes
     */
    #recordSend(byteCount) {
        if (this.#mode === 'sab' && this.#metricsView) {
            Atomics.add(this.#metricsView, MetricsOffsets.OSC_OUT_MESSAGES_SENT, 1);
            Atomics.add(this.#metricsView, MetricsOffsets.OSC_OUT_BYTES_SENT, byteCount);
        } else {
            this.#localMetrics.messagesSent++;
            this.#localMetrics.bytesSent += byteCount;
        }
    }

    /**
     * Get and reset local metrics (for periodic reporting)
     * @returns {Object} Metrics snapshot
     */
    getAndResetMetrics() {
        const snapshot = { ...this.#localMetrics };
        this.#localMetrics = { messagesSent: 0, bytesSent: 0 };
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
            };
        }
        return { ...this.#localMetrics };
    }

    // =========================================================================
    // Sending
    // =========================================================================

    /**
     * Send an OSC message: frame it onto the IN ring (SAB) or postMessage it to
     * the worklet (PM). Classification and scheduling happen on the audio thread
     * (the engine's OscIngress + BundleScheduler) — the producer never classifies.
     *
     * In SAB mode, workers block (Atomics.wait) for guaranteed delivery; the main
     * thread cannot wait, so it spins a bounded number of times and returns false
     * on the rare lock contention.
     *
     * @param {Uint8Array} oscData - OSC message bytes
     * @returns {boolean} true if sent
     */
    send(oscData) {
        if (this.#mode === 'postMessage') {
            if (!this.#directPort) return false;
            this.#directPort.postMessage({ type: 'osc', oscData, sourceId: this.#sourceId });
            this.#recordSend(oscData.length);
            return true;
        }

        const success = writeToRingBuffer(this.#ring, oscData, this.#sourceId, this.#blocking);
        if (success) {
            this.#recordSend(oscData.length);
        } else if (this.#metricsView) {
            // Dropped: couldn't acquire the IN-ring lock within the spin budget,
            // or the ring was full. Counted so the loss isn't silent.
            Atomics.add(this.#metricsView, MetricsOffsets.RING_BUFFER_DIRECT_WRITE_FAILS, 1);
        }
        return success;
    }

    /**
     * Alias of {@link send} — kept for callers that used the explicit direct path.
     * @param {Uint8Array} oscData
     * @returns {boolean}
     */
    sendDirect(oscData) {
        return this.send(oscData);
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
        // Pre-request next range at halfway through current range (PM worker only)
        if (this.#nodeIdPort && !this.#pendingNodeIdRange &&
            (this.#nodeIdTo - this.#nextNodeId) <= (this.#nodeIdRangeSize >>> 1)) {
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
            // yielding to the event loop.
            throw new Error(
                '[OscChannel] Node ID range exhausted before async refill arrived. ' +
                'Yield to the event loop between large batches of nextNodeId() calls.'
            );
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
    }

    // =========================================================================
    // Static Factory Methods
    // =========================================================================

    /**
     * Create a postMessage-backed OscChannel
     * @private
     * @param {Object} config
     * @param {MessagePort} config.port - MessagePort connected to the worklet
     * @param {number} [config.sourceId=0] - Source ID (0 = main, 1+ = workers)
     * @param {boolean} [config.blocking] - Unused in postMessage mode (only applies to SAB transport)
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
                sourceId: data.sourceId,
                blocking: data.blocking,
            });
        }
    }
}

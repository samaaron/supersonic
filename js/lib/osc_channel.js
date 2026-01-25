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
            getCurrentNTP: getCurrentNTPFromPerformance,
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
     * Get current local metrics without resetting (for inspection)
     */
    getMetrics() {
        return { ...this.#localMetrics };
    }

    // =========================================================================
    // Sending
    // =========================================================================

    /**
     * Send directly to worklet (bypass path)
     * @param {Uint8Array} oscData
     * @param {string} [bypassCategory] - Category for metrics (PM mode only)
     */
    #sendDirect(oscData, bypassCategory = null) {
        if (this.#mode === 'postMessage') {
            if (!this.#directPort) return false;
            // Include bypass category and sourceId so worklet can track metrics and log source
            this.#directPort.postMessage({ type: 'osc', oscData, bypassCategory, sourceId: this.#sourceId });
            return true;
        } else {
            // SAB mode - write to ring buffer with sourceId in header
            return writeToRingBuffer({
                atomicView: this.#views.atomicView,
                dataView: this.#views.dataView,
                uint8View: this.#views.uint8View,
                bufferConstants: this.#sabConfig.bufferConstants,
                ringBufferBase: this.#sabConfig.ringBufferBase,
                controlIndices: this.#sabConfig.controlIndices,
                oscMessage: oscData,
                sourceId: this.#sourceId,
                maxSpins: 10,  // Workers can spin briefly
            });
        }
    }

    /**
     * Send to prescheduler (far-future path)
     */
    #sendToPrescheduler(oscData) {
        if (!this.#preschedulerPort) {
            // Fallback: send direct if no prescheduler port
            // This shouldn't happen in normal usage
            console.warn('[OscChannel] No prescheduler port, sending direct');
            return this.#sendDirect(oscData);
        }
        this.#preschedulerPort.postMessage({ type: 'osc', oscData });
        return true;
    }

    /**
     * Send OSC message with automatic routing
     *
     * - nonBundle/immediate/nearFuture/late → direct to worklet
     * - farFuture → prescheduler for proper scheduling
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
            preschedulerPort: this.#preschedulerPort,
            bypassLookaheadS: this.#bypassLookaheadS,
            sourceId: this.#sourceId,
        };

        if (this.#mode === 'postMessage') {
            return {
                ...base,
                port: this.#directPort,
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
     * @param {Object} config
     * @param {MessagePort} config.port - MessagePort connected to the worklet
     * @param {MessagePort} config.preschedulerPort - MessagePort to prescheduler
     * @param {number} [config.bypassLookaheadS=0.2] - Threshold for bypass routing (seconds)
     * @param {number} [config.sourceId=0] - Source ID (0 = main, 1+ = workers)
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
     * @param {Object} config
     * @param {SharedArrayBuffer} config.sharedBuffer
     * @param {number} config.ringBufferBase
     * @param {Object} config.bufferConstants
     * @param {Object} [config.controlIndices] - If not provided, will be calculated
     * @param {MessagePort} config.preschedulerPort - MessagePort to prescheduler
     * @param {number} [config.bypassLookaheadS=0.2] - Threshold for bypass routing (seconds)
     * @param {number} [config.sourceId=0] - Source ID (0 = main, 1+ = workers)
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
            });
        }
    }
}

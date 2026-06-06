// SPDX-License-Identifier: MIT OR GPL-3.0-or-later
// Copyright (c) 2025 Sam Aaron

import { Transport } from './transport.js';
import { createWorker } from '../worker_loader.js';
import { OscChannel } from '../osc_channel.js';
import { getCurrentNTPFromPerformance } from '../osc_classifier.js';

/**
 * PostMessage Transport
 *
 * Uses MessagePort for communication with the audio worklet.
 * All OSC messages are sent via postMessage and queued in the worklet.
 * Requires larger scheduling lookahead to absorb message passing latency.
 *
 * Architecture:
 * - OSC OUT: Prescheduler worker dispatches via direct MessageChannel to worklet
 * - OSC IN: Worklet sends replies via MessagePort
 * - DEBUG: Worklet sends debug messages via MessagePort
 * - Reuses existing prescheduler worker with mode='postMessage'
 */
export class PostMessageTransport extends Transport {
    #workletPort;
    #workerBaseURL;

    // Callbacks
    #onReplyCallback;
    #onDebugCallback;
    #onErrorCallback;
    #onOscLogCallback;

    // Source ID tracking (0 = main thread, 1+ = workers)
    #nextSourceId = 1;

    // Lazily-created main-thread channel for the transport's own send()
    #mainChannel = null;

    // State
    #initialized = false;
    #snapshotIntervalMs;
    #bufferConstants = null;

    // Metrics (using canonical names matching metrics_offsets.js)
    #oscOutMessagesSent = 0;
    #oscInMessagesDropped = 0;
    #oscOutBytesSent = 0;
    #oscInMessagesReceived = 0;
    #oscInBytesReceived = 0;
    #lastSequenceReceived = -1;
    #debugMessagesReceived = 0;
    #debugBytesReceived = 0;

    // Timing functions
    #getAudioContextTime;
    #getNTPStartTime;

    /**
     * @param {Object} config
     * @param {string} config.workerBaseURL - Base URL for worker scripts
     * @param {Function} config.getAudioContextTime - Returns AudioContext.currentTime
     * @param {Function} config.getNTPStartTime - Returns NTP start time
     * @param {number} [config.snapshotIntervalMs] - Interval for metrics/tree snapshots
     */
    constructor(config) {
        super({ ...config, mode: 'postMessage' });

        this.#workerBaseURL = config.workerBaseURL;
        this.#snapshotIntervalMs = config.snapshotIntervalMs;
        this.#getAudioContextTime = config.getAudioContextTime;
        this.#getNTPStartTime = config.getNTPStartTime;
    }

    /**
     * Initialize the transport
     * @param {MessagePort} workletPort - Port connected to the audio worklet
     */
    async initialize(workletPort) {
        if (this.#initialized) {
            if (__DEV__) console.warn('[PostMessageTransport] Already initialized');
            return;
        }

        if (!workletPort) {
            throw new Error('PostMessageTransport requires workletPort');
        }

        this.#workletPort = workletPort;

        // Set up message handler for worklet responses
        this.#workletPort.onmessage = (event) => {
            this.#handleWorkletMessage(event.data);
        };

        // No prescheduler: producers postMessage OSC straight to the worklet,
        // which classifies + schedules on the audio thread.
        this.#initialized = true;
    }

    /**
     * Set buffer constants
     * Called by SuperSonic after receiving bufferConstants from worklet
     * @param {Object} bufferConstants
     */
    setBufferConstants(bufferConstants) {
        this.#bufferConstants = bufferConstants;
    }

    /**
     * Send an OSC message by postMessaging it to the worklet (via an internal
     * main-thread channel). The audio thread classifies + schedules.
     */
    send(message) {
        if (!this.#initialized || this._disposed) {
            return false;
        }
        if (!this.#mainChannel) this.#mainChannel = this.createOscChannel({ sourceId: 0 });
        const ok = this.#mainChannel.send(message);
        if (ok) {
            this.#oscOutMessagesSent++;
            this.#oscOutBytesSent += message.length;
        }
        return ok;
    }

    /**
     * Create an OscChannel for direct worker-to-worklet communication
     *
     * Returns an OscChannel that can be transferred to a Web Worker,
     * allowing that worker to send OSC messages directly to the AudioWorklet
     * without going through the main thread.
     *
     * Usage:
     *   const channel = transport.createOscChannel();
     *   myWorker.postMessage({ channel: channel.transferable }, channel.transferList);
     *
     * In worker:
     *   const channel = OscChannel.fromTransferable(event.data.channel);
     *   channel.send(oscBytes);
     *
     * @param {Object} [options]
     * @param {number} [options.sourceId] - Override sourceId (default: auto-assign)
     * @returns {OscChannel}
     */
    createOscChannel(options = {}) {
        if (!this.#initialized) {
            throw new Error('Transport not initialized');
        }

        // Use provided sourceId or auto-assign
        // sourceId 0 is reserved for main thread, 1+ for workers
        const sourceId = options.sourceId ?? this.#nextSourceId++;

        // Create a MessageChannel for direct worklet communication
        const directChannel = new MessageChannel();

        // Register one port with the worklet, including sourceId for logging
        this.#workletPort.postMessage(
            { type: 'addOscPort', sourceId },
            [directChannel.port1]
        );

        return OscChannel.createPostMessage({
            port: directChannel.port2,
            sourceId,
            blocking: options.blocking,
            nodeIdSource: this._config.nodeIdSource,
        });
    }

    onReply(callback) {
        this.#onReplyCallback = callback;
    }

    onDebug(callback) {
        this.#onDebugCallback = callback;
    }

    onError(callback) {
        this.#onErrorCallback = callback;
    }

    onOscLog(callback) {
        this.#onOscLogCallback = callback;
    }

    /**
     * Handle raw debug bytes from worklet (postMessage mode)
     * Decodes UTF-8 text and forwards to callback
     * Supports packed buffer format: { messages: [...], count: N, buffer: ArrayBuffer }
     * @param {Object} data - Debug message batch
     */
    handleDebugRaw(data) {
        // Decode debug messages inline using TextDecoder
        // (decoding happens here to avoid allocations on the real-time audio thread)
        if (data.messages && data.count > 0 && data.buffer) {
            // New packed buffer format
            const textDecoder = new TextDecoder('utf-8');
            const debugBuffer = new Uint8Array(data.buffer);
            for (let i = 0; i < data.count; i++) {
                const entry = data.messages[i];
                try {
                    const bytes = debugBuffer.subarray(entry.offset, entry.offset + entry.length);
                    let text = textDecoder.decode(bytes);
                    if (text.endsWith('\n')) {
                        text = text.slice(0, -1);
                    }
                    // Track debug metrics
                    this.#debugMessagesReceived++;
                    this.#debugBytesReceived += entry.length;
                    if (this.#onDebugCallback) {
                        this.#onDebugCallback({
                            text: text,
                            timestamp: performance.now(),
                            sequence: entry.sequence
                        });
                    }
                } catch (err) {
                    console.error('[PostMessageTransport] Failed to decode debug message:', err);
                }
            }
        }
    }

    getMetrics() {
        // Note: oscOutMessagesSent, oscOutBytesSent, and bypass metrics are NOT included here.
        // In PM mode, the worklet counts all received messages (from main thread + OscChannel workers)
        // and reports them in the snapshot buffer. Including them here would overwrite the worklet's count.
        return {
            oscInMessagesReceived: this.#oscInMessagesReceived,
            oscInBytesReceived: this.#oscInBytesReceived,
            oscInMessagesDropped: this.#oscInMessagesDropped,
            debugMessagesReceived: this.#debugMessagesReceived,
            debugBytesReceived: this.#debugBytesReceived,
        };
    }

    get ready() {
        return this.#initialized && !this._disposed;
    }

    dispose() {
        if (this._disposed) return;

        this.#workletPort = null;
        this.#initialized = false;
        super.dispose();
    }

    // =========================================================================
    // Private Methods
    // =========================================================================

    #handleWorkletMessage(data) {
        switch (data.type) {
            case 'oscReplies':
                // OSC replies from scsynth (packed buffer format)
                // Format: { messages: [...entries], count: N, buffer: ArrayBuffer }
                if (data.messages && data.count > 0 && data.buffer) {
                    const replyBuffer = new Uint8Array(data.buffer);
                    for (let i = 0; i < data.count; i++) {
                        const entry = data.messages[i];
                        // entry has { offset, length, sequence }
                        const oscData = replyBuffer.subarray(entry.offset, entry.offset + entry.length);

                        // Check for dropped messages via sequence gaps
                        if (entry.sequence !== undefined && this.#lastSequenceReceived >= 0) {
                            const expectedSeq = (this.#lastSequenceReceived + 1) & 0xFFFFFFFF;
                            if (entry.sequence !== expectedSeq) {
                                const dropped = (entry.sequence - expectedSeq + 0x100000000) & 0xFFFFFFFF;
                                if (dropped < 1000) { // Sanity check
                                    this.#oscInMessagesDropped += dropped;
                                }
                            }
                        }
                        if (entry.sequence !== undefined) {
                            this.#lastSequenceReceived = entry.sequence;
                        }

                        this.#oscInMessagesReceived++;
                        this.#oscInBytesReceived += entry.length;
                        if (this.#onReplyCallback) {
                            this.#onReplyCallback(oscData, entry.sequence, getCurrentNTPFromPerformance());
                        }
                    }
                }
                break;

            case 'metrics':
                // Metrics update from worklet (periodic)
                // Could store/emit these
                break;

            case 'bufferLoaded':
                // Buffer load acknowledgment
                // Handled by buffer manager
                break;

            case 'debugRawBatch':
                // Debug messages from worklet (packed buffer format)
                this.handleDebugRaw(data);
                break;

            case 'oscLog':
                // OSC log entries from worklet (packed buffer format)
                // Format: { entries: [...], count: N, buffer: ArrayBuffer }
                if (__DEV__) {
                    console.log('[PostMessageTransport] oscLog received:', {
                        hasCallback: !!this.#onOscLogCallback,
                        count: data.count,
                        hasBuffer: !!data.buffer,
                        hasEntries: !!data.entries,
                        entriesLength: data.entries?.length,
                    });
                }
                if (this.#onOscLogCallback) {
                    if (data.count > 0 && data.buffer && data.entries) {
                        // New packed buffer format
                        const logBuffer = new Uint8Array(data.buffer);
                        const entries = [];
                        for (let i = 0; i < data.count; i++) {
                            const entry = data.entries[i];
                            // entry has { offset, length, originalLength, sourceId, sequence }
                            const oscData = logBuffer.subarray(entry.offset, entry.offset + entry.length);
                            entries.push({
                                oscData,
                                sourceId: entry.sourceId,
                                sequence: entry.sequence,
                                timestamp: getCurrentNTPFromPerformance(),
                                truncated: entry.length < entry.originalLength,
                                originalLength: entry.originalLength,
                            });
                        }
                        this.#onOscLogCallback(entries);
                    }
                }
                break;

            case 'error':
                console.error('[PostMessageTransport] Worklet error:', data.error);
                this.#oscInMessagesDropped++;
                if (this.#onErrorCallback) {
                    this.#onErrorCallback(data.error, 'worklet');
                }
                break;

            case 'debug':
                // Debug message from worklet
                if (__DEV__) console.log('[PostMessageTransport] Worklet debug:', data.message);
                break;
        }
    }

}

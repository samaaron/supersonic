// SPDX-License-Identifier: MIT OR GPL-3.0-or-later
// Copyright (c) 2025 Sam Aaron

import { Transport } from './transport.js';
import { createWorker } from '../worker_loader.js';
import { OscChannel } from '../osc_channel.js';

/**
 * PostMessage Transport
 *
 * Uses MessagePort for communication with the audio worklet.
 * All OSC messages are sent via postMessage and queued in the worklet.
 * Requires larger scheduling lookahead to absorb message passing latency.
 *
 * Architecture:
 * - OSC OUT: Prescheduler worker dispatches via postMessage, we forward to worklet
 * - OSC IN: Worklet sends replies via MessagePort
 * - DEBUG: Worklet sends debug messages via MessagePort
 * - Reuses existing prescheduler worker with mode='postMessage'
 */
export class PostMessageTransport extends Transport {
    #workletPort;
    #preschedulerWorker;
    #workerBaseURL;

    // Callbacks
    #onReplyCallback;
    #onDebugCallback;
    #onErrorCallback;
    #onOscLogCallback;

    // Source ID tracking (0 = main thread, 1+ = workers)
    #nextSourceId = 1;

    // Cached prescheduler metrics (prescheduler sends via postMessage)
    #cachedPreschedulerMetrics = null;

    // State
    #initialized = false;
    #preschedulerCapacity;
    #snapshotIntervalMs;

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
     * @param {number} [config.preschedulerCapacity=65536] - Max pending messages
     * @param {number} [config.snapshotIntervalMs] - Interval for metrics/tree snapshots
     */
    constructor(config) {
        super({ ...config, mode: 'postMessage' });

        this.#workerBaseURL = config.workerBaseURL;
        this.#preschedulerCapacity = config.preschedulerCapacity || 65536;
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
            console.warn('[PostMessageTransport] Already initialized');
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

        // Create a MessageChannel for prescheduler -> worklet direct communication
        // This bypasses the main thread relay for scheduled OSC messages
        const preschedulerChannel = new MessageChannel();

        // Register one port with the worklet
        this.#workletPort.postMessage(
            { type: 'addOscPort' },
            [preschedulerChannel.port1]
        );

        // Create the prescheduler worker
        // Uses Blob URL if cross-origin (enables CDN-only deployment)
        this.#preschedulerWorker = await createWorker(
            this.#workerBaseURL + 'osc_out_prescheduler_worker.js',
            { type: 'module' }
        );

        this.#preschedulerWorker.onmessage = (event) => {
            this.#handlePreschedulerMessage(event.data);
        };

        // Initialize prescheduler worker with the other port for direct worklet communication
        await this.#initPreschedulerWorker(preschedulerChannel.port2);

        this.#initialized = true;
    }

    /**
     * Send OSC message via prescheduler
     */
    send(message, timestamp) {
        if (!this.#initialized || this._disposed) {
            return false;
        }

        // Send to prescheduler for timing coordination
        this.#preschedulerWorker.postMessage({
            type: 'send',
            oscData: message,
            sessionId: 0,
            runTag: '',
            audioTimeS: null,
            currentTimeS: null,
        });

        this.#oscOutMessagesSent++;
        this.#oscOutBytesSent += message.length;
        return true;
    }

    /**
     * Send with full options
     */
    sendWithOptions(message, options = {}) {
        if (!this.#initialized || this._disposed) {
            return false;
        }

        const { sessionId = 0, runTag = '', audioTimeS = null, currentTimeS = null } = options;

        this.#preschedulerWorker.postMessage({
            type: 'send',
            oscData: message,
            sessionId,
            runTag,
            audioTimeS,
            currentTimeS,
        });

        this.#oscOutMessagesSent++;
        this.#oscOutBytesSent += message.length;
        return true;
    }

    /**
     * Send immediately, bypassing prescheduler
     * @param {Uint8Array} message - OSC message data
     * @param {string} [category] - Bypass category: 'nonBundle', 'immediate', 'nearFuture', or 'late'
     */
    sendImmediate(message, category) {
        if (!this.#initialized || this._disposed) {
            return false;
        }

        // Send directly to worklet, bypassing prescheduler
        // Include bypassCategory so worklet can track all bypass metrics (from main thread + workers)
        this.#workletPort.postMessage({
            type: 'osc',
            oscData: message,
            bypassCategory: category,
        });

        this.#oscOutMessagesSent++;
        this.#oscOutBytesSent += message.length;

        return true;
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

        // Create a MessageChannel for prescheduler communication
        const preschedulerChannel = new MessageChannel();

        // Register one port with the prescheduler worker
        this.#preschedulerWorker.postMessage(
            { type: 'addOscSource' },
            [preschedulerChannel.port1]
        );

        // Return OscChannel with both direct and prescheduler paths
        return OscChannel.createPostMessage({
            port: directChannel.port2,
            preschedulerPort: preschedulerChannel.port2,
            bypassLookaheadS: this._config.bypassLookaheadS,
            sourceId,
        });
    }

    /**
     * Cancel by session and tag
     */
    cancelSessionTag(sessionId, runTag) {
        if (!this.#initialized) return;
        this.#preschedulerWorker.postMessage({ type: 'cancelSessionTag', sessionId, runTag });
    }

    /**
     * Cancel by session
     */
    cancelSession(sessionId) {
        if (!this.#initialized) return;
        this.#preschedulerWorker.postMessage({ type: 'cancelSession', sessionId });
    }

    /**
     * Cancel by tag
     */
    cancelTag(runTag) {
        if (!this.#initialized) return;
        this.#preschedulerWorker.postMessage({ type: 'cancelTag', runTag });
    }

    /**
     * Cancel all
     */
    cancelAll() {
        if (!this.#initialized) return;
        this.#preschedulerWorker.postMessage({ type: 'cancelAll' });
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
     * @param {Object} data - { messages: Array<{bytes, sequence}> }
     */
    handleDebugRaw(data) {
        // Decode debug messages inline using TextDecoder
        // (TextDecoder not available in AudioWorklet, so decoding happens here)
        if (data.messages) {
            const textDecoder = new TextDecoder('utf-8');
            for (const raw of data.messages) {
                try {
                    const bytes = new Uint8Array(raw.bytes);
                    let text = textDecoder.decode(bytes);
                    if (text.endsWith('\n')) {
                        text = text.slice(0, -1);
                    }
                    // Track debug metrics
                    this.#debugMessagesReceived++;
                    this.#debugBytesReceived += bytes.length;
                    if (this.#onDebugCallback) {
                        this.#onDebugCallback({
                            text: text,
                            timestamp: performance.now(),
                            sequence: raw.sequence
                        });
                    }
                } catch (err) {
                    console.error('[PostMessageTransport] Failed to decode debug message:', err);
                }
            }
        }
    }

    /**
     * Get cached prescheduler metrics (postMessage mode only)
     * @returns {Uint32Array|null}
     */
    getPreschedulerMetrics() {
        return this.#cachedPreschedulerMetrics;
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

        if (this.#preschedulerWorker) {
            this.#preschedulerWorker.postMessage({ type: 'stop' });
            this.#preschedulerWorker.terminate();
            this.#preschedulerWorker = null;
        }

        this.#workletPort = null;
        this.#initialized = false;
        super.dispose();
    }

    // =========================================================================
    // Private Methods
    // =========================================================================

    #initPreschedulerWorker(workletPort) {
        return new Promise((resolve, reject) => {
            const timeout = setTimeout(() => {
                reject(new Error('Prescheduler worker initialization timeout'));
            }, 5000);

            const handler = (event) => {
                if (event.data.type === 'initialized') {
                    clearTimeout(timeout);
                    this.#preschedulerWorker.removeEventListener('message', handler);
                    resolve();
                }
            };

            this.#preschedulerWorker.addEventListener('message', handler);

            // Transfer the worklet port to prescheduler for direct communication
            this.#preschedulerWorker.postMessage({
                type: 'init',
                mode: 'postMessage',  // Use postMessage dispatch mode
                maxPendingMessages: this.#preschedulerCapacity,
                snapshotIntervalMs: this.#snapshotIntervalMs,
                workletPort: workletPort,  // Direct port to worklet
            }, [workletPort]);  // Transfer the port
        });
    }

    #handleWorkletMessage(data) {
        switch (data.type) {
            case 'oscReplies':
                // OSC replies from scsynth (batch of messages)
                if (data.messages) {
                    for (const msg of data.messages) {
                        if (msg.oscData) {
                            // Check for dropped messages via sequence gaps
                            if (msg.sequence !== undefined && this.#lastSequenceReceived >= 0) {
                                const expectedSeq = (this.#lastSequenceReceived + 1) & 0xFFFFFFFF;
                                if (msg.sequence !== expectedSeq) {
                                    const dropped = (msg.sequence - expectedSeq + 0x100000000) & 0xFFFFFFFF;
                                    if (dropped < 1000) { // Sanity check
                                        this.#oscInMessagesDropped += dropped;
                                    }
                                }
                            }
                            if (msg.sequence !== undefined) {
                                this.#lastSequenceReceived = msg.sequence;
                            }

                            this.#oscInMessagesReceived++;
                            this.#oscInBytesReceived += msg.oscData.byteLength || msg.oscData.length || 0;
                            if (this.#onReplyCallback) {
                                this.#onReplyCallback(msg.oscData, msg.sequence);
                            }
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

            case 'oscLog':
                // OSC log from worklet (centralized logging)
                if (data.entries && this.#onOscLogCallback) {
                    this.#onOscLogCallback(data.entries);
                }
                break;

            case 'error':
                console.error('[PostMessageTransport] Worklet error:', data.error);
                this.#oscInMessagesDropped++;
                if (this.#onErrorCallback) {
                    this.#onErrorCallback(data.error, 'worklet');
                }
                break;
        }
    }

    #handlePreschedulerMessage(data) {
        switch (data.type) {
            // Note: 'dispatch' no longer comes here - prescheduler sends directly to worklet

            case 'preschedulerMetrics':
                // Cache prescheduler metrics for retrieval
                this.#cachedPreschedulerMetrics = data.metrics;
                break;

            case 'error':
                console.error('[PostMessageTransport] Prescheduler error:', data.error);
                this.#oscInMessagesDropped++;
                if (this.#onErrorCallback) {
                    this.#onErrorCallback(data.error, 'oscOut');
                }
                break;
        }
    }
}

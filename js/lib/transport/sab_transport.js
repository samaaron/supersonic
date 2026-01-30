// SPDX-License-Identifier: MIT OR GPL-3.0-or-later
// Copyright (c) 2025 Sam Aaron

import { Transport } from './transport.js';
import { createWorker } from '../worker_loader.js';
import { OscChannel } from '../osc_channel.js';
import { calculateInControlIndices } from '../control_offsets.js';

/**
 * SAB (SharedArrayBuffer) Transport
 *
 * Uses ring buffers in shared memory for high-performance, low-latency
 * OSC message passing. Requires crossOriginIsolated context.
 *
 * Architecture:
 * - OSC OUT: DirectWriter for immediate messages, prescheduler worker for future bundles
 * - OSC IN: Worker polls OUT ring buffer, forwards to callbacks
 * - DEBUG: Worker polls DEBUG ring buffer, forwards to callbacks
 */
export class SABTransport extends Transport {
    #sharedBuffer;
    #ringBufferBase;
    #bufferConstants;

    // Cached views for ring buffer access
    #atomicView;
    #dataView;
    #uint8View;
    #controlIndices;

    // Workers
    #oscOutWorker;
    #oscInWorker;
    #debugWorker;
    #oscOutLogWorker;
    #workerBaseURL;

    // Callbacks
    #onReplyCallback;
    #onDebugCallback;
    #onErrorCallback;
    #onOscLogCallback;

    // Source ID tracking (0 = main thread, 1+ = workers)
    #nextSourceId = 1;

    // State
    #initialized = false;
    #preschedulerCapacity;

    // Metrics (using canonical names matching metrics_offsets.js)
    #oscOutMessagesSent = 0;
    #oscOutMessagesDropped = 0;
    #oscOutBytesSent = 0;

    // Cached prescheduler metrics (worker sends via postMessage)
    #cachedPreschedulerMetrics = null;

    /**
     * @param {Object} config
     * @param {SharedArrayBuffer} config.sharedBuffer
     * @param {number} config.ringBufferBase
     * @param {Object} config.bufferConstants
     * @param {string} config.workerBaseURL
     * @param {Function} config.getAudioContextTime
     * @param {Function} config.getNTPStartTime
     * @param {number} [config.preschedulerCapacity=65536]
     */
    constructor(config) {
        super({ ...config, mode: 'sab' });

        this.#sharedBuffer = config.sharedBuffer;
        this.#ringBufferBase = config.ringBufferBase;
        this.#bufferConstants = config.bufferConstants;
        this.#workerBaseURL = config.workerBaseURL;
        this.#preschedulerCapacity = config.preschedulerCapacity || 65536;

        // Validate SAB requirements
        if (!(this.#sharedBuffer instanceof SharedArrayBuffer)) {
            throw new Error('SABTransport requires a SharedArrayBuffer');
        }

        // Initialize typed array views
        this.#initializeViews();
    }

    /**
     * Initialize the transport - spawns workers
     */
    async initialize() {
        if (this.#initialized) {
            console.warn('[SABTransport] Already initialized');
            return;
        }

        // Spawn workers (uses Blob URL if cross-origin)
        const [oscOutWorker, oscInWorker, debugWorker, oscOutLogWorker] = await Promise.all([
            createWorker(this.#workerBaseURL + 'osc_out_prescheduler_worker.js', { type: 'module' }),
            createWorker(this.#workerBaseURL + 'osc_in_worker.js', { type: 'module' }),
            createWorker(this.#workerBaseURL + 'debug_worker.js', { type: 'module' }),
            createWorker(this.#workerBaseURL + 'osc_out_log_sab_worker.js', { type: 'module' })
        ]);
        this.#oscOutWorker = oscOutWorker;
        this.#oscInWorker = oscInWorker;
        this.#debugWorker = debugWorker;
        this.#oscOutLogWorker = oscOutLogWorker;

        // Set up message handlers
        this.#setupWorkerHandlers();

        // Initialize workers with shared buffer
        await Promise.all([
            this.#initWorker(this.#oscOutWorker, 'OSC OUT', {
                maxPendingMessages: this.#preschedulerCapacity,
                bypassLookaheadS: this._config.bypassLookaheadS,
            }),
            this.#initWorker(this.#oscInWorker, 'OSC IN'),
            this.#initWorker(this.#debugWorker, 'DEBUG'),
            this.#initWorker(this.#oscOutLogWorker, 'OSC OUT LOG'),
        ]);

        // Start polling workers
        this.#oscInWorker.postMessage({ type: 'start' });
        this.#debugWorker.postMessage({ type: 'start' });
        this.#oscOutLogWorker.postMessage({ type: 'start' });

        this.#initialized = true;
    }

    /**
     * Send OSC message via prescheduler worker
     */
    send(message, timestamp) {
        if (!this.#initialized || this._disposed) {
            return false;
        }

        this.#oscOutWorker.postMessage({
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
     * Send with full options (sessionId, runTag, timing)
     */
    sendWithOptions(message, options = {}) {
        if (!this.#initialized || this._disposed) {
            return false;
        }

        const { sessionId = 0, runTag = '', audioTimeS = null, currentTimeS = null } = options;

        this.#oscOutWorker.postMessage({
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
     * Send immediately, ignoring bundle timestamps
     */
    sendImmediate(message) {
        if (!this.#initialized || this._disposed) {
            return false;
        }

        this.#oscOutWorker.postMessage({
            type: 'sendImmediate',
            oscData: message,
        });

        this.#oscOutMessagesSent++;
        this.#oscOutBytesSent += message.length;
        return true;
    }

    /**
     * Create an OscChannel for direct worker-to-worklet communication
     *
     * Returns an OscChannel backed by the SharedArrayBuffer that can be
     * transferred to a Web Worker, allowing that worker to send OSC messages
     * directly to the AudioWorklet's ring buffer.
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

        // Create a MessageChannel for prescheduler communication
        const preschedulerChannel = new MessageChannel();

        // Register one port with the prescheduler worker
        this.#oscOutWorker.postMessage(
            { type: 'addOscSource' },
            [preschedulerChannel.port1]
        );

        // Return OscChannel with both direct (SAB) and prescheduler paths
        return OscChannel.createSAB({
            sharedBuffer: this.#sharedBuffer,
            ringBufferBase: this.#ringBufferBase,
            bufferConstants: this.#bufferConstants,
            controlIndices: this.#controlIndices,
            preschedulerPort: preschedulerChannel.port2,
            bypassLookaheadS: this._config.bypassLookaheadS,
            sourceId,
        });
    }

    /**
     * Cancel scheduled messages by session and tag
     */
    cancelSessionTag(sessionId, runTag) {
        if (!this.#initialized) return;
        this.#oscOutWorker.postMessage({ type: 'cancelSessionTag', sessionId, runTag });
    }

    /**
     * Cancel all messages from a session
     */
    cancelSession(sessionId) {
        if (!this.#initialized) return;
        this.#oscOutWorker.postMessage({ type: 'cancelSession', sessionId });
    }

    /**
     * Cancel all messages with a tag
     */
    cancelTag(runTag) {
        if (!this.#initialized) return;
        this.#oscOutWorker.postMessage({ type: 'cancelTag', runTag });
    }

    /**
     * Cancel all scheduled messages
     */
    cancelAll() {
        if (!this.#initialized) return;
        this.#oscOutWorker.postMessage({ type: 'cancelAll' });
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
     * Handle OSC log entries from worklet
     * In SAB mode, SuperSonic forwards these from the worklet port
     * @param {Array} entries - Array of {sourceId, oscData, timestamp}
     */
    handleOscLog(entries) {
        if (this.#onOscLogCallback) {
            this.#onOscLogCallback(entries);
        }
    }

    getMetrics() {
        return {
            oscOutMessagesSent: this.#oscOutMessagesSent,
            oscOutMessagesDropped: this.#oscOutMessagesDropped,
            oscOutBytesSent: this.#oscOutBytesSent,
        };
    }

    get ready() {
        return this.#initialized && !this._disposed;
    }

    dispose() {
        if (this._disposed) return;

        // Stop and terminate workers
        if (this.#oscOutWorker) {
            this.#oscOutWorker.postMessage({ type: 'stop' });
            this.#oscOutWorker.terminate();
            this.#oscOutWorker = null;
        }
        if (this.#oscInWorker) {
            this.#oscInWorker.postMessage({ type: 'stop' });
            this.#oscInWorker.terminate();
            this.#oscInWorker = null;
        }
        if (this.#debugWorker) {
            this.#debugWorker.postMessage({ type: 'stop' });
            this.#debugWorker.terminate();
            this.#debugWorker = null;
        }
        if (this.#oscOutLogWorker) {
            this.#oscOutLogWorker.postMessage({ type: 'stop' });
            this.#oscOutLogWorker.terminate();
            this.#oscOutLogWorker = null;
        }

        this.#initialized = false;
        super.dispose();
    }

    // =========================================================================
    // Private Methods
    // =========================================================================

    #initializeViews() {
        this.#atomicView = new Int32Array(this.#sharedBuffer);
        this.#dataView = new DataView(this.#sharedBuffer);
        this.#uint8View = new Uint8Array(this.#sharedBuffer);

        this.#controlIndices = calculateInControlIndices(
            this.#ringBufferBase,
            this.#bufferConstants.CONTROL_START
        );
    }

    #initWorker(worker, name, extraConfig = {}) {
        return new Promise((resolve, reject) => {
            const timeout = setTimeout(() => {
                reject(new Error(`${name} worker initialization timeout`));
            }, 5000);

            const handler = (event) => {
                if (event.data.type === 'initialized') {
                    clearTimeout(timeout);
                    worker.removeEventListener('message', handler);
                    resolve();
                }
            };

            worker.addEventListener('message', handler);
            worker.postMessage({
                type: 'init',
                sharedBuffer: this.#sharedBuffer,
                ringBufferBase: this.#ringBufferBase,
                bufferConstants: this.#bufferConstants,
                ...extraConfig,
            });
        });
    }

    #setupWorkerHandlers() {
        // OSC IN worker - receives replies from scsynth
        this.#oscInWorker.onmessage = (event) => {
            const data = event.data;
            if (data.type === 'messages' && this.#onReplyCallback) {
                data.messages.forEach(msg => {
                    if (msg.oscData) {
                        this.#onReplyCallback(msg.oscData, msg.sequence);
                    }
                });
            } else if (data.type === 'error') {
                console.error('[SABTransport] OSC IN error:', data.error);
                if (this.#onErrorCallback) {
                    this.#onErrorCallback(data.error, 'oscIn');
                }
            }
        };

        // DEBUG worker - receives debug messages
        this.#debugWorker.onmessage = (event) => {
            const data = event.data;
            if (data.type === 'debug' && this.#onDebugCallback) {
                data.messages.forEach(msg => {
                    this.#onDebugCallback(msg);
                });
            } else if (data.type === 'error') {
                console.error('[SABTransport] DEBUG error:', data.error);
                if (this.#onErrorCallback) {
                    this.#onErrorCallback(data.error, 'debug');
                }
            }
        };

        // OSC OUT worker - handles metrics and errors
        this.#oscOutWorker.onmessage = (event) => {
            const data = event.data;
            if (data.type === 'preschedulerMetrics') {
                this.#cachedPreschedulerMetrics = data.metrics;
            } else if (data.type === 'error') {
                console.error('[SABTransport] OSC OUT error:', data.error);
                this.#oscOutMessagesDropped++;
                if (this.#onErrorCallback) {
                    this.#onErrorCallback(data.error, 'oscOut');
                }
            }
        };

        // OSC OUT LOG worker - receives logged OSC messages
        this.#oscOutLogWorker.onmessage = (event) => {
            const data = event.data;
            if (data.type === 'oscLog' && this.#onOscLogCallback) {
                this.#onOscLogCallback(data.entries);
            } else if (data.type === 'error') {
                console.error('[SABTransport] OSC OUT LOG error:', data.error);
                if (this.#onErrorCallback) {
                    this.#onErrorCallback(data.error, 'oscOutLog');
                }
            }
        };
    }

    /**
     * Get cached prescheduler metrics
     * @returns {Uint32Array|null}
     */
    getPreschedulerMetrics() {
        return this.#cachedPreschedulerMetrics;
    }
}

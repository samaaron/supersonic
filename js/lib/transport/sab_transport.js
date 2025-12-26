/*
    SuperSonic - SAB Transport

    SharedArrayBuffer-based transport for OSC messages.
    Uses ring buffers with atomics for lock-free communication.
    Requires crossOriginIsolated context.
*/

import { Transport } from './transport.js';
import { writeToRingBuffer } from '../ring_buffer_writer.js';
import { DirectWriter } from '../direct_writer.js';

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
    #directWriter;

    // Cached views for ring buffer access
    #atomicView;
    #dataView;
    #uint8View;
    #controlIndices;

    // Workers
    #oscOutWorker;
    #oscInWorker;
    #debugWorker;
    #workerBaseURL;

    // Callbacks
    #onReplyCallback;
    #onDebugCallback;

    // State
    #initialized = false;
    #preschedulerCapacity;

    // Metrics
    #messagesSent = 0;
    #messagesDropped = 0;
    #bytesSent = 0;

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

        // Initialize direct writer for low-latency path
        this.#directWriter = new DirectWriter({
            sharedBuffer: this.#sharedBuffer,
            ringBufferBase: this.#ringBufferBase,
            bufferConstants: this.#bufferConstants,
            getAudioContextTime: config.getAudioContextTime,
            getNTPStartTime: config.getNTPStartTime,
        });
    }

    /**
     * Initialize the transport - spawns workers
     */
    async initialize() {
        if (this.#initialized) {
            console.warn('[SABTransport] Already initialized');
            return;
        }

        // Spawn workers
        this.#oscOutWorker = new Worker(
            this.#workerBaseURL + 'osc_out_prescheduler_worker.js',
            { type: 'module' }
        );
        this.#oscInWorker = new Worker(
            this.#workerBaseURL + 'osc_in_worker.js',
            { type: 'module' }
        );
        this.#debugWorker = new Worker(
            this.#workerBaseURL + 'debug_worker.js',
            { type: 'module' }
        );

        // Set up message handlers
        this.#setupWorkerHandlers();

        // Initialize workers with shared buffer
        await Promise.all([
            this.#initWorker(this.#oscOutWorker, 'OSC OUT', {
                maxPendingMessages: this.#preschedulerCapacity
            }),
            this.#initWorker(this.#oscInWorker, 'OSC IN'),
            this.#initWorker(this.#debugWorker, 'DEBUG'),
        ]);

        // Start polling workers
        this.#oscInWorker.postMessage({ type: 'start' });
        this.#debugWorker.postMessage({ type: 'start' });

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

        this.#messagesSent++;
        this.#bytesSent += message.length;
        return true;
    }

    /**
     * Try direct write for low-latency messages
     */
    trySendDirect(message) {
        if (!this.#initialized || this._disposed) {
            return false;
        }

        const written = this.#directWriter.tryWrite(message);
        if (written) {
            this.#messagesSent++;
            this.#bytesSent += message.length;
        }
        return written;
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

        this.#messagesSent++;
        this.#bytesSent += message.length;
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

        this.#messagesSent++;
        this.#bytesSent += message.length;
        return true;
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

    getMetrics() {
        return {
            messagesSent: this.#messagesSent,
            messagesDropped: this.#messagesDropped,
            bytesSent: this.#bytesSent,
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

        const CONTROL_START = this.#bufferConstants.CONTROL_START;
        this.#controlIndices = {
            IN_HEAD: (this.#ringBufferBase + CONTROL_START + 0) / 4,
            IN_TAIL: (this.#ringBufferBase + CONTROL_START + 4) / 4,
            IN_SEQUENCE: (this.#ringBufferBase + CONTROL_START + 24) / 4,
            IN_WRITE_LOCK: (this.#ringBufferBase + CONTROL_START + 40) / 4,
        };
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
                        this.#onReplyCallback(msg.oscData);
                    }
                });
            } else if (data.type === 'error') {
                console.error('[SABTransport] OSC IN error:', data.error);
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
            }
        };

        // OSC OUT worker - mainly for errors
        this.#oscOutWorker.onmessage = (event) => {
            const data = event.data;
            if (data.type === 'error') {
                console.error('[SABTransport] OSC OUT error:', data.error);
                this.#messagesDropped++;
            }
        };
    }
}

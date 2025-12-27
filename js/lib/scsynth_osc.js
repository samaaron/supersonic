/*
    SuperSonic - SuperCollider AudioWorklet WebAssembly port
    Copyright (c) 2025 Sam Aaron

    Based on SuperCollider by James McCartney and community
    GPL v3 or later
*/

import osc from '../vendor/osc.js/osc.js';
import { createWorker } from './worker_loader.js';

/**
 * ScsynthOSC - OSC communication layer for scsynth
 *
 * Supports two modes:
 * - 'sab': Uses SharedArrayBuffer ring buffers with polling workers
 * - 'postMessage': Uses MessagePort for communication (no SAB required)
 *
 * Provides clean API for sending/receiving OSC and debug messages
 */

export default class ScsynthOSC {
    constructor(workerBaseURL = null) {
        this.workerBaseURL = workerBaseURL;

        // Transport mode: 'sab' or 'postMessage'
        this.mode = 'sab';

        // Worklet port for postMessage mode
        this.workletPort = null;

        this.workers = {
            oscOut: null,      // Scheduler worker (both modes)
            oscIn: null,       // SAB mode only: polls OUT ring buffer
            debug: null        // Both modes: SAB polls ring buffer, postMessage decodes raw bytes
        };

        this.callbacks = {
            onRawOSC: null,
            onParsedOSC: null,
            onDebugMessage: null,
            onError: null,
            onInitialized: null
        };

        this.initialized = false;
        this.sharedBuffer = null;
        this.ringBufferBase = null;
        this.bufferConstants = null;

        // Cached prescheduler metrics for postMessage mode
        // (prescheduler worker can't write to worklet's WASM memory, so it sends metrics via postMessage)
        this.cachedPreschedulerMetrics = null;
    }

    /**
     * Get cached prescheduler metrics (postMessage mode only)
     * @returns {Uint32Array|null} Raw metrics array, or null if not in postMessage mode
     */
    getPreschedulerMetrics() {
        return this.cachedPreschedulerMetrics;
    }

    /**
     * Initialize OSC communication
     *
     * @param {Object} config - Configuration object
     * @param {string} [config.mode='sab'] - Transport mode: 'sab' or 'postMessage'
     * @param {SharedArrayBuffer} [config.sharedBuffer] - Required for SAB mode
     * @param {number} [config.ringBufferBase] - Required for SAB mode
     * @param {Object} [config.bufferConstants] - Required for SAB mode
     * @param {MessagePort} [config.workletPort] - Required for postMessage mode
     * @param {number} [config.preschedulerCapacity=65536] - Max pending messages
     */
    async init(config = {}) {
        if (this.initialized) {
            console.warn('[ScsynthOSC] Already initialized');
            return;
        }

        this.mode = config.mode || 'sab';
        this.preschedulerCapacity = config.preschedulerCapacity || 65536;

        if (this.mode === 'sab') {
            await this.#initSABMode(config);
        } else if (this.mode === 'postMessage') {
            await this.#initPostMessageMode(config);
        } else {
            throw new Error(`Unknown mode: ${this.mode}`);
        }

        this.initialized = true;

        if (this.callbacks.onInitialized) {
            this.callbacks.onInitialized();
        }
    }

    /**
     * Initialize SAB mode - uses ring buffers and polling workers
     */
    async #initSABMode(config) {
        this.sharedBuffer = config.sharedBuffer;
        this.ringBufferBase = config.ringBufferBase;
        this.bufferConstants = config.bufferConstants;

        if (!this.sharedBuffer || !this.bufferConstants) {
            throw new Error('SAB mode requires sharedBuffer and bufferConstants');
        }

        try {
            // Create all workers (uses Blob URL if cross-origin)
            const [oscOutWorker, oscInWorker, debugWorker] = await Promise.all([
                createWorker(this.workerBaseURL + 'osc_out_prescheduler_worker.js', {type: 'module'}),
                createWorker(this.workerBaseURL + 'osc_in_worker.js', {type: 'module'}),
                createWorker(this.workerBaseURL + 'debug_worker.js', {type: 'module'})
            ]);
            this.workers.oscOut = oscOutWorker;
            this.workers.oscIn = oscInWorker;
            this.workers.debug = debugWorker;

            // Set up worker message handlers
            this.#setupSABWorkerHandlers();

            // Initialize all workers with SharedArrayBuffer
            const initPromises = [
                this.#initWorker(this.workers.oscOut, 'OSC SCHEDULER+WRITER', {
                    mode: 'sab',
                    maxPendingMessages: this.preschedulerCapacity
                }),
                this.#initWorker(this.workers.oscIn, 'OSC IN'),
                this.#initWorker(this.workers.debug, 'DEBUG')
            ];

            await Promise.all(initPromises);

            // Start polling workers
            this.workers.oscIn.postMessage({ type: 'start' });
            this.workers.debug.postMessage({ type: 'start' });

        } catch (error) {
            console.error('[ScsynthOSC] SAB mode initialization failed:', error);
            if (this.callbacks.onError) {
                this.callbacks.onError(error);
            }
            throw error;
        }
    }

    /**
     * Initialize postMessage mode - uses MessagePort for communication
     */
    async #initPostMessageMode(config) {
        this.workletPort = config.workletPort;

        if (!this.workletPort) {
            throw new Error('postMessage mode requires workletPort');
        }

        try {
            // Create prescheduler and debug workers (uses Blob URL if cross-origin)
            // Debug worker handles text decoding (TextDecoder not available in AudioWorklet)
            const [oscOutWorker, debugWorker] = await Promise.all([
                createWorker(this.workerBaseURL + 'osc_out_prescheduler_worker.js', {type: 'module'}),
                createWorker(this.workerBaseURL + 'debug_worker.js', {type: 'module'})
            ]);
            this.workers.oscOut = oscOutWorker;
            this.workers.debug = debugWorker;

            // Set up handlers
            this.#setupPostMessageHandlers();

            // Initialize workers
            const initPromises = [
                this.#initWorker(this.workers.oscOut, 'OSC SCHEDULER', {
                    mode: 'postMessage',
                    maxPendingMessages: this.preschedulerCapacity
                }),
                this.#initWorker(this.workers.debug, 'DEBUG', {
                    mode: 'postMessage'
                })
            ];

            await Promise.all(initPromises);

        } catch (error) {
            console.error('[ScsynthOSC] postMessage mode initialization failed:', error);
            if (this.callbacks.onError) {
                this.callbacks.onError(error);
            }
            throw error;
        }
    }

    /**
     * Initialize a single worker
     */
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

            // Build init message based on mode
            const initMsg = {
                type: 'init',
                ...extraConfig
            };

            // SAB mode needs buffer info
            if (this.mode === 'sab' && this.sharedBuffer) {
                initMsg.sharedBuffer = this.sharedBuffer;
                initMsg.ringBufferBase = this.ringBufferBase;
                initMsg.bufferConstants = this.bufferConstants;
            }

            worker.postMessage(initMsg);
        });
    }

    /**
     * Set up handlers for SAB mode workers
     */
    #setupSABWorkerHandlers() {
        // OSC IN worker handler - receives OSC replies from scsynth
        this.workers.oscIn.onmessage = (event) => {
            const data = event.data;
            switch (data.type) {
                case 'messages':
                    this.#handleOSCMessages(data.messages);
                    break;
                case 'error':
                    console.error('[ScsynthOSC] OSC IN error:', data.error);
                    if (this.callbacks.onError) {
                        this.callbacks.onError(data.error, 'oscIn');
                    }
                    break;
            }
        };

        // DEBUG worker handler
        this.workers.debug.onmessage = (event) => {
            const data = event.data;
            switch (data.type) {
                case 'debug':
                    if (this.callbacks.onDebugMessage) {
                        data.messages.forEach(msg => {
                            this.callbacks.onDebugMessage(msg);
                        });
                    }
                    break;
                case 'error':
                    console.error('[ScsynthOSC] DEBUG error:', data.error);
                    if (this.callbacks.onError) {
                        this.callbacks.onError(data.error, 'debug');
                    }
                    break;
            }
        };

        // OSC OUT worker handler (errors only in SAB mode)
        this.workers.oscOut.onmessage = (event) => {
            const data = event.data;
            if (data.type === 'error') {
                console.error('[ScsynthOSC] OSC OUT error:', data.error);
                if (this.callbacks.onError) {
                    this.callbacks.onError(data.error, 'oscOut');
                }
            }
        };
    }

    /**
     * Set up handlers for postMessage mode
     */
    #setupPostMessageHandlers() {
        // Prescheduler dispatches messages to us, we forward to worklet
        this.workers.oscOut.onmessage = (event) => {
            const data = event.data;
            switch (data.type) {
                case 'dispatch':
                    // Forward OSC message to worklet
                    if (this.workletPort) {
                        this.workletPort.postMessage({
                            type: 'osc',
                            oscData: data.oscData,
                            timestamp: data.timestamp
                        });
                    }
                    break;
                case 'preschedulerMetrics':
                    // Cache prescheduler metrics (worklet can't access prescheduler's local buffer)
                    this.cachedPreschedulerMetrics = data.metrics;
                    break;
                case 'error':
                    console.error('[ScsynthOSC] OSC OUT error:', data.error);
                    if (this.callbacks.onError) {
                        this.callbacks.onError(data.error, 'oscOut');
                    }
                    break;
            }
        };

        // Debug worker handler - receives decoded messages
        this.workers.debug.onmessage = (event) => {
            const data = event.data;
            switch (data.type) {
                case 'debug':
                    if (this.callbacks.onDebugMessage) {
                        data.messages.forEach(msg => {
                            this.callbacks.onDebugMessage(msg);
                        });
                    }
                    break;
                case 'error':
                    console.error('[ScsynthOSC] DEBUG error:', data.error);
                    if (this.callbacks.onError) {
                        this.callbacks.onError(data.error, 'debug');
                    }
                    break;
            }
        };

        // Worklet sends OSC replies and debug messages to us
        // Use addEventListener to avoid conflicts with other port listeners
        this.workletPort.addEventListener('message', (event) => {
            const data = event.data;
            switch (data.type) {
                case 'oscReply':
                    // OSC reply from scsynth
                    if (data.oscData) {
                        this.#handleOSCMessages([{ oscData: data.oscData }]);
                    }
                    break;
                case 'oscReplies':
                    // Batch of OSC replies
                    if (data.messages) {
                        this.#handleOSCMessages(data.messages);
                    }
                    break;
                case 'debugRawBatch':
                    // Raw debug bytes from worklet - forward to debug_worker for decoding
                    if (this.workers.debug && data.messages) {
                        this.workers.debug.postMessage({
                            type: 'debugRaw',
                            messages: data.messages
                        });
                    }
                    break;
                // Other worklet messages (bufferLoaded, etc.) handled elsewhere
            }
        });
    }

    /**
     * Handle incoming OSC messages (shared by both modes)
     */
    #handleOSCMessages(messages) {
        messages.forEach(msg => {
            if (!msg.oscData) return;

            // Fire raw OSC callback
            if (this.callbacks.onRawOSC) {
                this.callbacks.onRawOSC({
                    oscData: msg.oscData,
                    sequence: msg.sequence
                });
            }

            // Parse and fire parsed OSC callback
            if (this.callbacks.onParsedOSC) {
                try {
                    const options = { metadata: false, unpackSingleArgs: false };
                    const decoded = osc.readPacket(msg.oscData, options);
                    this.callbacks.onParsedOSC(decoded);
                } catch (e) {
                    console.error('[ScsynthOSC] Failed to decode OSC message:', e, msg);
                }
            }
        });
    }

    /**
     * Send OSC data (message or bundle)
     */
    send(oscData, options = {}) {
        if (!this.initialized) {
            console.error('[ScsynthOSC] Not initialized');
            return;
        }

        const { sessionId = 0, runTag = '', audioTimeS = null, currentTimeS = null } = options;

        this.workers.oscOut.postMessage({
            type: 'send',
            oscData: oscData,
            sessionId: sessionId,
            runTag: runTag,
            audioTimeS: audioTimeS,
            currentTimeS: currentTimeS
        });
    }

    /**
     * Send OSC data immediately, ignoring any bundle timestamps
     */
    sendImmediate(oscData) {
        if (!this.initialized) {
            console.error('[ScsynthOSC] Not initialized');
            return;
        }

        this.workers.oscOut.postMessage({
            type: 'sendImmediate',
            oscData: oscData
        });
    }

    /**
     * Cancel scheduled OSC bundles by session and tag
     */
    cancelSessionTag(sessionId, runTag) {
        if (!this.initialized) return;

        this.workers.oscOut.postMessage({
            type: 'cancelSessionTag',
            sessionId,
            runTag
        });
    }

    /**
     * Cancel all scheduled OSC bundles from a session
     */
    cancelSession(sessionId) {
        if (!this.initialized) return;

        this.workers.oscOut.postMessage({
            type: 'cancelSession',
            sessionId
        });
    }

    /**
     * Cancel all scheduled OSC bundles with a specific tag (any session)
     */
    cancelTag(runTag) {
        if (!this.initialized) return;

        this.workers.oscOut.postMessage({
            type: 'cancelTag',
            runTag
        });
    }

    /**
     * Cancel all scheduled OSC bundles
     */
    cancelAll() {
        if (!this.initialized) return;

        this.workers.oscOut.postMessage({
            type: 'cancelAll'
        });
    }

    /**
     * Clear debug buffer (SAB mode only)
     */
    clearDebug() {
        if (!this.initialized) return;

        if (this.mode === 'sab' && this.workers.debug) {
            this.workers.debug.postMessage({
                type: 'clear'
            });
        }
        // In postMessage mode, debug buffer is managed by worklet
    }

    /**
     * Set callback for raw binary OSC messages received from scsynth
     */
    onRawOSC(callback) {
        this.callbacks.onRawOSC = callback;
    }

    /**
     * Set callback for parsed OSC messages received from scsynth
     */
    onParsedOSC(callback) {
        this.callbacks.onParsedOSC = callback;
    }

    /**
     * Set callback for debug messages
     */
    onDebugMessage(callback) {
        this.callbacks.onDebugMessage = callback;
    }

    /**
     * Set callback for errors
     */
    onError(callback) {
        this.callbacks.onError = callback;
    }

    /**
     * Set callback for initialization complete
     */
    onInitialized(callback) {
        this.callbacks.onInitialized = callback;
    }

    /**
     * Terminate all workers and cleanup
     */
    terminate() {
        if (this.workers.oscOut) {
            this.workers.oscOut.postMessage({ type: 'stop' });
            this.workers.oscOut.terminate();
        }

        if (this.workers.oscIn) {
            this.workers.oscIn.postMessage({ type: 'stop' });
            this.workers.oscIn.terminate();
        }

        if (this.workers.debug) {
            this.workers.debug.postMessage({ type: 'stop' });
            this.workers.debug.terminate();
        }

        this.workers = {
            oscOut: null,
            oscIn: null,
            debug: null
        };

        this.workletPort = null;
        this.initialized = false;
        if (__DEV__) console.log('[Dbg-ScsynthOSC] All workers terminated');
    }
}

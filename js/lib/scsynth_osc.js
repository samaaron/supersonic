/*
    SuperSonic - SuperCollider AudioWorklet WebAssembly port
    Copyright (c) 2025 Sam Aaron

    Based on SuperCollider by James McCartney and community
    GPL v3 or later
*/

import osc from '../vendor/osc.js/osc.js';

/**
 * ScsynthOSC - OSC communication layer for scsynth
 * Manages OSC IN, OSC OUT, and DEBUG workers
 * Provides clean API for sending/receiving OSC and debug messages
 */

export default class ScsynthOSC {
    constructor() {
        this.workers = {
            oscOut: null,      // Scheduler worker (now also writes directly to ring buffer)
            oscIn: null,
            debug: null
        };

        this.callbacks = {
            onRawOSC: null,         // Raw binary OSC callback
            onParsedOSC: null,      // Parsed OSC callback
            onDebugMessage: null,
            onError: null,
            onInitialized: null
        };

        this.initialized = false;
        this.sharedBuffer = null;
        this.ringBufferBase = null;
        this.bufferConstants = null;
    }

    /**
     * Initialize all workers with SharedArrayBuffer
     */
    async init(sharedBuffer, ringBufferBase, bufferConstants) {
        if (this.initialized) {
            console.warn('[ScsynthOSC] Already initialized');
            return;
        }

        this.sharedBuffer = sharedBuffer;
        this.ringBufferBase = ringBufferBase;
        this.bufferConstants = bufferConstants;

        try {
            // Create all workers
            // osc_out_prescheduler_worker.js handles scheduling/tag cancellation AND writes directly to ring buffer
            // osc_in_worker.js handles receiving OSC messages from scsynth
            // debug_worker.js handles receiving debug messages from scsynth
            // Use import.meta.url to resolve worker paths relative to the module location
            // Module workers (type: 'module') support cross-origin loading for CDN deployment
            this.workers.oscOut = new Worker(new URL('./workers/osc_out_prescheduler_worker.js', import.meta.url), {type: 'module'});
            this.workers.oscIn = new Worker(new URL('./workers/osc_in_worker.js', import.meta.url), {type: 'module'});
            this.workers.debug = new Worker(new URL('./workers/debug_worker.js', import.meta.url), {type: 'module'});

            // Set up worker message handlers
            this.setupWorkerHandlers();

            // Initialize all workers with SharedArrayBuffer
            const initPromises = [
                this.initWorker(this.workers.oscOut, 'OSC SCHEDULER+WRITER'),
                this.initWorker(this.workers.oscIn, 'OSC IN'),
                this.initWorker(this.workers.debug, 'DEBUG')
            ];

            await Promise.all(initPromises);

            // Start the workers
            this.workers.oscIn.postMessage({ type: 'start' });
            this.workers.debug.postMessage({ type: 'start' });

            this.initialized = true;

            if (this.callbacks.onInitialized) {
                this.callbacks.onInitialized();
            }

        } catch (error) {
            console.error('[ScsynthOSC] Initialization failed:', error);
            if (this.callbacks.onError) {
                this.callbacks.onError(error);
            }
            throw error;
        }
    }

    /**
     * Initialize a single worker
     */
    initWorker(worker, name) {
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
                sharedBuffer: this.sharedBuffer,
                ringBufferBase: this.ringBufferBase,
                bufferConstants: this.bufferConstants
            });
        });
    }

    /**
     * Set up message handlers for all workers
     */
    setupWorkerHandlers() {
        // OSC IN worker handler
        this.workers.oscIn.onmessage = (event) => {
            const data = event.data;
            switch (data.type) {
                case 'messages':
                    data.messages.forEach(msg => {
                        if (!msg.oscData) return;

                        // First, fire raw OSC callback if registered
                        if (this.callbacks.onRawOSC) {
                            this.callbacks.onRawOSC({
                                oscData: msg.oscData,
                                sequence: msg.sequence
                            });
                        }

                        // Then, parse and fire parsed OSC callback if registered
                        if (this.callbacks.onParsedOSC) {
                            try {
                                // Use custom options to ensure args is always an array
                                const options = { metadata: false, unpackSingleArgs: false };
                                const decoded = osc.readPacket(msg.oscData, options);
                                // Pass the decoded message with address and args
                                this.callbacks.onParsedOSC(decoded);
                            } catch (e) {
                                console.error('[ScsynthOSC] Failed to decode OSC message:', e, msg);
                            }
                        }
                    });
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

        // OSC OUT worker handler (mainly for errors since it writes directly)
        this.workers.oscOut.onmessage = (event) => {
            const data = event.data;
            switch (data.type) {
                case 'error':
                    console.error('[ScsynthOSC] OSC OUT error:', data.error);
                    if (this.callbacks.onError) {
                        this.callbacks.onError(data.error, 'oscOut');
                    }
                    break;
            }
        };
    }

    /**
     * Send OSC data (message or bundle)
     * - OSC messages are sent immediately
     * - OSC bundles are scheduled based on audioTimeS (target audio time)
     *
     * @param {Uint8Array} oscData - Binary OSC data (message or bundle)
     * @param {Object} options - Optional metadata (editorId, runTag, audioTimeS, currentTimeS)
     */
    send(oscData, options = {}) {
        if (!this.initialized) {
            console.error('[ScsynthOSC] Not initialized');
            return;
        }

        const { editorId = 0, runTag = '', audioTimeS = null, currentTimeS = null } = options;

        this.workers.oscOut.postMessage({
            type: 'send',
            oscData: oscData,
            editorId: editorId,
            runTag: runTag,
            audioTimeS: audioTimeS,
            currentTimeS: currentTimeS
        });
    }

    /**
     * Send OSC data immediately, ignoring any bundle timestamps
     * - Extracts all messages from bundles
     * - Sends all messages immediately to scsynth
     * - For applications that don't expect server-side scheduling
     *
     * @param {Uint8Array} oscData - Binary OSC data (message or bundle)
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
     * Cancel scheduled OSC bundles by editor and tag
     */
    cancelEditorTag(editorId, runTag) {
        if (!this.initialized) return;

        this.workers.oscOut.postMessage({
            type: 'cancelEditorTag',
            editorId,
            runTag
        });
    }

    /**
     * Cancel all scheduled OSC bundles from an editor
     */
    cancelEditor(editorId) {
        if (!this.initialized) return;

        this.workers.oscOut.postMessage({
            type: 'cancelEditor',
            editorId
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
     * Clear debug buffer
     */
    clearDebug() {
        if (!this.initialized) return;

        this.workers.debug.postMessage({
            type: 'clear'
        });
    }

    /**
     * Get statistics from all workers
     */
    async getStats() {
        if (!this.initialized) {
            return null;
        }

        const statsPromises = [
            this.getWorkerStats(this.workers.oscOut, 'oscOut'),
            this.getWorkerStats(this.workers.oscIn, 'oscIn'),
            this.getWorkerStats(this.workers.debug, 'debug')
        ];

        const results = await Promise.all(statsPromises);

        return {
            oscOut: results[0],
            oscIn: results[1],
            debug: results[2]
        };
    }

    /**
     * Get stats from a single worker
     */
    getWorkerStats(worker, name) {
        return new Promise((resolve) => {
            const timeout = setTimeout(() => {
                resolve({ error: 'Timeout getting stats' });
            }, 1000);

            const handler = (event) => {
                if (event.data.type === 'stats') {
                    clearTimeout(timeout);
                    worker.removeEventListener('message', handler);
                    resolve(event.data.stats);
                }
            };

            worker.addEventListener('message', handler);
            worker.postMessage({ type: 'getStats' });
        });
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

        this.initialized = false;
        console.log('[ScsynthOSC] All workers terminated');
    }
}

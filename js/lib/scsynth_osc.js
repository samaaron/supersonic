/*
    SuperSonic - SuperCollider AudioWorklet WebAssembly port
    Copyright (c) 2025 Sam Aaron

    Based on SuperCollider by James McCartney and community
    GPL v3 or later
*/

/**
 * ScsynthOSC - OSC communication layer for scsynth
 * Manages OSC IN, OSC OUT, and DEBUG workers
 * Provides clean API for sending/receiving OSC and debug messages
 */

export default class ScsynthOSC {
    constructor() {
        this.workers = {
            oscOut: null,
            oscIn: null,
            debug: null
        };

        this.callbacks = {
            onOSCMessage: null,
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
            // Create all three workers
            // osc_out_worker.js handles sending OSC messages to scsynth
            // osc_in_worker.js handles receiving OSC messages from scsynth
            // debug_worker.js handles receiving debug messages from scsynth
            this.workers.oscOut = new Worker('./dist/workers/osc_out_worker.js');
            this.workers.oscIn = new Worker('./dist/workers/osc_in_worker.js');
            this.workers.debug = new Worker('./dist/workers/debug_worker.js');

            // Set up worker message handlers
            this.setupWorkerHandlers();

            // Initialize all workers with SharedArrayBuffer
            const initPromises = [
                this.initWorker(this.workers.oscOut, 'OSC OUT'),
                this.initWorker(this.workers.oscIn, 'OSC IN'),
                this.initWorker(this.workers.debug, 'DEBUG')
            ];

            await Promise.all(initPromises);

            // Start the receiving workers (they use Atomics.wait)
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
                    if (this.callbacks.onOSCMessage) {
                        data.messages.forEach(msg => {
                            this.callbacks.onOSCMessage(msg);
                        });
                    }
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
     * - OSC bundles are scheduled based on waitTimeMs (calculated by SuperSonic)
     *
     * @param {Uint8Array} oscData - Binary OSC data (message or bundle)
     * @param {Object} options - Optional metadata (editorId, runTag, waitTimeMs)
     */
    send(oscData, options = {}) {
        if (!this.initialized) {
            console.error('[ScsynthOSC] Not initialized');
            return;
        }

        const { editorId = 0, runTag = '', waitTimeMs = null } = options;

        this.workers.oscOut.postMessage({
            type: 'send',
            oscData: oscData,
            editorId: editorId,
            runTag: runTag,
            waitTimeMs: waitTimeMs
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
     * Set callback for OSC messages received from scsynth
     */
    onOSCMessage(callback) {
        this.callbacks.onOSCMessage = callback;
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
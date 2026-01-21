// SPDX-License-Identifier: MIT OR GPL-3.0-or-later
// Copyright (c) 2025 Sam Aaron

import { Transport } from './transport.js';
import { createWorker } from '../worker_loader.js';

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

    // Cached prescheduler metrics (prescheduler sends via postMessage)
    #cachedPreschedulerMetrics = null;

    // State
    #initialized = false;
    #preschedulerCapacity;
    #snapshotIntervalMs;

    // Metrics
    #messagesSent = 0;
    #messagesDropped = 0;
    #bytesSent = 0;
    #messagesReceived = 0;
    #bytesReceived = 0;
    #directSends = 0;
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
     * @param {number} [config.snapshotIntervalMs=50] - Interval for metrics/tree snapshots
     */
    constructor(config) {
        super({ ...config, mode: 'postMessage' });

        this.#workerBaseURL = config.workerBaseURL;
        this.#preschedulerCapacity = config.preschedulerCapacity || 65536;
        this.#snapshotIntervalMs = config.snapshotIntervalMs || 50;
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

        // Reuse the existing prescheduler worker with postMessage mode
        // It will dispatch messages to us instead of writing to ring buffer
        // Uses Blob URL if cross-origin (enables CDN-only deployment)
        this.#preschedulerWorker = await createWorker(
            this.#workerBaseURL + 'osc_out_prescheduler_worker.js',
            { type: 'module' }
        );

        this.#preschedulerWorker.onmessage = (event) => {
            this.#handlePreschedulerMessage(event.data);
        };

        // Initialize prescheduler worker in postMessage mode
        await this.#initPreschedulerWorker();

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

        this.#messagesSent++;
        this.#bytesSent += message.length;
        return true;
    }

    /**
     * Try direct send - in postMessage mode, always returns false
     * (no direct write possible without SAB)
     */
    trySendDirect(message) {
        // PostMessage mode doesn't have a direct write path
        // All messages go through the prescheduler
        return false;
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

        this.#messagesSent++;
        this.#bytesSent += message.length;
        return true;
    }

    /**
     * Send immediately, bypassing prescheduler
     */
    sendImmediate(message) {
        if (!this.#initialized || this._disposed) {
            return false;
        }

        // Send directly to worklet, bypassing prescheduler
        this.#workletPort.postMessage({
            type: 'osc',
            oscData: message,
        });

        this.#messagesSent++;
        this.#bytesSent += message.length;
        this.#directSends++;
        return true;
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

    /**
     * Handle raw debug bytes from worklet (postMessage mode)
     * Forwards to debug worker for text decoding
     * @param {Object} data - { messages: Array<{bytes, sequence}> }
     */
    handleDebugRaw(data) {
        if (this.#preschedulerWorker && data.messages) {
            // Debug worker is not used in postMessage mode for raw bytes
            // We need to create a debug worker or handle this differently
            // For now, decode here using TextDecoder
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
        return {
            messagesSent: this.#messagesSent,
            messagesDropped: this.#messagesDropped,
            bytesSent: this.#bytesSent,
            messagesReceived: this.#messagesReceived,
            bytesReceived: this.#bytesReceived,
            directSends: this.#directSends,
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

    #initPreschedulerWorker() {
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
            this.#preschedulerWorker.postMessage({
                type: 'init',
                mode: 'postMessage',  // Use postMessage dispatch mode
                maxPendingMessages: this.#preschedulerCapacity,
                snapshotIntervalMs: this.#snapshotIntervalMs,
                // No sharedBuffer, ringBufferBase, or bufferConstants needed
            });
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
                                        this.#messagesDropped += dropped;
                                    }
                                }
                            }
                            if (msg.sequence !== undefined) {
                                this.#lastSequenceReceived = msg.sequence;
                            }

                            this.#messagesReceived++;
                            this.#bytesReceived += msg.oscData.byteLength || msg.oscData.length || 0;
                            if (this.#onReplyCallback) {
                                this.#onReplyCallback(msg.oscData, msg.sequence);
                            }
                        }
                    }
                }
                break;

            case 'debug':
                // Debug message from scsynth
                if (this.#onDebugCallback && data.message) {
                    this.#onDebugCallback(data.message);
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

            case 'error':
                console.error('[PostMessageTransport] Worklet error:', data.error);
                this.#messagesDropped++;
                if (this.#onErrorCallback) {
                    this.#onErrorCallback(data.error, 'worklet');
                }
                break;
        }
    }

    #handlePreschedulerMessage(data) {
        switch (data.type) {
            case 'dispatch':
                // Prescheduler says it's time to send this message
                if (this.#workletPort && data.oscData) {
                    this.#workletPort.postMessage({
                        type: 'osc',
                        oscData: data.oscData,
                        timestamp: data.timestamp,
                    });
                }
                break;

            case 'preschedulerMetrics':
                // Cache prescheduler metrics for retrieval
                this.#cachedPreschedulerMetrics = data.metrics;
                break;

            case 'error':
                console.error('[PostMessageTransport] Prescheduler error:', data.error);
                this.#messagesDropped++;
                if (this.#onErrorCallback) {
                    this.#onErrorCallback(data.error, 'oscOut');
                }
                break;
        }
    }
}

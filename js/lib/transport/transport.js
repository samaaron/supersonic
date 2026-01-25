// SPDX-License-Identifier: MIT OR GPL-3.0-or-later
// Copyright (c) 2025 Sam Aaron

/**
 * @typedef {Object} TransportConfig
 * @property {string} mode - 'sab' or 'postMessage'
 * @property {MessagePort} [workletPort] - Port for worklet communication
 * @property {SharedArrayBuffer} [sharedBuffer] - Shared memory (SAB mode only)
 * @property {WebAssembly.Memory} [wasmMemory] - WASM memory (SAB mode only)
 * @property {number} [ringBufferBase] - Ring buffer base offset (SAB mode only)
 * @property {Object} [bufferConstants] - Buffer layout constants (SAB mode only)
 * @property {Function} [getAudioContextTime] - Returns AudioContext.currentTime
 * @property {Function} [getNTPStartTime] - Returns NTP start time
 */

/**
 * @typedef {Object} TransportMetrics
 * @property {number} messagesSent - Total messages sent
 * @property {number} messagesDropped - Messages that couldn't be sent
 * @property {number} bytesSent - Total bytes sent
 */

/**
 * Abstract transport interface for OSC message delivery
 */
export class Transport {
    /**
     * @param {TransportConfig} config
     */
    constructor(config) {
        if (new.target === Transport) {
            throw new Error('Transport is abstract - use SABTransport or PostMessageTransport');
        }
        this._config = config;
        this._disposed = false;
    }

    /**
     * Get the transport mode
     * @returns {'sab' | 'postMessage'}
     */
    get mode() {
        return this._config.mode;
    }

    /**
     * Send an OSC message to the audio engine
     * @param {Uint8Array} message - OSC message bytes
     * @param {number} [timestamp] - NTP timestamp for scheduling (optional)
     * @returns {boolean} True if sent successfully
     */
    send(message, timestamp) {
        throw new Error('Abstract method - implement in subclass');
    }

    /**
     * Register callback for OSC replies from engine
     * @param {function(Uint8Array): void} callback
     */
    onReply(callback) {
        throw new Error('Abstract method - implement in subclass');
    }

    /**
     * Register callback for debug messages from engine
     * @param {function(string): void} callback
     */
    onDebug(callback) {
        throw new Error('Abstract method - implement in subclass');
    }

    /**
     * Register callback for transport errors
     * @param {function(string, string): void} callback - Receives (error, workerName)
     */
    onError(callback) {
        throw new Error('Abstract method - implement in subclass');
    }

    /**
     * Get current transport metrics
     * @returns {TransportMetrics}
     */
    getMetrics() {
        throw new Error('Abstract method - implement in subclass');
    }

    /**
     * Initialize the transport (called after worklet is ready)
     * @returns {Promise<void>}
     */
    async initialize() {
        throw new Error('Abstract method - implement in subclass');
    }

    /**
     * Create an OscChannel for direct worker-to-worklet communication
     *
     * Returns an OscChannel that can be transferred to a Web Worker,
     * allowing that worker to send OSC messages directly to the AudioWorklet.
     *
     * @returns {OscChannel}
     */
    createOscChannel() {
        throw new Error('Abstract method - implement in subclass');
    }

    /**
     * Clean up resources
     */
    dispose() {
        this._disposed = true;
    }

    /**
     * Check if transport is ready to send
     * @returns {boolean}
     */
    get ready() {
        throw new Error('Abstract method - implement in subclass');
    }
}

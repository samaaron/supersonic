// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2025 Sam Aaron

/**
 * DEBUG Worker - Receives debug messages from AudioWorklet
 * Uses Atomics.wait() for instant wake when debug logs arrive
 * Reads from DEBUG ring buffer and forwards to main thread
 */

import * as MetricsOffsets from '../lib/metrics_offsets.js';
import { readMessagesFromBuffer } from '../lib/ring_buffer_core.js';

// Transport mode: 'sab' or 'postMessage'
let mode = 'sab';

// Ring buffer configuration (SAB mode only)
let sharedBuffer = null;
let ringBufferBase = null;
let atomicView = null;
let dataView = null;
let uint8View = null;

// Ring buffer layout constants (loaded from WASM at initialization)
let bufferConstants = null;

// Control indices (calculated after init)
let CONTROL_INDICES = {};

// Metrics view (for writing stats to SAB)
let metricsView = null;

// Worker state
let running = false;

// Reusable TextDecoder for debug message parsing
const textDecoder = new TextDecoder('utf-8');

const debugWorkerLog = (...args) => {
    if (__DEV__) {
        console.log(...args);
    }
};

/**
 * Initialize ring buffer access
 */
const initRingBuffer = (buffer, base, constants) => {
    sharedBuffer = buffer;
    ringBufferBase = base;
    bufferConstants = constants;
    atomicView = new Int32Array(sharedBuffer);
    dataView = new DataView(sharedBuffer);
    uint8View = new Uint8Array(sharedBuffer);

    // Calculate control indices using constants from WASM
    CONTROL_INDICES = {
        DEBUG_HEAD: (ringBufferBase + bufferConstants.CONTROL_START + 16) / 4,
        DEBUG_TAIL: (ringBufferBase + bufferConstants.CONTROL_START + 20) / 4
    };

    // Initialize metrics view
    const metricsBase = ringBufferBase + bufferConstants.METRICS_START;
    metricsView = new Uint32Array(sharedBuffer, metricsBase, bufferConstants.METRICS_SIZE / 4);
};

/**
 * Read debug messages from buffer
 * Uses shared ring_buffer_core for read logic
 */
const readDebugMessages = () => {
    const head = Atomics.load(atomicView, CONTROL_INDICES.DEBUG_HEAD);
    const tail = Atomics.load(atomicView, CONTROL_INDICES.DEBUG_TAIL);

    if (head === tail) {
        return null; // No messages
    }

    const messages = [];

    const { newTail, messagesRead } = readMessagesFromBuffer({
        uint8View,
        dataView,
        bufferStart: ringBufferBase + bufferConstants.DEBUG_BUFFER_START,
        bufferSize: bufferConstants.DEBUG_BUFFER_SIZE,
        head,
        tail,
        messageMagic: bufferConstants.MESSAGE_MAGIC,
        paddingMagic: bufferConstants.PADDING_MAGIC,
        headerSize: bufferConstants.MESSAGE_HEADER_SIZE,
        maxMessages: 1000,
        onMessage: (payload, sequence, length) => {
            // Convert bytes to string using TextDecoder for proper UTF-8 handling
            let messageText = textDecoder.decode(payload);

            // Remove trailing newline if present
            if (messageText.endsWith('\n')) {
                messageText = messageText.slice(0, -1);
            }

            messages.push({
                text: messageText,
                timestamp: performance.now(),
                sequence
            });

            // Update metrics
            if (metricsView) {
                Atomics.add(metricsView, MetricsOffsets.DEBUG_MESSAGES_RECEIVED, 1);
                Atomics.add(metricsView, MetricsOffsets.DEBUG_BYTES_RECEIVED, payload.length);
            }
        },
        onCorruption: (position) => {
            console.error('[DebugWorker] Corrupted message at position', position);
        }
    });

    // Update tail pointer (consume messages)
    if (messagesRead > 0) {
        Atomics.store(atomicView, CONTROL_INDICES.DEBUG_TAIL, newTail);
    }

    return messages.length > 0 ? messages : null;
};

/**
 * Main wait loop using Atomics.wait for instant wake
 */
const waitLoop = () => {
    while (running) {
        try {
            // Get current DEBUG_HEAD value
            const currentHead = Atomics.load(atomicView, CONTROL_INDICES.DEBUG_HEAD);
            const currentTail = Atomics.load(atomicView, CONTROL_INDICES.DEBUG_TAIL);

            // If buffer is empty, wait for AudioWorklet to notify us
            if (currentHead === currentTail) {
                // Wait for up to 100ms (allows checking stop signal)
                const result = Atomics.wait(atomicView, CONTROL_INDICES.DEBUG_HEAD, currentHead, 100);

                if (result === 'ok' || result === 'not-equal') {
                    // We were notified or value changed!
                } else if (result === 'timed-out') {
                    continue; // Check running flag
                }
            }

            // Read all available debug messages
            const messages = readDebugMessages();

            if (messages && messages.length > 0) {
                // Send to main thread
                self.postMessage({
                    type: 'debug',
                    messages
                });
            }

        } catch (error) {
            console.error('[DebugWorker] Error in wait loop:', error);
            self.postMessage({
                type: 'error',
                error: error.message
            });

            // Brief pause on error before retrying (use existing atomicView)
            // Wait on a value that won't change for 10ms as a simple delay
            Atomics.wait(atomicView, 0, atomicView[0], 10);
        }
    }
};

/**
 * Start the wait loop
 */
const start = () => {
    if (!sharedBuffer) {
        console.error('[DebugWorker] Cannot start - not initialized');
        return;
    }

    if (running) {
        console.warn('[DebugWorker] Already running');
        return;
    }

    running = true;
    waitLoop();
};

/**
 * Stop the wait loop
 */
const stop = () => {
    running = false;
};

/**
 * Clear debug buffer
 */
const clear = () => {
    if (!sharedBuffer) return;

    // Reset head and tail to 0
    Atomics.store(atomicView, CONTROL_INDICES.DEBUG_HEAD, 0);
    Atomics.store(atomicView, CONTROL_INDICES.DEBUG_TAIL, 0);
};

/**
 * Decode raw bytes from postMessage mode
 * Called when AudioWorklet sends debugRawBatch messages
 */
const decodeRawMessages = (rawMessages) => {
    const messages = [];

    for (const raw of rawMessages) {
        try {
            const bytes = new Uint8Array(raw.bytes);
            let text = textDecoder.decode(bytes);

            // Remove trailing newline if present
            if (text.endsWith('\n')) {
                text = text.slice(0, -1);
            }

            messages.push({
                text: text,
                timestamp: performance.now(),
                sequence: raw.sequence
            });
        } catch (err) {
            console.error('[DebugWorker] Failed to decode message:', err);
        }
    }

    if (messages.length > 0) {
        self.postMessage({
            type: 'debug',
            messages
        });
    }
};

/**
 * Handle messages from main thread
 */
self.addEventListener('message', (event) => {
    const { data } = event;

    try {
        switch (data.type) {
            case 'init':
                mode = data.mode || 'sab';
                if (mode === 'sab') {
                    initRingBuffer(data.sharedBuffer, data.ringBufferBase, data.bufferConstants);
                }
                self.postMessage({ type: 'initialized' });
                break;

            case 'start':
                if (mode === 'sab') {
                    start();
                }
                // In postMessage mode, we just wait for debugRaw messages
                break;

            case 'stop':
                stop();
                break;

            case 'clear':
                if (mode === 'sab') {
                    clear();
                }
                break;

            case 'debugRaw':
                // PostMessage mode: decode raw bytes from AudioWorklet
                if (data.messages) {
                    decodeRawMessages(data.messages);
                }
                break;

            default:
                console.warn('[DebugWorker] Unknown message type:', data.type);
        }
    } catch (error) {
        console.error('[DebugWorker] Error:', error);
        self.postMessage({
            type: 'error',
            error: error.message
        });
    }
});

debugWorkerLog('[DebugWorker] Script loaded');

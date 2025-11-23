/*
    SuperSonic - SuperCollider AudioWorklet WebAssembly port
    Copyright (c) 2025 Sam Aaron

    Based on SuperCollider by James McCartney and community
    GPL v3 or later
*/

/**
 * OSC IN Worker - Receives OSC messages from scsynth
 * Uses Atomics.wait() for instant wake when data arrives
 * Reads from OUT ring buffer and forwards to main thread
 */

import * as MetricsOffsets from '../lib/metrics_offsets.js';

// Ring buffer configuration
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

const oscInLog = (...args) => {
    if (__DEV__) {
        console.log(...args);
    }
};

// Sequence tracking for dropped message detection
let lastSequenceReceived = -1;

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
        OUT_HEAD: (ringBufferBase + bufferConstants.CONTROL_START + 8) / 4,
        OUT_TAIL: (ringBufferBase + bufferConstants.CONTROL_START + 12) / 4
    };

    // Initialize metrics view
    const metricsBase = ringBufferBase + bufferConstants.METRICS_START;
    metricsView = new Uint32Array(sharedBuffer, metricsBase, bufferConstants.METRICS_SIZE / 4);
};

/**
 * Read all available messages from OUT buffer
 */
const readMessages = () => {
    const head = Atomics.load(atomicView, CONTROL_INDICES.OUT_HEAD);
    const tail = Atomics.load(atomicView, CONTROL_INDICES.OUT_TAIL);

    const messages = [];

    if (head === tail) {
        return messages; // No messages
    }

    let currentTail = tail;
    let messagesRead = 0;
    const maxMessages = 100;

    while (currentTail !== head && messagesRead < maxMessages) {
        const bytesToEnd = bufferConstants.OUT_BUFFER_SIZE - currentTail;
        if (bytesToEnd < bufferConstants.MESSAGE_HEADER_SIZE) {
            currentTail = 0;
            continue;
        }

        const readPos = ringBufferBase + bufferConstants.OUT_BUFFER_START + currentTail;

        // Read message header (now contiguous or wrapped)
        const magic = dataView.getUint32(readPos, true);

        // Check for padding marker - skip to beginning
        if (magic === bufferConstants.PADDING_MAGIC) {
            currentTail = 0;
            continue;
        }

        if (magic !== bufferConstants.MESSAGE_MAGIC) {
            console.error('[OSCInWorker] Corrupted message at position', currentTail);
            if (metricsView) Atomics.add(metricsView, MetricsOffsets.OSC_IN_DROPPED_MESSAGES, 1);
            // Skip this byte and continue
            currentTail = (currentTail + 1) % bufferConstants.OUT_BUFFER_SIZE;
            continue;
        }

        const length = dataView.getUint32(readPos + 4, true);
        const sequence = dataView.getUint32(readPos + 8, true);

        // Validate message length
        if (length < bufferConstants.MESSAGE_HEADER_SIZE || length > bufferConstants.OUT_BUFFER_SIZE) {
            console.error('[OSCInWorker] Invalid message length:', length);
            if (metricsView) Atomics.add(metricsView, MetricsOffsets.OSC_IN_DROPPED_MESSAGES, 1);
            currentTail = (currentTail + 1) % bufferConstants.OUT_BUFFER_SIZE;
            continue;
        }

        // Check for dropped messages via sequence
        if (lastSequenceReceived >= 0) {
            const expectedSeq = (lastSequenceReceived + 1) & 0xFFFFFFFF;
            if (sequence !== expectedSeq) {
                const dropped = (sequence - expectedSeq + 0x100000000) & 0xFFFFFFFF;
                if (dropped < 1000) { // Sanity check
                    console.warn('[OSCInWorker] Detected', dropped, 'dropped messages (expected seq', expectedSeq, 'got', sequence, ')');
                    if (metricsView) Atomics.add(metricsView, MetricsOffsets.OSC_IN_DROPPED_MESSAGES, dropped);
                }
            }
        }
        lastSequenceReceived = sequence;

        // Read payload (OSC binary data) - now contiguous due to padding
        const payloadLength = length - bufferConstants.MESSAGE_HEADER_SIZE;
        const payloadStart = readPos + bufferConstants.MESSAGE_HEADER_SIZE;

        // Create a proper copy (not a view into SharedArrayBuffer)
        const payload = new Uint8Array(payloadLength);
        for (let i = 0; i < payloadLength; i++) {
            payload[i] = uint8View[payloadStart + i];
        }

        messages.push({
            oscData: payload,
            sequence
        });

        // Move to next message
        currentTail = (currentTail + length) % bufferConstants.OUT_BUFFER_SIZE;
        messagesRead++;
        if (metricsView) {
            Atomics.add(metricsView, MetricsOffsets.OSC_IN_MESSAGES_RECEIVED, 1);
            Atomics.add(metricsView, MetricsOffsets.OSC_IN_BYTES_RECEIVED, payloadLength);
        }
    }

    // Update tail pointer (consume messages)
    if (messagesRead > 0) {
        Atomics.store(atomicView, CONTROL_INDICES.OUT_TAIL, currentTail);
    }

    return messages;
};

/**
 * Main wait loop using Atomics.wait for instant wake
 */
const waitLoop = () => {
    while (running) {
        try {
            // Get current OUT_HEAD value
            const currentHead = Atomics.load(atomicView, CONTROL_INDICES.OUT_HEAD);
            const currentTail = Atomics.load(atomicView, CONTROL_INDICES.OUT_TAIL);

            // If buffer is empty, wait for AudioWorklet to notify us
            if (currentHead === currentTail) {
                // Wait for up to 100ms (allows checking stop signal)
                const result = Atomics.wait(atomicView, CONTROL_INDICES.OUT_HEAD, currentHead, 100);

                if (result === 'ok' || result === 'not-equal') {
                    // We were notified or value changed!
                } else if (result === 'timed-out') {
                    continue; // Check running flag
                }
            }

            // Read all available messages
            const messages = readMessages();

            if (messages.length > 0) {
                // Send to main thread
                self.postMessage({
                    type: 'messages',
                    messages
                });
            }

        } catch (error) {
            console.error('[OSCInWorker] Error in wait loop:', error);
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
        console.error('[OSCInWorker] Cannot start - not initialized');
        return;
    }

    if (running) {
        console.warn('[OSCInWorker] Already running');
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
 * Handle messages from main thread
 */
self.addEventListener('message', (event) => {
    const { data } = event;

    try {
        switch (data.type) {
            case 'init':
                initRingBuffer(data.sharedBuffer, data.ringBufferBase, data.bufferConstants);
                self.postMessage({ type: 'initialized' });
                break;

            case 'start':
                start();
                break;

            case 'stop':
                stop();
                break;

            default:
                console.warn('[OSCInWorker] Unknown message type:', data.type);
        }
    } catch (error) {
        console.error('[OSCInWorker] Error:', error);
        self.postMessage({
            type: 'error',
            error: error.message
        });
    }
});

oscInLog('[OSCInWorker] Script loaded');

// SPDX-License-Identifier: MIT OR GPL-3.0-or-later
// Copyright (c) 2025 Sam Aaron

/**
 * OSC IN Worker - Receives OSC messages from scsynth
 * Uses Atomics.wait() for instant wake when data arrives
 * Reads from OUT ring buffer and forwards to main thread
 */

import * as MetricsOffsets from '../lib/metrics_offsets.js';
import { readMessagesFromBuffer } from '../lib/ring_buffer_core.js';
import { calculateOutControlIndices } from '../lib/control_offsets.js';

// NTP timestamp helper (inline to avoid import in worker)
const NTP_EPOCH_OFFSET = 2208988800;
const getCurrentNTP = () => (performance.timeOrigin + performance.now()) / 1000 + NTP_EPOCH_OFFSET;

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
    CONTROL_INDICES = calculateOutControlIndices(ringBufferBase, bufferConstants.CONTROL_START);

    // Initialize metrics view
    const metricsBase = ringBufferBase + bufferConstants.METRICS_START;
    metricsView = new Uint32Array(sharedBuffer, metricsBase, bufferConstants.METRICS_SIZE / 4);
};

/**
 * Read all available messages from OUT buffer
 * Uses shared ring_buffer_core for read logic
 */
const readMessages = () => {
    const head = Atomics.load(atomicView, CONTROL_INDICES.OUT_HEAD);
    const tail = Atomics.load(atomicView, CONTROL_INDICES.OUT_TAIL);

    if (head === tail) {
        return []; // No messages
    }

    const messages = [];

    const { newTail, messagesRead } = readMessagesFromBuffer({
        uint8View,
        dataView,
        bufferStart: ringBufferBase + bufferConstants.OUT_BUFFER_START,
        bufferSize: bufferConstants.OUT_BUFFER_SIZE,
        head,
        tail,
        messageMagic: bufferConstants.MESSAGE_MAGIC,
        paddingMagic: bufferConstants.PADDING_MAGIC,
        headerSize: bufferConstants.MESSAGE_HEADER_SIZE,
        maxMessages: 100,
        onMessage: (payloadOffset, payloadLength, sequence, sourceId) => {
            // Check for dropped messages via sequence
            if (lastSequenceReceived >= 0) {
                const expectedSeq = (lastSequenceReceived + 1) & 0xFFFFFFFF;
                if (sequence !== expectedSeq) {
                    const dropped = (sequence - expectedSeq + 0x100000000) & 0xFFFFFFFF;
                    if (dropped < 1000) { // Sanity check
                        console.error('[OSCInWorker] Detected', dropped, 'dropped messages (expected seq', expectedSeq, 'got', sequence, ')');
                        if (metricsView) Atomics.add(metricsView, MetricsOffsets.OSC_IN_DROPPED_MESSAGES, dropped);
                    }
                }
            }
            lastSequenceReceived = sequence;

            // Worker can allocate - it's not in the audio thread
            // Copy the data since ring buffer may be overwritten
            const oscData = new Uint8Array(payloadLength);
            for (let i = 0; i < payloadLength; i++) {
                oscData[i] = uint8View[payloadOffset + i];
            }

            messages.push({
                oscData,
                sequence,
                timestamp: getCurrentNTP()
            });

            // Update metrics
            if (metricsView) {
                Atomics.add(metricsView, MetricsOffsets.OSC_IN_MESSAGES_RECEIVED, 1);
                Atomics.add(metricsView, MetricsOffsets.OSC_IN_BYTES_RECEIVED, payloadLength);
            }
        },
        onCorruption: (position) => {
            console.error('[OSCInWorker] Corrupted message at position', position);
            if (metricsView) {
                Atomics.add(metricsView, MetricsOffsets.OSC_IN_DROPPED_MESSAGES, 1);
                Atomics.add(metricsView, MetricsOffsets.OSC_IN_CORRUPTED, 1);
            }
        }
    });

    // Update tail pointer (consume messages)
    if (messagesRead > 0) {
        Atomics.store(atomicView, CONTROL_INDICES.OUT_TAIL, newTail);
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
                Atomics.wait(atomicView, CONTROL_INDICES.OUT_HEAD, currentHead);
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
        if (__DEV__) console.warn('[OSCInWorker] Already running');
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
                if (__DEV__) console.warn('[OSCInWorker] Unknown message type:', data.type);
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

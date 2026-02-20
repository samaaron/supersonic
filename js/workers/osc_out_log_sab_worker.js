// SPDX-License-Identifier: MIT OR GPL-3.0-or-later
// Copyright (c) 2025 Sam Aaron

/**
 * OSC OUT Log Worker (SAB Mode) - Logs OSC messages sent to scsynth
 * Uses Atomics.wait() for instant wake when data arrives
 * Reads from IN ring buffer and forwards to main thread for logging
 *
 * Unlike osc_in_worker which consumes messages (advancing IN_TAIL),
 * this worker only reads messages for logging purposes using IN_LOG_TAIL.
 * The C++ scsynth advances IN_TAIL when it processes messages.
 */

import { readMessagesFromBuffer } from '../lib/ring_buffer_core.js';
import { calculateInControlIndices } from '../lib/control_offsets.js';

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

// Worker state
let running = false;

const oscOutLogLog = (...args) => {
    if (__DEV__) {
        console.log('[OSCOutLogWorker]', ...args);
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
    CONTROL_INDICES = calculateInControlIndices(ringBufferBase, bufferConstants.CONTROL_START);

    // Initialize IN_LOG_TAIL to current IN_HEAD value
    // This ensures we only log messages from this point forward
    const currentHead = Atomics.load(atomicView, CONTROL_INDICES.IN_HEAD);
    Atomics.store(atomicView, CONTROL_INDICES.IN_LOG_TAIL, currentHead);

    oscOutLogLog('Initialized, IN_LOG_TAIL set to', currentHead);
};

/**
 * Peek at the magic uint32 at a given relative buffer position,
 * handling wrap-around if the 4 bytes straddle the buffer boundary.
 */
const peekMagic = (relativeOffset) => {
    const bufferStart = ringBufferBase + bufferConstants.IN_BUFFER_START;
    const bufferSize = bufferConstants.IN_BUFFER_SIZE;
    if (relativeOffset + 4 <= bufferSize) {
        return dataView.getUint32(bufferStart + relativeOffset, true);
    }
    let val = 0;
    for (let b = 0; b < 4; b++) {
        val |= uint8View[bufferStart + ((relativeOffset + b) % bufferSize)] << (b * 8);
    }
    return val;
};

/**
 * Read all available messages from IN buffer for logging
 * Uses shared ring_buffer_core for read logic
 */
const readMessages = () => {
    const head = Atomics.load(atomicView, CONTROL_INDICES.IN_HEAD);
    const logTail = Atomics.load(atomicView, CONTROL_INDICES.IN_LOG_TAIL);

    if (head === logTail) {
        return []; // No new messages to log
    }

    // Check if logTail is still at a valid message boundary.
    // The writer only checks IN_TAIL (C++ consumer) for space, not IN_LOG_TAIL.
    // If the C++ consumer is fast and the buffer wraps, the writer can overwrite
    // positions the log worker hasn't read yet (lapping). Detect this by peeking
    // at the magic — if it's not MESSAGE_MAGIC, resync to head and skip this batch.
    const magic = peekMagic(logTail);
    if (magic !== bufferConstants.MESSAGE_MAGIC) {
        if (__DEV__) {
            console.warn(
                `[OSCOutLogWorker] Resyncing: invalid magic at logTail=${logTail}` +
                ` (got=0x${(magic >>> 0).toString(16).padStart(8, '0')}` +
                ` expected=0x${(bufferConstants.MESSAGE_MAGIC >>> 0).toString(16).padStart(8, '0')})` +
                ` head=${head} — writer likely lapped log reader, skipping to head`
            );
        }
        Atomics.store(atomicView, CONTROL_INDICES.IN_LOG_TAIL, head);
        return [];
    }

    const entries = [];

    const { newTail, messagesRead } = readMessagesFromBuffer({
        uint8View,
        dataView,
        bufferStart: ringBufferBase + bufferConstants.IN_BUFFER_START,
        bufferSize: bufferConstants.IN_BUFFER_SIZE,
        head,
        tail: logTail,
        messageMagic: bufferConstants.MESSAGE_MAGIC,
        paddingMagic: bufferConstants.PADDING_MAGIC,
        headerSize: bufferConstants.MESSAGE_HEADER_SIZE,
        maxMessages: 100,
        onMessage: (payloadOffset, payloadLength, sequence, sourceId) => {
            // SAB worker can allocate - it's not in the audio thread
            // Copy the data since ring buffer may be overwritten
            const oscData = new Uint8Array(payloadLength);
            for (let i = 0; i < payloadLength; i++) {
                oscData[i] = uint8View[payloadOffset + i];
            }
            entries.push({
                sourceId,
                oscData,
                sequence,
                timestamp: getCurrentNTP()
            });
        },
        onCorruption: (position) => {
            // Rate-limit corruption logs: first 3 in detail, then summary
            if (!readMessages._corruptCount) readMessages._corruptCount = 0;
            readMessages._corruptCount++;
            if (readMessages._corruptCount <= 3) {
                // Dump diagnostic info: head, logTail, bytes at position, expected magic
                const absPos = ringBufferBase + bufferConstants.IN_BUFFER_START + position;
                const got = dataView.getUint32(absPos, true);
                const byte0 = uint8View[absPos];
                const byte1 = uint8View[absPos + 1];
                const byte2 = uint8View[absPos + 2];
                const byte3 = uint8View[absPos + 3];
                const inTail = Atomics.load(atomicView, CONTROL_INDICES.IN_TAIL);
                console.error(
                    `[OSCOutLogWorker] Corrupted message at position ${position}:` +
                    ` head=${head} logTail=${logTail} inTail=${inTail}` +
                    ` got=0x${(got >>> 0).toString(16).padStart(8, '0')}` +
                    ` expected=0x${(bufferConstants.MESSAGE_MAGIC >>> 0).toString(16).padStart(8, '0')}` +
                    ` bytes=[${byte0},${byte1},${byte2},${byte3}]` +
                    ` bufStart=${ringBufferBase + bufferConstants.IN_BUFFER_START}` +
                    ` bufSize=${bufferConstants.IN_BUFFER_SIZE}`
                );
            } else if (readMessages._corruptCount === 4) {
                console.error(`[OSCOutLogWorker] Suppressing further corruption logs (${readMessages._corruptCount}+ total)`);
            }
        }
    });

    // Update log tail pointer (mark messages as logged)
    if (messagesRead > 0) {
        Atomics.store(atomicView, CONTROL_INDICES.IN_LOG_TAIL, newTail);
    }

    return entries;
};

/**
 * Main wait loop using Atomics.wait for instant wake
 */
const waitLoop = () => {
    while (running) {
        try {
            // Get current IN_HEAD and IN_LOG_TAIL values
            const currentHead = Atomics.load(atomicView, CONTROL_INDICES.IN_HEAD);
            const currentLogTail = Atomics.load(atomicView, CONTROL_INDICES.IN_LOG_TAIL);

            // If no new messages, wait for ring_buffer_writer to notify us
            if (currentHead === currentLogTail) {
                Atomics.wait(atomicView, CONTROL_INDICES.IN_HEAD, currentHead);
            }

            // Read all available messages
            const entries = readMessages();

            if (entries.length > 0) {
                // Send to main thread
                self.postMessage({
                    type: 'oscLog',
                    entries
                });
            }

        } catch (error) {
            console.error('[OSCOutLogWorker] Error in wait loop:', error);
            self.postMessage({
                type: 'error',
                error: error.message
            });

            // Brief pause on error before retrying
            Atomics.wait(atomicView, 0, atomicView[0], 10);
        }
    }
};

/**
 * Start the wait loop
 */
const start = () => {
    if (!sharedBuffer) {
        console.error('[OSCOutLogWorker] Cannot start - not initialized');
        return;
    }

    if (running) {
        if (__DEV__) console.warn('[OSCOutLogWorker] Already running');
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
                if (__DEV__) console.warn('[OSCOutLogWorker] Unknown message type:', data.type);
        }
    } catch (error) {
        console.error('[OSCOutLogWorker] Error:', error);
        self.postMessage({
            type: 'error',
            error: error.message
        });
    }
});

oscOutLogLog('Script loaded');

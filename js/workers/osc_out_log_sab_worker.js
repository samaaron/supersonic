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
import { getCurrentNTPFromPerformance as getCurrentNTP } from '../lib/osc_classifier.js';
import { runSabWorker } from '../lib/sab_worker_loop.js';

let corruptCount = 0;

/**
 * Read all available messages from IN buffer for logging
 */
function readLogMessages(ctx) {
    const { atomicView, uint8View, dataView, ringBufferBase, bufferConstants, CONTROL_INDICES } = ctx;
    const head = Atomics.load(atomicView, CONTROL_INDICES.IN_HEAD);
    const logTail = Atomics.load(atomicView, CONTROL_INDICES.IN_LOG_TAIL);
    if (head === logTail) return [];

    // Check if logTail is still at a valid message boundary.
    // The writer only checks IN_TAIL (C++ consumer) for space, not IN_LOG_TAIL.
    // If the C++ consumer is fast and the buffer wraps, the writer can overwrite
    // positions the log worker hasn't read yet (lapping). Detect this by peeking
    // at the magic — if it's not MESSAGE_MAGIC, resync to head and skip this batch.
    const bufferStart = ringBufferBase + bufferConstants.IN_BUFFER_START;
    const bufferSize = bufferConstants.IN_BUFFER_SIZE;
    let magic;
    if (logTail + 4 <= bufferSize) {
        magic = dataView.getUint32(bufferStart + logTail, true);
    } else {
        magic = 0;
        for (let b = 0; b < 4; b++) {
            magic |= uint8View[bufferStart + ((logTail + b) % bufferSize)] << (b * 8);
        }
    }

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
        bufferStart,
        bufferSize,
        head,
        tail: logTail,
        messageMagic: bufferConstants.MESSAGE_MAGIC,
        paddingMagic: bufferConstants.PADDING_MAGIC,
        headerSize: bufferConstants.MESSAGE_HEADER_SIZE,
        maxMessages: 100,
        onMessage: (payloadOffset, payloadLength, sequence, sourceId) => {
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
            corruptCount++;
            if (corruptCount <= 3) {
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
                    ` bufStart=${bufferStart} bufSize=${bufferSize}`
                );
            } else if (corruptCount === 4) {
                console.error(`[OSCOutLogWorker] Suppressing further corruption logs (${corruptCount}+ total)`);
            }
        }
    });

    if (messagesRead > 0) {
        Atomics.store(atomicView, CONTROL_INDICES.IN_LOG_TAIL, newTail);
    }
    return entries;
}

runSabWorker({
    name: 'OSCOutLogWorker',
    calculateControlIndices: calculateInControlIndices,
    headIndex: (idx) => idx.IN_HEAD,
    tailIndex: (idx) => idx.IN_LOG_TAIL,
    readMessages: readLogMessages,
    postResults: (entries) => self.postMessage({ type: 'oscLog', entries }),
    initMetrics: false,
    onInit: (ctx) => {
        // Initialize IN_LOG_TAIL to current IN_HEAD value
        // This ensures we only log messages from this point forward
        const currentHead = Atomics.load(ctx.atomicView, ctx.CONTROL_INDICES.IN_HEAD);
        Atomics.store(ctx.atomicView, ctx.CONTROL_INDICES.IN_LOG_TAIL, currentHead);
        if (__DEV__) console.log('[OSCOutLogWorker] Initialized, IN_LOG_TAIL set to', currentHead);
    },
});

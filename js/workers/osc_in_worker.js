// SPDX-License-Identifier: MIT OR GPL-3.0-or-later
// Copyright (c) 2025 Sam Aaron

/**
 * OSC IN Worker — drains the OUT ring buffer and forwards every reply to the
 * main thread. The main thread fans out to registered callbacks; this worker
 * is a dumb pump (Atomics.wait for instant wake, copy out, postMessage).
 */

import * as MetricsOffsets from '../lib/metrics_offsets.js';
import { readMessagesFromBuffer } from '../lib/ring_buffer_core.js';
import { calculateOutControlIndices } from '../lib/control_offsets.js';
import { getCurrentNTPFromPerformance as getCurrentNTP } from '../lib/osc_classifier.js';
import { runSabWorker } from '../lib/sab_worker_loop.js';

// Sequence tracking for dropped message detection
let lastSequenceReceived = -1;

/**
 * Read all available OSC messages from the OUT ring buffer
 */
function readOscMessages(ctx) {
    const { atomicView, uint8View, dataView, ringBufferBase, bufferConstants, metricsView, CONTROL_INDICES } = ctx;
    const head = Atomics.load(atomicView, CONTROL_INDICES.OUT_HEAD);
    const tail = Atomics.load(atomicView, CONTROL_INDICES.OUT_TAIL);
    if (head === tail) return [];

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

            // Egress frames are [route:u32][osc]; strip the route word.
            const ROUTE_SIZE = 4;
            const oscOffset = payloadOffset + ROUTE_SIZE;
            const oscLength = payloadLength - ROUTE_SIZE;
            if (oscLength <= 0) return;

            // Copy the data since ring buffer may be overwritten
            const oscData = new Uint8Array(oscLength);
            for (let i = 0; i < oscLength; i++) {
                oscData[i] = uint8View[oscOffset + i];
            }

            messages.push({
                oscData,
                sequence,
                timestamp: getCurrentNTP()
            });

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

    if (messagesRead > 0) {
        Atomics.store(atomicView, CONTROL_INDICES.OUT_TAIL, newTail);
    }
    return messages;
}

runSabWorker({
    name: 'OSCInWorker',
    calculateControlIndices: calculateOutControlIndices,
    headIndex: (idx) => idx.OUT_HEAD,
    tailIndex: (idx) => idx.OUT_TAIL,
    readMessages: readOscMessages,
    postResults: (messages) => self.postMessage({ type: 'messages', messages }),
});

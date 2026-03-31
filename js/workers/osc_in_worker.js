// SPDX-License-Identifier: MIT OR GPL-3.0-or-later
// Copyright (c) 2025 Sam Aaron

/**
 * OSC IN Worker - Receives OSC messages from scsynth
 * Uses Atomics.wait() for instant wake when data arrives
 * Reads from OUT ring buffer and forwards to main thread
 */

import * as MetricsOffsets from '../lib/metrics_offsets.js';
import { readMessagesFromBuffer, writeMessageToBuffer, calculateAvailableSpace } from '../lib/ring_buffer_core.js';
import { calculateOutControlIndices } from '../lib/control_offsets.js';
import { getCurrentNTPFromPerformance as getCurrentNTP } from '../lib/osc_classifier.js';
import { runSabWorker } from '../lib/sab_worker_loop.js';

// Sequence tracking for dropped message detection
let lastSequenceReceived = -1;

// Per-channel reply buffer fan-out state (populated on init)
let replyChannels = null;  // Array of { bufferStart, bufferSize, headIndex, tailIndex, activeIndex }
let replyHeaderScratch = null;  // Pre-allocated scratch for wrap-around writes
let replyHeaderScratchView = null;

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

            // Fan out to active reply channel buffers
            if (replyChannels) {
                for (let ch = 0; ch < replyChannels.length; ch++) {
                    const rc = replyChannels[ch];
                    if (Atomics.load(atomicView, rc.activeIndex) !== 1) continue;

                    const chHead = Atomics.load(atomicView, rc.headIndex);
                    const chTail = Atomics.load(atomicView, rc.tailIndex);
                    const available = calculateAvailableSpace(chHead, chTail, rc.bufferSize);
                    const needed = ((bufferConstants.MESSAGE_HEADER_SIZE + payloadLength) + 3) & ~3;
                    if (needed > available) continue;  // Channel buffer full — drop for this channel

                    const newHead = writeMessageToBuffer({
                        uint8View, dataView,
                        bufferStart: rc.bufferStart,
                        bufferSize: rc.bufferSize,
                        head: chHead,
                        payload: oscData,
                        sequence,
                        messageMagic: bufferConstants.MESSAGE_MAGIC,
                        headerSize: bufferConstants.MESSAGE_HEADER_SIZE,
                        headerScratch: replyHeaderScratch,
                        headerScratchView: replyHeaderScratchView,
                    });
                    Atomics.store(atomicView, rc.headIndex, newHead);
                    Atomics.notify(atomicView, rc.headIndex);
                }
            }

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
    onInit: (ctx) => {
        const bc = ctx.bufferConstants;
        if (!bc.REPLY_CHANNEL_COUNT) return;  // Old WASM without reply channels

        // Pre-allocate scratch buffers for wrap-around writes (avoids allocation per reply)
        replyHeaderScratch = new Uint8Array(bc.MESSAGE_HEADER_SIZE);
        replyHeaderScratchView = new DataView(replyHeaderScratch.buffer);

        // Compute per-channel offsets.
        // Control region: REPLY_CHANNELS_CONTROL_START + (i * 12) → [head(4), tail(4), active(4)]
        // Buffer region:  REPLY_CHANNELS_BUFFER_START + (i * REPLY_CHANNEL_BUFFER_SIZE)
        const base = ctx.ringBufferBase;
        replyChannels = [];
        for (let i = 0; i < bc.REPLY_CHANNEL_COUNT; i++) {
            const controlBase = base + bc.REPLY_CHANNELS_CONTROL_START + (i * bc.REPLY_CHANNEL_CONTROL_SIZE);
            replyChannels.push({
                bufferStart: base + bc.REPLY_CHANNELS_BUFFER_START + (i * bc.REPLY_CHANNEL_BUFFER_SIZE),
                bufferSize: bc.REPLY_CHANNEL_BUFFER_SIZE,
                headIndex: controlBase / 4,       // Int32Array index
                tailIndex: (controlBase + 4) / 4,
                activeIndex: (controlBase + 8) / 4,
            });
        }
    },
});

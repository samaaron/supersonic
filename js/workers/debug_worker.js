// SPDX-License-Identifier: MIT OR GPL-3.0-or-later
// Copyright (c) 2025 Sam Aaron

/**
 * DEBUG Worker - Receives debug messages from AudioWorklet
 * Uses Atomics.wait() for instant wake when debug logs arrive
 * Reads from DEBUG ring buffer and forwards to main thread
 */

import * as MetricsOffsets from '../lib/metrics_offsets.js';
import { readMessagesFromBuffer } from '../lib/ring_buffer_core.js';
import { calculateDebugControlIndices } from '../lib/control_offsets.js';
import { runSabWorker } from '../lib/sab_worker_loop.js';

const textDecoder = new TextDecoder('utf-8');

/**
 * Decode a debug payload from the ring buffer into text
 */
function decodeDebugPayload(uint8View, payloadOffset, payloadLength) {
    // Must copy to a regular buffer since TextDecoder cannot decode SharedArrayBuffer views
    const payload = new Uint8Array(payloadLength);
    for (let i = 0; i < payloadLength; i++) {
        payload[i] = uint8View[payloadOffset + i];
    }
    let text = textDecoder.decode(payload);
    if (text.endsWith('\n')) text = text.slice(0, -1);
    return text;
}

/**
 * Read debug messages from the DEBUG ring buffer
 */
function readDebugMessages(ctx) {
    const { atomicView, uint8View, dataView, ringBufferBase, bufferConstants, metricsView, CONTROL_INDICES } = ctx;
    const head = Atomics.load(atomicView, CONTROL_INDICES.DEBUG_HEAD);
    const tail = Atomics.load(atomicView, CONTROL_INDICES.DEBUG_TAIL);
    if (head === tail) return [];

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
        onMessage: (payloadOffset, payloadLength, sequence) => {
            messages.push({
                text: decodeDebugPayload(uint8View, payloadOffset, payloadLength),
                timestamp: performance.now(),
                sequence
            });
            if (metricsView) {
                Atomics.add(metricsView, MetricsOffsets.DEBUG_MESSAGES_RECEIVED, 1);
                Atomics.add(metricsView, MetricsOffsets.DEBUG_BYTES_RECEIVED, payloadLength);
            }
        },
        onCorruption: (position) => {
            console.error('[DebugWorker] Corrupted message at position', position);
        }
    });

    if (messagesRead > 0) {
        Atomics.store(atomicView, CONTROL_INDICES.DEBUG_TAIL, newTail);
    }
    return messages;
}

/**
 * Decode raw debug bytes from postMessage mode
 */
function decodeRawMessages(rawMessages) {
    const messages = [];
    for (const raw of rawMessages) {
        try {
            let text = textDecoder.decode(new Uint8Array(raw.bytes));
            if (text.endsWith('\n')) text = text.slice(0, -1);
            messages.push({ text, timestamp: performance.now(), sequence: raw.sequence });
        } catch (err) {
            console.error('[DebugWorker] Failed to decode message:', err);
        }
    }
    if (messages.length > 0) {
        self.postMessage({ type: 'debug', messages });
    }
}

runSabWorker({
    name: 'DebugWorker',
    calculateControlIndices: calculateDebugControlIndices,
    headIndex: (idx) => idx.DEBUG_HEAD,
    tailIndex: (idx) => idx.DEBUG_TAIL,
    readMessages: readDebugMessages,
    postResults: (messages) => self.postMessage({ type: 'debug', messages }),
    extraHandlers: {
        clear: (_data, ctx) => {
            if (!ctx.sharedBuffer) return;
            Atomics.store(ctx.atomicView, ctx.CONTROL_INDICES.DEBUG_HEAD, 0);
            Atomics.store(ctx.atomicView, ctx.CONTROL_INDICES.DEBUG_TAIL, 0);
        },
        debugRaw: (data) => {
            if (data.messages) decodeRawMessages(data.messages);
        },
    },
});

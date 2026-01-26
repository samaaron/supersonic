// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2025 Sam Aaron

/**
 * OSC OUT Log Worker (PostMessage Mode) - Decodes OSC log entries
 * Receives raw bytes from worklet via MessagePort, parses them,
 * and emits structured entries to main thread.
 *
 * In PM mode, the worklet reads raw bytes from the IN buffer and sends them
 * here for parsing. This keeps the worklet fast (no parsing in audio thread)
 * and allows TextDecoder usage (not available in AudioWorklet).
 */

import { readMessagesFromBuffer } from '../lib/ring_buffer_core.js';

// Buffer constants (received at initialization)
let bufferConstants = null;

// MessagePort connected to the worklet
let workletPort = null;

const oscOutLogPMLog = (...args) => {
    if (__DEV__) {
        console.log('[OSCOutLogPMWorker]', ...args);
    }
};

/**
 * Parse raw buffer bytes into OSC log entries
 * @param {ArrayBuffer} buffer - Raw bytes from ring buffer
 * @param {number} timestamp - Timestamp when bytes were captured
 * @returns {Array} Array of {sourceId, oscData, timestamp}
 */
const parseBufferChunks = (chunks, timestamp) => {
    if (!bufferConstants) {
        console.error('[OSCOutLogPMWorker] Not initialized');
        return [];
    }

    // If we have multiple chunks (wrap-around), concatenate them
    let combinedBytes;
    if (chunks.length === 1) {
        combinedBytes = new Uint8Array(chunks[0].bytes);
    } else {
        // Calculate total size
        const totalSize = chunks.reduce((sum, chunk) => sum + chunk.bytes.byteLength, 0);
        combinedBytes = new Uint8Array(totalSize);

        // Copy chunks in order
        let offset = 0;
        for (const chunk of chunks) {
            const chunkBytes = new Uint8Array(chunk.bytes);
            combinedBytes.set(chunkBytes, offset);
            offset += chunkBytes.length;
        }
    }

    // Create views for parsing
    const uint8View = combinedBytes;
    const dataView = new DataView(combinedBytes.buffer);

    const entries = [];

    // Parse messages from the combined buffer
    // Note: We're reading from a snapshot, so head = buffer length, tail = 0
    readMessagesFromBuffer({
        uint8View,
        dataView,
        bufferStart: 0,
        bufferSize: combinedBytes.length,
        head: combinedBytes.length,
        tail: 0,
        messageMagic: bufferConstants.MESSAGE_MAGIC,
        paddingMagic: bufferConstants.PADDING_MAGIC,
        headerSize: bufferConstants.MESSAGE_HEADER_SIZE,
        onMessage: (payload, sequence, length, sourceId) => {
            entries.push({
                sourceId,
                oscData: new Uint8Array(payload),
                timestamp
            });
        },
        onCorruption: (position) => {
            console.error('[OSCOutLogPMWorker] Corrupted message at position', position);
        }
    });

    return entries;
};

/**
 * Handle messages from worklet via MessagePort
 */
const handleWorkletMessage = (event) => {
    const { data } = event;

    if (data.type === 'oscLogRaw') {
        // Raw bytes format (for future use if needed)
        const entries = parseBufferChunks(data.chunks, data.timestamp || performance.now());

        if (entries.length > 0) {
            // Send to main thread
            self.postMessage({
                type: 'oscLog',
                entries
            });
        }
    } else if (data.type === 'oscLogEntries') {
        // Structured entries format (PM mode uses this)
        // Worklet already has the parsed data, just forward to main thread
        if (data.entries && data.entries.length > 0) {
            self.postMessage({
                type: 'oscLog',
                entries: data.entries
            });
        }
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
                bufferConstants = data.bufferConstants;

                // Check if worklet port was transferred
                if (event.ports && event.ports.length > 0) {
                    workletPort = event.ports[0];
                    workletPort.onmessage = handleWorkletMessage;
                    oscOutLogPMLog('Initialized with worklet port');
                } else {
                    oscOutLogPMLog('Initialized without worklet port');
                }

                self.postMessage({ type: 'initialized' });
                break;

            case 'setWorkletPort':
                // Alternative: receive port separately
                if (event.ports && event.ports.length > 0) {
                    workletPort = event.ports[0];
                    workletPort.onmessage = handleWorkletMessage;
                    oscOutLogPMLog('Worklet port set');
                }
                break;

            default:
                console.warn('[OSCOutLogPMWorker] Unknown message type:', data.type);
        }
    } catch (error) {
        console.error('[OSCOutLogPMWorker] Error:', error);
        self.postMessage({
            type: 'error',
            error: error.message
        });
    }
});

oscOutLogPMLog('Script loaded');

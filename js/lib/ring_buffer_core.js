// SPDX-License-Identifier: MIT OR GPL-3.0-or-later
// Copyright (c) 2025 Sam Aaron

/*
    Ring Buffer Core
    Shared module for ring buffer read/write operations.
    Used by both SAB mode (workers) and postMessage mode (audioworklet).

    This module contains pure data operations - no atomics, no locking.
    Callers are responsible for concurrency control and pointer updates.
*/

/**
 * Calculate available space in a circular buffer.
 * @param {number} head - Current head position
 * @param {number} tail - Current tail position
 * @param {number} bufferSize - Total buffer size
 * @returns {number} bytes available for writing
 */
export function calculateAvailableSpace(head, tail, bufferSize) {
    return (bufferSize - 1 - head + tail) % bufferSize;
}

/**
 * Write a message to ring buffer, handling wrap-around at buffer boundary.
 * Pure data operation - no atomics, no locking.
 * Aligns total message length to 4 bytes.
 *
 * @param {Object} params - Write parameters
 * @param {Uint8Array} params.uint8View - Uint8Array view of memory
 * @param {DataView} params.dataView - DataView of memory
 * @param {number} params.bufferStart - Absolute byte offset of buffer start in memory
 * @param {number} params.bufferSize - Size of the ring buffer
 * @param {number} params.head - Current head position (relative to buffer start)
 * @param {Uint8Array} params.payload - Message payload to write
 * @param {number} params.sequence - Sequence number for this message
 * @param {number} params.messageMagic - Magic number for message header (e.g., 0xDEADBEEF)
 * @param {number} params.headerSize - Size of message header (typically 16)
 * @param {number} [params.sourceId=0] - Source ID for logging (0 = main, 1+ = workers)
 * @returns {number} new head position (aligned), relative to buffer start
 */
export function writeMessageToBuffer({
    uint8View,
    dataView,
    bufferStart,
    bufferSize,
    head,
    payload,
    sequence,
    messageMagic,
    headerSize,
    sourceId = 0
}) {
    const payloadSize = payload.length;
    const totalSize = headerSize + payloadSize;
    // Align to 4 bytes
    const alignedSize = (totalSize + 3) & ~3;

    // Calculate space to end of buffer
    const spaceToEnd = bufferSize - head;

    if (alignedSize > spaceToEnd) {
        // Message will wrap - write in two parts
        const headerBytes = new Uint8Array(headerSize);
        const headerView = new DataView(headerBytes.buffer);
        headerView.setUint32(0, messageMagic, true);
        headerView.setUint32(4, alignedSize, true);
        headerView.setUint32(8, sequence, true);
        headerView.setUint32(12, sourceId, true);  // sourceId (was padding)

        const writePos1 = bufferStart + head;
        const writePos2 = bufferStart;

        if (spaceToEnd >= headerSize) {
            // Header fits contiguously
            uint8View.set(headerBytes, writePos1);

            // Write payload (split across boundary)
            const payloadBytesInFirstPart = spaceToEnd - headerSize;
            if (payloadBytesInFirstPart > 0) {
                uint8View.set(payload.subarray(0, payloadBytesInFirstPart), writePos1 + headerSize);
            }
            uint8View.set(payload.subarray(payloadBytesInFirstPart), writePos2);
        } else {
            // Header is split across boundary
            uint8View.set(headerBytes.subarray(0, spaceToEnd), writePos1);
            uint8View.set(headerBytes.subarray(spaceToEnd), writePos2);

            // All payload goes at beginning after header remainder
            const payloadOffset = headerSize - spaceToEnd;
            uint8View.set(payload, writePos2 + payloadOffset);
        }
    } else {
        // Message fits contiguously - write normally
        const writePos = bufferStart + head;

        // Write header
        dataView.setUint32(writePos, messageMagic, true);
        dataView.setUint32(writePos + 4, alignedSize, true);
        dataView.setUint32(writePos + 8, sequence, true);
        dataView.setUint32(writePos + 12, sourceId, true);  // sourceId (was padding)

        // Write payload
        uint8View.set(payload, writePos + headerSize);
    }

    // Return new head position (aligned)
    return (head + alignedSize) % bufferSize;
}

/**
 * Read messages from ring buffer, handling wrap-around and padding markers.
 * Pure data operation - no atomics, no locking.
 * Calls onMessage callback for each valid message found.
 *
 * IMPORTANT: Callback receives offset/length, NOT a Uint8Array payload.
 * This design enables allocation-free reading in the AudioWorklet.
 * Caller must read directly from uint8View using the provided offset.
 *
 * @param {Object} params - Read parameters
 * @param {Uint8Array} params.uint8View - Uint8Array view of memory
 * @param {DataView} params.dataView - DataView of memory
 * @param {number} params.bufferStart - Absolute byte offset of buffer start in memory
 * @param {number} params.bufferSize - Size of the ring buffer
 * @param {number} params.head - Current head position (relative to buffer start)
 * @param {number} params.tail - Current tail position (relative to buffer start)
 * @param {number} params.messageMagic - Magic number for valid messages (e.g., 0xDEADBEEF)
 * @param {number} params.paddingMagic - Magic number for padding markers (e.g., 0xDEADFEED)
 * @param {number} params.headerSize - Size of message header (typically 16)
 * @param {number} [params.maxMessages=Infinity] - Maximum messages to read per call
 * @param {Function} params.onMessage - Callback: (payloadOffset: number, payloadLength: number, sequence: number, sourceId: number) => void
 *   payloadOffset is absolute byte offset into uint8View where payload starts
 *   Caller reads directly: uint8View[payloadOffset + i] for i in 0..payloadLength-1
 * @param {Function} [params.onCorruption] - Optional callback for corrupted messages: (position: number) => void
 * @returns {{ newTail: number, messagesRead: number }} new tail position and count
 */
export function readMessagesFromBuffer({
    uint8View,
    dataView,
    bufferStart,
    bufferSize,
    head,
    tail,
    messageMagic,
    paddingMagic,
    headerSize,
    maxMessages = Infinity,
    onMessage,
    onCorruption
}) {
    let currentTail = tail;
    let messagesRead = 0;

    while (currentTail !== head && messagesRead < maxMessages) {
        // Check if there's enough space for a header before the end
        const bytesToEnd = bufferSize - currentTail;
        if (bytesToEnd < headerSize) {
            // Not enough space for header, wrap to beginning
            currentTail = 0;
            continue;
        }

        const readPos = bufferStart + currentTail;

        // Read magic number
        const magic = dataView.getUint32(readPos, true);

        // Check for padding marker - skip to beginning
        if (magic === paddingMagic) {
            currentTail = 0;
            continue;
        }

        // Validate message magic
        if (magic !== messageMagic) {
            // Corrupted message - skip this byte
            if (onCorruption) {
                onCorruption(currentTail);
            }
            currentTail = (currentTail + 1) % bufferSize;
            continue;
        }

        // Read header fields
        const length = dataView.getUint32(readPos + 4, true);
        const sequence = dataView.getUint32(readPos + 8, true);
        const sourceId = dataView.getUint32(readPos + 12, true);

        // Validate message length
        if (length < headerSize || length > bufferSize) {
            // Invalid length - skip this byte
            if (onCorruption) {
                onCorruption(currentTail);
            }
            currentTail = (currentTail + 1) % bufferSize;
            continue;
        }

        // Calculate payload location (no allocation - just arithmetic)
        const payloadLength = length - headerSize;
        const payloadOffset = readPos + headerSize;

        // Call message handler with offset/length (caller reads directly from uint8View)
        onMessage(payloadOffset, payloadLength, sequence, sourceId);

        // Advance tail by message length (which may be aligned by writer)
        currentTail = (currentTail + length) % bufferSize;
        messagesRead++;
    }

    return { newTail: currentTail, messagesRead };
}

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
 * @param {Uint8Array} [params.headerScratch] - Pre-allocated buffer for header (avoids allocation on wrap)
 * @param {DataView} [params.headerScratchView] - Pre-allocated DataView for header
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
    sourceId = 0,
    headerScratch = null,
    headerScratchView = null
}) {
    const payloadSize = payload.length;
    const totalSize = headerSize + payloadSize;
    // Align to 4 bytes
    const alignedSize = (totalSize + 3) & ~3;

    // Calculate space to end of buffer
    const spaceToEnd = bufferSize - head;

    if (alignedSize > spaceToEnd) {
        // Message will wrap - write in two parts
        // Use pre-allocated scratch buffers if provided, otherwise allocate
        const headerBytes = headerScratch || new Uint8Array(headerSize);
        const headerView = headerScratchView || new DataView(headerBytes.buffer);
        headerView.setUint32(0, messageMagic, true);
        headerView.setUint32(4, alignedSize, true);
        headerView.setUint32(8, sequence, true);
        headerView.setUint32(12, sourceId, true);  // sourceId (was padding)

        const writePos1 = bufferStart + head;
        const writePos2 = bufferStart;

        if (spaceToEnd >= headerSize) {
            // Header fits contiguously
            uint8View.set(headerBytes, writePos1);

            // Write payload (split across boundary) — byte-by-byte to avoid subarray() allocation
            const payloadBytesInFirstPart = spaceToEnd - headerSize;
            for (let i = 0; i < payloadBytesInFirstPart; i++) {
                uint8View[writePos1 + headerSize + i] = payload[i];
            }
            for (let i = payloadBytesInFirstPart; i < payloadSize; i++) {
                uint8View[writePos2 + i - payloadBytesInFirstPart] = payload[i];
            }
        } else {
            // Header is split across boundary — byte-by-byte to avoid subarray() allocation
            for (let i = 0; i < spaceToEnd; i++) {
                uint8View[writePos1 + i] = headerBytes[i];
            }
            for (let i = spaceToEnd; i < headerSize; i++) {
                uint8View[writePos2 + i - spaceToEnd] = headerBytes[i];
            }

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
 *   Note: for OUT/DEBUG buffers (C++ writer), payloads never wrap — padding markers are used at buffer boundaries.
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

    // Helper: read a little-endian uint32 that may span the buffer boundary
    const readU32Wrap = (relativeOffset) => {
        const absOff = relativeOffset;
        if (absOff + 4 <= bufferSize) {
            return dataView.getUint32(bufferStart + absOff, true);
        }
        // Split across boundary — read byte by byte (little-endian)
        let val = 0;
        for (let b = 0; b < 4; b++) {
            val |= uint8View[bufferStart + ((absOff + b) % bufferSize)] << (b * 8);
        }
        return val;
    };

    while (currentTail !== head && messagesRead < maxMessages) {
        const bytesToEnd = bufferSize - currentTail;

        // Read magic — may be split across buffer boundary
        let magic;
        if (bytesToEnd >= 4) {
            magic = dataView.getUint32(bufferStart + currentTail, true);
        } else {
            magic = readU32Wrap(currentTail);
        }

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

        // Read header fields (may span the boundary, matching C++ split-read approach)
        const length = readU32Wrap((currentTail + 4) % bufferSize);
        const sequence = readU32Wrap((currentTail + 8) % bufferSize);
        const sourceId = readU32Wrap((currentTail + 12) % bufferSize);

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
        const payloadOffset = bufferStart + ((currentTail + headerSize) % bufferSize);

        // Call message handler with offset/length (caller reads directly from uint8View)
        onMessage(payloadOffset, payloadLength, sequence, sourceId);

        // Advance tail by message length (which may be aligned by writer)
        currentTail = (currentTail + length) % bufferSize;
        messagesRead++;
    }

    return { newTail: currentTail, messagesRead };
}

// =========================================================================
// Locked ring buffer write (SAB mode)
// =========================================================================

/**
 * Try to acquire the write lock.
 * @param {Int32Array} atomicView
 * @param {number} lockIndex
 * @param {number} maxSpins - Maximum spin attempts (0 = try once)
 * @param {boolean} useWait - If true, use Atomics.wait() for guaranteed acquisition (workers only)
 * @returns {boolean} true if lock acquired
 */
function tryAcquireLock(atomicView, lockIndex, maxSpins = 0, useWait = false) {
    for (let i = 0; i <= maxSpins; i++) {
        const oldValue = Atomics.compareExchange(atomicView, lockIndex, 0, 1);
        if (oldValue === 0) {
            return true;
        }
    }

    if (useWait) {
        const MAX_WAIT_ATTEMPTS = 100;
        for (let attempt = 0; attempt < MAX_WAIT_ATTEMPTS; attempt++) {
            Atomics.wait(atomicView, lockIndex, 1, 100);
            const oldValue = Atomics.compareExchange(atomicView, lockIndex, 0, 1);
            if (oldValue === 0) {
                return true;
            }
        }
        console.error('[RingBuffer] Lock acquisition timeout after 10s - possible deadlock');
        return false;
    }

    return false;
}

function releaseLock(atomicView, lockIndex) {
    Atomics.store(atomicView, lockIndex, 0);
    Atomics.notify(atomicView, lockIndex, 1);
}

/**
 * Write an OSC message to the IN ring buffer with lock acquisition.
 * @param {Object} params
 * @param {Int32Array} params.atomicView
 * @param {DataView} params.dataView
 * @param {Uint8Array} params.uint8View
 * @param {Object} params.bufferConstants
 * @param {number} params.ringBufferBase
 * @param {Object} params.controlIndices
 * @param {Uint8Array} params.oscMessage
 * @param {number} [params.sourceId=0]
 * @param {number} [params.maxSpins=0]
 * @param {boolean} [params.useWait=false]
 * @returns {boolean} true if write succeeded
 */
export function writeToRingBuffer({
    atomicView, dataView, uint8View,
    bufferConstants, ringBufferBase, controlIndices,
    oscMessage, sourceId = 0, maxSpins = 0, useWait = false,
    headerScratch = null, headerScratchView = null
}) {
    const payloadSize = oscMessage.length;
    const totalSize = bufferConstants.MESSAGE_HEADER_SIZE + payloadSize;

    if (totalSize > bufferConstants.IN_BUFFER_SIZE - bufferConstants.MESSAGE_HEADER_SIZE) {
        return false;
    }

    if (!tryAcquireLock(atomicView, controlIndices.IN_WRITE_LOCK, maxSpins, useWait)) {
        return false;
    }

    try {
        const head = Atomics.load(atomicView, controlIndices.IN_HEAD);
        const tail = Atomics.load(atomicView, controlIndices.IN_TAIL);
        const alignedSize = (totalSize + 3) & ~3;
        const available = calculateAvailableSpace(head, tail, bufferConstants.IN_BUFFER_SIZE);

        if (available < alignedSize) {
            return false;
        }

        const messageSeq = Atomics.add(atomicView, controlIndices.IN_SEQUENCE, 1);

        const newHead = writeMessageToBuffer({
            uint8View, dataView,
            bufferStart: ringBufferBase + bufferConstants.IN_BUFFER_START,
            bufferSize: bufferConstants.IN_BUFFER_SIZE,
            head, payload: oscMessage, sequence: messageSeq,
            messageMagic: bufferConstants.MESSAGE_MAGIC,
            headerSize: bufferConstants.MESSAGE_HEADER_SIZE,
            sourceId,
            headerScratch, headerScratchView
        });

        Atomics.load(atomicView, controlIndices.IN_HEAD);
        Atomics.store(atomicView, controlIndices.IN_HEAD, newHead);
        Atomics.notify(atomicView, controlIndices.IN_HEAD, 1);

        return true;
    } finally {
        releaseLock(atomicView, controlIndices.IN_WRITE_LOCK);
    }
}

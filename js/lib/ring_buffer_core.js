/*
    SuperSonic - Ring Buffer Core
    Shared module for ring buffer operations.
    Used by both SAB mode (ring_buffer_writer.js) and postMessage mode (audioworklet).

    This module contains pure data operations - no atomics, no locking.
    Callers are responsible for concurrency control.
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
    headerSize
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
        headerView.setUint32(12, 0, true);  // padding

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
        dataView.setUint32(writePos + 12, 0, true);  // padding

        // Write payload
        uint8View.set(payload, writePos + headerSize);
    }

    // Return new head position (aligned)
    return (head + alignedSize) % bufferSize;
}

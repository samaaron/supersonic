/*
    SuperSonic - Ring Buffer Writer
    Shared module for writing OSC messages to the IN ring buffer.
    Used by both main thread (supersonic.js) and prescheduler worker.

    Uses a spinlock to prevent race conditions between concurrent writers.
*/

/**
 * Try to acquire the write lock.
 * @param {Int32Array} atomicView - Int32Array view of SharedArrayBuffer
 * @param {number} lockIndex - Index of the lock in atomicView
 * @param {number} maxSpins - Maximum spin attempts (0 = try once)
 * @returns {boolean} true if lock acquired, false if failed
 */
function tryAcquireLock(atomicView, lockIndex, maxSpins = 0) {
    // Try to CAS 0 (unlocked) → 1 (locked)
    for (let i = 0; i <= maxSpins; i++) {
        const oldValue = Atomics.compareExchange(atomicView, lockIndex, 0, 1);
        if (oldValue === 0) {
            return true; // Lock acquired
        }
        // If spinning, brief pause (only matters for worker, main thread uses maxSpins=0)
    }
    return false; // Failed to acquire
}

/**
 * Release the write lock.
 * @param {Int32Array} atomicView - Int32Array view of SharedArrayBuffer
 * @param {number} lockIndex - Index of the lock in atomicView
 */
function releaseLock(atomicView, lockIndex) {
    Atomics.store(atomicView, lockIndex, 0);
}

/**
 * Write an OSC message to the IN ring buffer.
 * Handles wrap-around at buffer boundary.
 * Uses spinlock to prevent race conditions between writers.
 *
 * @param {Object} params - Write parameters
 * @param {Int32Array} params.atomicView - Int32Array view of SharedArrayBuffer
 * @param {DataView} params.dataView - DataView of SharedArrayBuffer
 * @param {Uint8Array} params.uint8View - Uint8Array view of SharedArrayBuffer
 * @param {Object} params.bufferConstants - Buffer layout constants
 * @param {number} params.ringBufferBase - Base offset of ring buffer in SAB
 * @param {Object} params.controlIndices - Control pointer indices (IN_HEAD, IN_TAIL, IN_SEQUENCE, IN_WRITE_LOCK)
 * @param {Uint8Array} params.oscMessage - OSC message data to write
 * @param {number} [params.maxSpins=0] - Max lock spin attempts (0 = try once, for main thread)
 * @returns {boolean} true if write succeeded, false if lock contention or buffer full
 */
export function writeToRingBuffer({
    atomicView,
    dataView,
    uint8View,
    bufferConstants,
    ringBufferBase,
    controlIndices,
    oscMessage,
    maxSpins = 0
}) {
    const payloadSize = oscMessage.length;
    const totalSize = bufferConstants.MESSAGE_HEADER_SIZE + payloadSize;

    // Check if message fits in buffer at all (before acquiring lock)
    if (totalSize > bufferConstants.IN_BUFFER_SIZE - bufferConstants.MESSAGE_HEADER_SIZE) {
        return false;
    }

    // Try to acquire write lock
    if (!tryAcquireLock(atomicView, controlIndices.IN_WRITE_LOCK, maxSpins)) {
        return false; // Lock contention - caller should fall back to worker
    }

    // === LOCK ACQUIRED ===
    // From here, we must release the lock before returning

    try {
        // Read current head and tail
        const head = Atomics.load(atomicView, controlIndices.IN_HEAD);
        const tail = Atomics.load(atomicView, controlIndices.IN_TAIL);

        // Calculate available space
        const available = (bufferConstants.IN_BUFFER_SIZE - 1 - head + tail) % bufferConstants.IN_BUFFER_SIZE;

        if (available < totalSize) {
            // Buffer full
            return false;
        }

        // Get next sequence number atomically
        const messageSeq = Atomics.add(atomicView, controlIndices.IN_SEQUENCE, 1);

        // Calculate space to end of buffer
        const spaceToEnd = bufferConstants.IN_BUFFER_SIZE - head;

        if (totalSize > spaceToEnd) {
            // Message will wrap - write in two parts
            const headerBytes = new Uint8Array(bufferConstants.MESSAGE_HEADER_SIZE);
            const headerView = new DataView(headerBytes.buffer);
            headerView.setUint32(0, bufferConstants.MESSAGE_MAGIC, true);
            headerView.setUint32(4, totalSize, true);
            headerView.setUint32(8, messageSeq, true);
            headerView.setUint32(12, 0, true);

            const writePos1 = ringBufferBase + bufferConstants.IN_BUFFER_START + head;
            const writePos2 = ringBufferBase + bufferConstants.IN_BUFFER_START;

            if (spaceToEnd >= bufferConstants.MESSAGE_HEADER_SIZE) {
                // Header fits contiguously
                uint8View.set(headerBytes, writePos1);

                // Write payload (split across boundary)
                const payloadBytesInFirstPart = spaceToEnd - bufferConstants.MESSAGE_HEADER_SIZE;
                uint8View.set(oscMessage.subarray(0, payloadBytesInFirstPart), writePos1 + bufferConstants.MESSAGE_HEADER_SIZE);
                uint8View.set(oscMessage.subarray(payloadBytesInFirstPart), writePos2);
            } else {
                // Header is split across boundary
                uint8View.set(headerBytes.subarray(0, spaceToEnd), writePos1);
                uint8View.set(headerBytes.subarray(spaceToEnd), writePos2);

                // All payload goes at beginning
                const payloadOffset = bufferConstants.MESSAGE_HEADER_SIZE - spaceToEnd;
                uint8View.set(oscMessage, writePos2 + payloadOffset);
            }
        } else {
            // Message fits contiguously - write normally
            const writePos = ringBufferBase + bufferConstants.IN_BUFFER_START + head;

            // Write header
            dataView.setUint32(writePos, bufferConstants.MESSAGE_MAGIC, true);
            dataView.setUint32(writePos + 4, totalSize, true);
            dataView.setUint32(writePos + 8, messageSeq, true);
            dataView.setUint32(writePos + 12, 0, true);

            // Write payload
            uint8View.set(oscMessage, writePos + bufferConstants.MESSAGE_HEADER_SIZE);
        }

        // CRITICAL: Ensure memory barrier before publishing head pointer
        Atomics.load(atomicView, controlIndices.IN_HEAD);

        // Update head pointer (publish message)
        const newHead = (head + totalSize) % bufferConstants.IN_BUFFER_SIZE;
        Atomics.store(atomicView, controlIndices.IN_HEAD, newHead);

        return true;
    } finally {
        // === RELEASE LOCK ===
        releaseLock(atomicView, controlIndices.IN_WRITE_LOCK);
    }
}

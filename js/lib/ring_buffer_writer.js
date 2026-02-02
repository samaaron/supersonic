// SPDX-License-Identifier: MIT OR GPL-3.0-or-later
// Copyright (c) 2025 Sam Aaron

import { writeMessageToBuffer, calculateAvailableSpace } from './ring_buffer_core.js';

/**
 * Try to acquire the write lock.
 * @param {Int32Array} atomicView - Int32Array view of SharedArrayBuffer
 * @param {number} lockIndex - Index of the lock in atomicView
 * @param {number} maxSpins - Maximum spin attempts (0 = try once)
 * @param {boolean} useWait - If true, use Atomics.wait() for guaranteed acquisition (workers only)
 * @returns {boolean} true if lock acquired, false if failed
 */
function tryAcquireLock(atomicView, lockIndex, maxSpins = 0, useWait = false) {
    // Try to CAS 0 (unlocked) → 1 (locked)
    for (let i = 0; i <= maxSpins; i++) {
        const oldValue = Atomics.compareExchange(atomicView, lockIndex, 0, 1);
        if (oldValue === 0) {
            return true; // Lock acquired
        }
        // If spinning, brief pause (only matters for worker, main thread uses maxSpins=0)
    }

    // If useWait is enabled (workers only), block until lock becomes available
    // Main thread CANNOT use Atomics.wait() - browser will throw
    if (useWait) {
        const MAX_WAIT_ATTEMPTS = 100; // 100 * 100ms = 10 seconds max
        for (let attempt = 0; attempt < MAX_WAIT_ATTEMPTS; attempt++) {
            // Wait for lock to become 0 (unlocked)
            // Timeout after 100ms to avoid deadlock, then retry
            Atomics.wait(atomicView, lockIndex, 1, 100);

            // Try to acquire after waking
            const oldValue = Atomics.compareExchange(atomicView, lockIndex, 0, 1);
            if (oldValue === 0) {
                return true; // Lock acquired
            }
            // Lock was taken by someone else, wait again
        }
        console.error('[RingBuffer] Lock acquisition timeout after 10s - possible deadlock');
        return false;
    }

    return false; // Failed to acquire (non-blocking mode)
}

/**
 * Release the write lock.
 * @param {Int32Array} atomicView - Int32Array view of SharedArrayBuffer
 * @param {number} lockIndex - Index of the lock in atomicView
 */
function releaseLock(atomicView, lockIndex) {
    Atomics.store(atomicView, lockIndex, 0);
    // Wake one waiting thread (if any are blocked on Atomics.wait)
    Atomics.notify(atomicView, lockIndex, 1);
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
 * @param {number} [params.sourceId=0] - Source ID for logging (0 = main, 1+ = workers)
 * @param {number} [params.maxSpins=0] - Max lock spin attempts (0 = try once, for main thread)
 * @param {boolean} [params.useWait=false] - Use Atomics.wait() for guaranteed lock acquisition (workers only)
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
    sourceId = 0,
    maxSpins = 0,
    useWait = false
}) {
    const payloadSize = oscMessage.length;
    const totalSize = bufferConstants.MESSAGE_HEADER_SIZE + payloadSize;

    // Check if message fits in buffer at all (before acquiring lock)
    if (totalSize > bufferConstants.IN_BUFFER_SIZE - bufferConstants.MESSAGE_HEADER_SIZE) {
        return false;
    }

    // Try to acquire write lock
    if (!tryAcquireLock(atomicView, controlIndices.IN_WRITE_LOCK, maxSpins, useWait)) {
        return false; // Lock contention - caller should fall back to worker
    }

    // === LOCK ACQUIRED ===
    // From here, we must release the lock before returning

    try {
        // Read current head and tail
        const head = Atomics.load(atomicView, controlIndices.IN_HEAD);
        const tail = Atomics.load(atomicView, controlIndices.IN_TAIL);

        // Calculate available space (aligned size will be computed by core)
        const alignedSize = (totalSize + 3) & ~3;
        const available = calculateAvailableSpace(head, tail, bufferConstants.IN_BUFFER_SIZE);

        if (available < alignedSize) {
            // Buffer full
            return false;
        }

        // Get next sequence number atomically
        const messageSeq = Atomics.add(atomicView, controlIndices.IN_SEQUENCE, 1);

        // Write message using shared core logic
        const newHead = writeMessageToBuffer({
            uint8View,
            dataView,
            bufferStart: ringBufferBase + bufferConstants.IN_BUFFER_START,
            bufferSize: bufferConstants.IN_BUFFER_SIZE,
            head,
            payload: oscMessage,
            sequence: messageSeq,
            messageMagic: bufferConstants.MESSAGE_MAGIC,
            headerSize: bufferConstants.MESSAGE_HEADER_SIZE,
            sourceId
        });

        // CRITICAL: Ensure memory barrier before publishing head pointer
        Atomics.load(atomicView, controlIndices.IN_HEAD);

        // Update head pointer (publish message)
        Atomics.store(atomicView, controlIndices.IN_HEAD, newHead);

        // Notify waiting log worker that new data is available
        Atomics.notify(atomicView, controlIndices.IN_HEAD, 1);

        return true;
    } finally {
        // === RELEASE LOCK ===
        releaseLock(atomicView, controlIndices.IN_WRITE_LOCK);
    }
}

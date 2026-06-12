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
 * Calculate available space in a circular buffer (total free bytes).
 * NOTE: frames never wrap, so total free space alone does not mean a frame
 * fits — use canWriteMessage() for the authoritative fit test.
 * @param {number} head - Current head position
 * @param {number} tail - Current tail position
 * @param {number} bufferSize - Total buffer size
 * @returns {number} bytes available for writing
 */
export function calculateAvailableSpace(head, tail, bufferSize) {
    return (bufferSize - 1 - head + tail) % bufferSize;
}

/**
 * Authoritative fit test for the never-wrap convention: a frame needs
 * alignedSize contiguous bytes before the end of the ring, or (after a
 * padding marker) before the tail at offset 0.
 * @param {number} head - Current head position
 * @param {number} tail - Current tail position
 * @param {number} bufferSize - Total buffer size
 * @param {number} alignedSize - 4-byte-aligned frame size (header + payload)
 * @returns {boolean} true if writeMessageToBuffer may be called
 */
export function canWriteMessage(head, tail, bufferSize, alignedSize) {
    if (alignedSize > calculateAvailableSpace(head, tail, bufferSize)) return false;
    const spaceToEnd = bufferSize - head;
    if (alignedSize <= spaceToEnd) return true;
    // Restarting at offset 0 needs contiguous room before the tail
    // (tail - 1 to preserve the head !== tail empty/full distinction).
    return alignedSize <= (tail > 0 ? tail - 1 : 0);
}

/**
 * Write a message to the ring buffer. Frames NEVER wrap: when the frame
 * doesn't fit before the end of the ring, a padding marker (paddingMagic +
 * zeros to the boundary) is written and the frame restarts at offset 0, so
 * every frame is contiguous and readers parse in place. Mirrors the C++
 * writer (RingBufferWriter.h) byte-for-byte — the two are held identical by
 * the ring-wire conformance fixtures.
 *
 * Pure data operation - no atomics, no locking. The caller must have
 * verified fit with canWriteMessage().
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
 * @param {number} params.paddingMagic - Magic number for the boundary padding marker (e.g., 0xDEADFEED)
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
    paddingMagic,
    headerSize,
    sourceId = 0
}) {
    const payloadSize = payload.length;
    const totalSize = headerSize + payloadSize;
    // Align to 4 bytes
    const alignedSize = (totalSize + 3) & ~3;

    // Frames never wrap: pad to the boundary and restart at offset 0.
    const spaceToEnd = bufferSize - head;
    if (alignedSize > spaceToEnd) {
        // Padding marker: magic word, zeros to the end of the ring. 4-byte
        // alignment guarantees at least the magic always fits. fill() is
        // memset-backed — the pad run can be large and executes under the
        // caller's write lock.
        dataView.setUint32(bufferStart + head, paddingMagic, true);
        uint8View.fill(0, bufferStart + head + 4, bufferStart + bufferSize);
        head = 0;
    }

    const writePos = bufferStart + head;

    // Header: length is the EXACT frame size (header + payload bytes);
    // readers advance by its 4-byte-aligned footprint. Payload sizes
    // round-trip exactly while offsets stay 4-aligned.
    dataView.setUint32(writePos, messageMagic, true);
    dataView.setUint32(writePos + 4, totalSize, true);
    dataView.setUint32(writePos + 8, sequence, true);
    dataView.setUint32(writePos + 12, sourceId, true);

    // Payload, then zero the 0-3 alignment pad bytes (determinism — the
    // conformance fixtures compare whole ring images).
    uint8View.set(payload, writePos + headerSize);
    if (alignedSize > totalSize) {
        uint8View.fill(0, writePos + totalSize, writePos + alignedSize);
    }

    return (head + alignedSize) % bufferSize;
}

/**
 * Read messages from ring buffer, following padding markers at the boundary.
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
 *   payloadOffset is absolute byte offset into uint8View where payload starts.
 *   Frames never wrap (every writer pads at the boundary and restarts at
 *   offset 0), so a direct uint8View[payloadOffset + i] read is safe.
 * @param {Function} [params.onCorruption] - Optional callback invoked once when
 *   corruption is detected: (position: number) => void. The walk then resyncs
 *   the tail to head (dropping pending frames) and stops — same policy as the
 *   C++ walker.
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

    // Corruption policy (mirrors ss_drain_ring, src/lanes/ring_drain.h):
    // anything malformed below head means the rest of the region is suspect —
    // resync the tail to head, dropping pending frames, and stop. A byte-wise
    // rescan would be unbounded work (this walk runs inside the AudioWorklet's
    // process() in postMessage mode) and can fabricate frames from stale
    // payload bytes.
    const corrupt = () => {
        if (onCorruption) onCorruption(currentTail);
        currentTail = head;
    };

    while (currentTail !== head && messagesRead < maxMessages) {
        const bytesToEnd = bufferSize - currentTail;
        const avail = (head - currentTail + bufferSize) % bufferSize;

        // Frame offsets are 4-aligned by construction; a tail closer to the
        // boundary than a magic word is cursor damage.
        if (bytesToEnd < 4 || avail < 4) {
            corrupt();
            break;
        }

        const magic = dataView.getUint32(bufferStart + currentTail, true);

        if (magic === paddingMagic) {
            // Writer hit end-of-ring and restarted at offset 0. A padding
            // marker AT offset 0 can never be legitimately written (the whole
            // ring lies ahead of a writer at 0) — treat it as corruption
            // rather than spinning on it.
            if (currentTail === 0) {
                corrupt();
                break;
            }
            currentTail = 0;
            continue;
        }
        if (magic !== messageMagic) {
            corrupt();
            break;
        }
        if (bytesToEnd < headerSize) {
            // A real header can't sit closer to the boundary than its own
            // size under the never-wrap convention.
            corrupt();
            break;
        }

        const length = dataView.getUint32(bufferStart + currentTail + 4, true);
        const sequence = dataView.getUint32(bufferStart + currentTail + 8, true);
        const sourceId = dataView.getUint32(bufferStart + currentTail + 12, true);

        // Length sanity, including the never-wrap invariant: the frame's
        // 4-aligned footprint must lie entirely before the ring boundary and
        // within the published region — consumers read payloads linearly, so
        // a frame crossing the boundary would walk past the ring's end.
        const footprint = (length + 3) & ~3;
        if (length < headerSize || footprint > bufferSize ||
            footprint > bytesToEnd || footprint > avail) {
            corrupt();
            break;
        }

        // Payload location (frames are contiguous; no allocation, just arithmetic)
        const payloadLength = length - headerSize;
        const payloadOffset = bufferStart + currentTail + headerSize;

        // Call message handler with offset/length (caller reads directly from uint8View)
        onMessage(payloadOffset, payloadLength, sequence, sourceId);

        // Advance tail by the frame's 4-aligned footprint (header length is
        // the exact byte count; the writer rounds occupancy up to 4)
        currentTail = (currentTail + footprint) % bufferSize;
        messagesRead++;
    }

    return { newTail: currentTail, messagesRead };
}

/**
 * Copy a message payload out of the ring into `dest`. Frames never wrap, so
 * this is a plain linear copy; the index re-wrapping is purely a defensive
 * bound against malformed offsets from a corrupt ring. No allocation —
 * caller provides `dest`.
 *
 * @param {Uint8Array} uint8View - byte view spanning the ring region
 * @param {number} bufferStart - absolute offset of the ring's first byte
 * @param {number} bufferSize - ring length in bytes
 * @param {number} payloadOffset - absolute offset of the payload's first byte
 * @param {number} length - number of payload bytes to copy
 * @param {Uint8Array} dest - destination buffer
 * @param {number} [destOffset=0] - start index in dest
 */
export function copyWrappedPayload(uint8View, bufferStart, bufferSize, payloadOffset, length, dest, destOffset = 0) {
    const rel = payloadOffset - bufferStart;
    for (let i = 0; i < length; i++) {
        dest[destOffset + i] = uint8View[bufferStart + ((rel + i) % bufferSize)];
    }
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
 * @param {Object}     ring - Cached IN-ring handle (built once per channel):
 *   { atomicView, dataView, uint8View, bufferConstants, ringBufferBase,
 *     controlIndices, headerScratch, headerScratchView }.
 * @param {Uint8Array} oscMessage - OSC bytes to frame.
 * @param {number}     [sourceId=0] - Writer identity stamped into the header.
 * @param {boolean}    [blocking=false] - Worker (true): spin then Atomics.wait
 *   for guaranteed delivery. Main thread (false): cannot wait — spin harder,
 *   then return false (backpressure) rather than block.
 * @returns {boolean} true if write succeeded
 */
export function writeToRingBuffer(ring, oscMessage, sourceId = 0, blocking = false) {
    const {
        atomicView, dataView, uint8View,
        bufferConstants, ringBufferBase, controlIndices,
    } = ring;
    // Lock-acquisition policy is the writer's concern, not the caller's.
    const maxSpins = blocking ? 10 : 64;
    const useWait  = blocking;

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

        if (!canWriteMessage(head, tail, bufferConstants.IN_BUFFER_SIZE, alignedSize)) {
            return false;
        }

        const messageSeq = Atomics.add(atomicView, controlIndices.IN_SEQUENCE, 1);

        const newHead = writeMessageToBuffer({
            uint8View, dataView,
            bufferStart: ringBufferBase + bufferConstants.IN_BUFFER_START,
            bufferSize: bufferConstants.IN_BUFFER_SIZE,
            head, payload: oscMessage, sequence: messageSeq,
            messageMagic: bufferConstants.MESSAGE_MAGIC,
            paddingMagic: bufferConstants.PADDING_MAGIC,
            headerSize: bufferConstants.MESSAGE_HEADER_SIZE,
            sourceId
        });

        Atomics.store(atomicView, controlIndices.IN_HEAD, newHead);
        Atomics.notify(atomicView, controlIndices.IN_HEAD, 1);

        return true;
    } finally {
        releaseLock(atomicView, controlIndices.IN_WRITE_LOCK);
    }
}

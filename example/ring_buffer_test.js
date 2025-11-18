/**
 * Ring Buffer Test Suite
 * Tests the SharedArrayBuffer ring buffer used for OSC communication
 * between JavaScript and C++ (WASM)
 */

// Mock buffer constants (matching shared_memory.h)
const BUFFER_CONSTANTS = {
    IN_BUFFER_START: 0,
    IN_BUFFER_SIZE: 9728,
    MESSAGE_HEADER_SIZE: 16,
    MESSAGE_MAGIC: 0xDEADBEEF,
    MAX_MESSAGE_SIZE: 9728 - 16
};

const CONTROL_INDICES = {
    IN_HEAD: 0,
    IN_TAIL: 1
};

/**
 * Ring Buffer Writer (simulates osc_out_prescheduler_worker.js)
 */
class RingBufferWriter {
    constructor(sharedBuffer, ringBufferBase = 64) {
        this.sharedBuffer = sharedBuffer;
        this.atomicView = new Int32Array(sharedBuffer);
        this.uint8View = new Uint8Array(sharedBuffer);
        this.dataView = new DataView(sharedBuffer);
        this.ringBufferBase = ringBufferBase;
        this.sequenceNum = 0;
    }

    write(payload) {
        const payloadSize = payload.length;
        const totalSize = BUFFER_CONSTANTS.MESSAGE_HEADER_SIZE + payloadSize;

        // Check if message fits at all
        if (totalSize > BUFFER_CONSTANTS.IN_BUFFER_SIZE - BUFFER_CONSTANTS.MESSAGE_HEADER_SIZE) {
            return { success: false, error: 'MESSAGE_TOO_LARGE' };
        }

        // Load head and tail
        const head = Atomics.load(this.atomicView, CONTROL_INDICES.IN_HEAD);
        const tail = Atomics.load(this.atomicView, CONTROL_INDICES.IN_TAIL);

        // Calculate available space
        const available = (BUFFER_CONSTANTS.IN_BUFFER_SIZE - 1 - head + tail) % BUFFER_CONSTANTS.IN_BUFFER_SIZE;

        if (available < totalSize) {
            return { success: false, error: 'BUFFER_FULL', available, needed: totalSize };
        }

        // Handle wrapping with split writes (no padding markers)
        const spaceToEnd = BUFFER_CONSTANTS.IN_BUFFER_SIZE - head;

        if (totalSize > spaceToEnd) {
            // Message will wrap - write in two parts
            console.log(`[Writer] Split write: head=${head} spaceToEnd=${spaceToEnd} totalSize=${totalSize}`);

            // Create header as a byte array
            const headerBytes = new Uint8Array(BUFFER_CONSTANTS.MESSAGE_HEADER_SIZE);
            const headerView = new DataView(headerBytes.buffer);
            headerView.setUint32(0, BUFFER_CONSTANTS.MESSAGE_MAGIC, true);
            headerView.setUint32(4, totalSize, true);
            headerView.setUint32(8, this.sequenceNum, true);
            headerView.setUint32(12, 0, true);

            const writePos1 = this.ringBufferBase + BUFFER_CONSTANTS.IN_BUFFER_START + head;
            const writePos2 = this.ringBufferBase + BUFFER_CONSTANTS.IN_BUFFER_START;

            // Write header (may be split)
            if (spaceToEnd >= BUFFER_CONSTANTS.MESSAGE_HEADER_SIZE) {
                // Header fits contiguously
                this.uint8View.set(headerBytes, writePos1);

                // Write payload (may be split)
                const payloadBytesInFirstPart = spaceToEnd - BUFFER_CONSTANTS.MESSAGE_HEADER_SIZE;
                this.uint8View.set(payload.subarray(0, payloadBytesInFirstPart), writePos1 + BUFFER_CONSTANTS.MESSAGE_HEADER_SIZE);
                this.uint8View.set(payload.subarray(payloadBytesInFirstPart), writePos2);
            } else {
                // Header is split
                this.uint8View.set(headerBytes.subarray(0, spaceToEnd), writePos1);
                this.uint8View.set(headerBytes.subarray(spaceToEnd), writePos2);

                // All payload goes at beginning
                const payloadOffset = BUFFER_CONSTANTS.MESSAGE_HEADER_SIZE - spaceToEnd;
                this.uint8View.set(payload, writePos2 + payloadOffset);
            }
        } else {
            // Message fits contiguously - write normally
            const writePos = this.ringBufferBase + BUFFER_CONSTANTS.IN_BUFFER_START + head;

            // Write header
            this.dataView.setUint32(writePos, BUFFER_CONSTANTS.MESSAGE_MAGIC, true);
            this.dataView.setUint32(writePos + 4, totalSize, true);
            this.dataView.setUint32(writePos + 8, this.sequenceNum, true);
            this.dataView.setUint32(writePos + 12, 0, true);

            // Write payload
            this.uint8View.set(payload, writePos + BUFFER_CONSTANTS.MESSAGE_HEADER_SIZE);
        }

        // Memory barrier
        Atomics.load(this.atomicView, CONTROL_INDICES.IN_HEAD);

        // Update head pointer
        const newHead = (head + totalSize) % BUFFER_CONSTANTS.IN_BUFFER_SIZE;
        Atomics.store(this.atomicView, CONTROL_INDICES.IN_HEAD, newHead);

        this.sequenceNum++;
        return { success: true, sequence: this.sequenceNum - 1, pos: head, size: totalSize };
    }
}

/**
 * Ring Buffer Reader (simulates audio_processor.cpp)
 */
class RingBufferReader {
    constructor(sharedBuffer, ringBufferBase = 64) {
        this.sharedBuffer = sharedBuffer;
        this.atomicView = new Int32Array(sharedBuffer);
        this.uint8View = new Uint8Array(sharedBuffer);
        this.dataView = new DataView(sharedBuffer);
        this.ringBufferBase = ringBufferBase;
        this.messagesRead = 0;
        this.errors = [];
    }

    readAll() {
        const messages = [];
        let head = Atomics.load(this.atomicView, CONTROL_INDICES.IN_HEAD);
        let tail = Atomics.load(this.atomicView, CONTROL_INDICES.IN_TAIL);

        while (head !== tail) {
            const msgOffset = this.ringBufferBase + BUFFER_CONSTANTS.IN_BUFFER_START + tail;
            const spaceToEnd = BUFFER_CONSTANTS.IN_BUFFER_SIZE - tail;

            // Handle split reads for header
            let magic, length, sequence;

            if (spaceToEnd >= BUFFER_CONSTANTS.MESSAGE_HEADER_SIZE) {
                // Header fits contiguously
                magic = this.dataView.getUint32(msgOffset, true);
                length = this.dataView.getUint32(msgOffset + 4, true);
                sequence = this.dataView.getUint32(msgOffset + 8, true);
            } else {
                // Header is split - read in two parts
                const headerBytes = new Uint8Array(BUFFER_CONSTANTS.MESSAGE_HEADER_SIZE);

                // Read first part
                for (let i = 0; i < spaceToEnd; i++) {
                    headerBytes[i] = this.uint8View[msgOffset + i];
                }

                // Read second part from beginning of buffer
                const secondPartStart = this.ringBufferBase + BUFFER_CONSTANTS.IN_BUFFER_START;
                const secondPartSize = BUFFER_CONSTANTS.MESSAGE_HEADER_SIZE - spaceToEnd;
                for (let i = 0; i < secondPartSize; i++) {
                    headerBytes[spaceToEnd + i] = this.uint8View[secondPartStart + i];
                }

                // Parse header from assembled bytes
                const headerView = new DataView(headerBytes.buffer);
                magic = headerView.getUint32(0, true);
                length = headerView.getUint32(4, true);
                sequence = headerView.getUint32(8, true);
            }

            if (this.errors.length === 0) {
                console.log(`[Reader] Reading from tail=${tail}: magic=0x${magic.toString(16)} (expected 0x${BUFFER_CONSTANTS.MESSAGE_MAGIC.toString(16)})`);
            }

            // Validate magic
            if (magic !== BUFFER_CONSTANTS.MESSAGE_MAGIC) {
                this.errors.push({
                    type: 'INVALID_MAGIC',
                    tail,
                    head,
                    magic: magic.toString(16),
                    length,
                    sequence
                });
                // Skip 1 byte and continue (mimics C++ behavior)
                Atomics.store(this.atomicView, CONTROL_INDICES.IN_TAIL, (tail + 1) % BUFFER_CONSTANTS.IN_BUFFER_SIZE);
                tail = Atomics.load(this.atomicView, CONTROL_INDICES.IN_TAIL);
                continue;
            }

            // Validate length
            if (length > BUFFER_CONSTANTS.MAX_MESSAGE_SIZE + BUFFER_CONSTANTS.MESSAGE_HEADER_SIZE) {
                this.errors.push({
                    type: 'INVALID_LENGTH',
                    tail,
                    head,
                    length
                });
                Atomics.store(this.atomicView, CONTROL_INDICES.IN_TAIL, (tail + length) % BUFFER_CONSTANTS.IN_BUFFER_SIZE);
                tail = Atomics.load(this.atomicView, CONTROL_INDICES.IN_TAIL);
                continue;
            }

            // Read payload (may be split across wrap boundary)
            const payloadSize = length - BUFFER_CONSTANTS.MESSAGE_HEADER_SIZE;
            const payload = new Uint8Array(payloadSize);

            const payloadStart = (tail + BUFFER_CONSTANTS.MESSAGE_HEADER_SIZE) % BUFFER_CONSTANTS.IN_BUFFER_SIZE;
            const payloadOffset = this.ringBufferBase + BUFFER_CONSTANTS.IN_BUFFER_START + payloadStart;
            const bytesToEnd = BUFFER_CONSTANTS.IN_BUFFER_SIZE - payloadStart;

            if (payloadSize <= bytesToEnd) {
                // Payload fits contiguously
                for (let i = 0; i < payloadSize; i++) {
                    payload[i] = this.uint8View[payloadOffset + i];
                }
            } else {
                // Payload is split - read in two parts
                for (let i = 0; i < bytesToEnd; i++) {
                    payload[i] = this.uint8View[payloadOffset + i];
                }

                const secondPartStart = this.ringBufferBase + BUFFER_CONSTANTS.IN_BUFFER_START;
                const secondPartSize = payloadSize - bytesToEnd;
                for (let i = 0; i < secondPartSize; i++) {
                    payload[bytesToEnd + i] = this.uint8View[secondPartStart + i];
                }
            }

            messages.push({
                sequence,
                payload,
                position: tail,
                length
            });

            // Update tail
            Atomics.store(this.atomicView, CONTROL_INDICES.IN_TAIL, (tail + length) % BUFFER_CONSTANTS.IN_BUFFER_SIZE);
            tail = Atomics.load(this.atomicView, CONTROL_INDICES.IN_TAIL);
            this.messagesRead++;
        }

        return messages;
    }
}

/**
 * Test utilities
 */
function createTestBuffer() {
    // Create SharedArrayBuffer with control area + ring buffer
    const bufferSize = 64 + BUFFER_CONSTANTS.IN_BUFFER_SIZE;
    const sharedBuffer = new SharedArrayBuffer(bufferSize);
    const atomicView = new Int32Array(sharedBuffer);

    // Initialise control pointers
    Atomics.store(atomicView, CONTROL_INDICES.IN_HEAD, 0);
    Atomics.store(atomicView, CONTROL_INDICES.IN_TAIL, 0);

    return sharedBuffer;
}

function createPayload(size, fillByte = 0xAA) {
    const payload = new Uint8Array(size);
    payload.fill(fillByte);
    return payload;
}

/**
 * Test Cases
 */
function runTests(options = {}) {
    const { iterations = 1, stopOnError = false } = options;

    console.log('=== Ring Buffer Test Suite ===\n');
    if (iterations > 1) {
        console.log(`Running ${iterations} iterations to catch sporadic bugs\n`);
    }

    const tests = [
        testBasicWriteRead,
        testMultipleMessages,
        testWrapping,
        testBufferFull,
        testLargeMessage,
        testSequentialFill,
        testConcurrentWriteRead,
        testEdgeCaseMessageSizes,
        testBootSimulation,
        testStressTest
    ];

    let totalPassed = 0;
    let totalFailed = 0;

    for (let iter = 0; iter < iterations; iter++) {
        if (iterations > 1) {
            console.log(`\n--- Iteration ${iter + 1}/${iterations} ---\n`);
        }

        for (const test of tests) {
            try {
                console.log(`Running: ${test.name}`);
                test();
                console.log(`✓ PASSED\n`);
                totalPassed++;
            } catch (error) {
                console.log(`✗ FAILED: ${error.message}\n`);
                console.error(error.stack);
                totalFailed++;
                if (stopOnError) {
                    console.log('\n=== Stopping on first error ===');
                    return false;
                }
            }
        }
    }

    console.log(`\n=== Results: ${totalPassed} passed, ${totalFailed} failed ===`);
    return totalFailed === 0;
}

function testBasicWriteRead() {
    const buffer = createTestBuffer();
    const writer = new RingBufferWriter(buffer);
    const reader = new RingBufferReader(buffer);

    const payload = createPayload(100);
    const writeResult = writer.write(payload);

    if (!writeResult.success) {
        throw new Error('Write failed: ' + writeResult.error);
    }

    const messages = reader.readAll();

    if (messages.length !== 1) {
        throw new Error(`Expected 1 message, got ${messages.length}`);
    }

    if (messages[0].sequence !== 0) {
        throw new Error(`Expected sequence 0, got ${messages[0].sequence}`);
    }

    if (!arraysEqual(messages[0].payload, payload)) {
        throw new Error('Payload mismatch');
    }
}

function testMultipleMessages() {
    const buffer = createTestBuffer();
    const writer = new RingBufferWriter(buffer);
    const reader = new RingBufferReader(buffer);

    const payloads = [
        createPayload(50, 0x11),
        createPayload(100, 0x22),
        createPayload(200, 0x33),
    ];

    for (const payload of payloads) {
        const result = writer.write(payload);
        if (!result.success) {
            throw new Error('Write failed');
        }
    }

    const messages = reader.readAll();

    if (messages.length !== payloads.length) {
        throw new Error(`Expected ${payloads.length} messages, got ${messages.length}`);
    }

    for (let i = 0; i < payloads.length; i++) {
        if (!arraysEqual(messages[i].payload, payloads[i])) {
            throw new Error(`Payload mismatch at index ${i}`);
        }
    }
}

function testWrapping() {
    const buffer = createTestBuffer();
    const writer = new RingBufferWriter(buffer);
    const reader = new RingBufferReader(buffer);

    // Fill buffer almost to the end - leave < 116 bytes so next write must wrap
    const largePayload = createPayload(9600, 0xAA);  // 9600 + 16 header = 9616
    writer.write(largePayload);
    reader.readAll(); // Consume

    // Write message that MUST wrap (needs 116 bytes, only 112 available)
    const smallPayload = createPayload(100, 0xBB);  // 100 + 16 header = 116
    const result = writer.write(smallPayload);

    if (!result.success) {
        throw new Error('Wrap write failed: ' + result.error);
    }

    // With split writes, the message starts at the original head position
    // and wraps across the boundary - that's expected behavior
    const expectedPos = 9616; // Where head was before the write
    if (result.pos !== expectedPos) {
        throw new Error(`Expected message to start at position ${expectedPos}, got ${result.pos}`);
    }

    const messages = reader.readAll();

    if (messages.length !== 1) {
        throw new Error(`Expected 1 message after wrap, got ${messages.length}`);
    }

    if (!arraysEqual(messages[0].payload, smallPayload)) {
        throw new Error('Wrapped payload mismatch');
    }

    if (reader.errors.length > 0) {
        throw new Error(`Reader errors: ${JSON.stringify(reader.errors)}`);
    }
}

function testBufferFull() {
    const buffer = createTestBuffer();
    const writer = new RingBufferWriter(buffer);

    // Fill buffer completely without reading
    let written = 0;
    const payloadSize = 100;

    while (true) {
        const result = writer.write(createPayload(payloadSize));
        if (!result.success) {
            if (result.error === 'BUFFER_FULL') {
                break;
            }
            throw new Error(`Unexpected error: ${result.error}`);
        }
        written++;
    }

    if (written === 0) {
        throw new Error('Should have written at least one message');
    }

    console.log(`  Wrote ${written} messages before buffer full`);
}

function testLargeMessage() {
    const buffer = createTestBuffer();
    const writer = new RingBufferWriter(buffer);
    const reader = new RingBufferReader(buffer);

    const largePayload = createPayload(BUFFER_CONSTANTS.MAX_MESSAGE_SIZE - 100);
    const result = writer.write(largePayload);

    if (!result.success) {
        throw new Error('Large message write failed');
    }

    const messages = reader.readAll();

    if (!arraysEqual(messages[0].payload, largePayload)) {
        throw new Error('Large payload mismatch');
    }
}

function testSequentialFill() {
    const buffer = createTestBuffer();
    const writer = new RingBufferWriter(buffer);
    const reader = new RingBufferReader(buffer);

    const numMessages = 100;
    const payloadSize = 50;

    // Write and read in batches
    for (let batch = 0; batch < 10; batch++) {
        // Write 10 messages
        for (let i = 0; i < 10; i++) {
            const payload = createPayload(payloadSize, batch * 10 + i);
            const result = writer.write(payload);
            if (!result.success) {
                throw new Error(`Write failed at batch ${batch}, message ${i}`);
            }
        }

        // Read all
        const messages = reader.readAll();
        if (messages.length !== 10) {
            throw new Error(`Expected 10 messages in batch ${batch}, got ${messages.length}`);
        }

        if (reader.errors.length > 0) {
            throw new Error(`Reader errors in batch ${batch}: ${JSON.stringify(reader.errors)}`);
        }
    }

    console.log(`  Processed ${numMessages} messages in batches`);
}

function testConcurrentWriteRead() {
    const buffer = createTestBuffer();
    const writer = new RingBufferWriter(buffer);
    const reader = new RingBufferReader(buffer);

    const totalMessages = 100;
    let written = 0;
    let read = 0;

    // Simulate concurrent write/read
    for (let i = 0; i < totalMessages; i++) {
        // Write 1-3 messages
        const writeBatch = Math.min(1 + Math.floor(Math.random() * 3), totalMessages - written);
        for (let j = 0; j < writeBatch; j++) {
            const payload = createPayload(50 + Math.floor(Math.random() * 100), written);
            const result = writer.write(payload);
            if (result.success) {
                written++;
            }
        }

        // Read some messages
        const messages = reader.readAll();
        read += messages.length;

        if (reader.errors.length > 0) {
            throw new Error(`Corruption detected: ${JSON.stringify(reader.errors)}`);
        }
    }

    // Read remaining
    const remaining = reader.readAll();
    read += remaining.length;

    if (read !== written) {
        throw new Error(`Message count mismatch: wrote ${written}, read ${read}`);
    }

    console.log(`  Wrote and read ${written} messages concurrently`);
}

function testEdgeCaseMessageSizes() {
    const buffer = createTestBuffer();
    const writer = new RingBufferWriter(buffer);
    const reader = new RingBufferReader(buffer);

    // Reset buffer state between size tests
    Atomics.store(reader.atomicView, CONTROL_INDICES.IN_HEAD, 0);
    Atomics.store(reader.atomicView, CONTROL_INDICES.IN_TAIL, 0);

    const sizes = [
        1,   // Minimum
        15,  // Just under header size
        16,  // Exactly header size
        17,  // Just over header size
        100, // Normal
        1000, // Large
        BUFFER_CONSTANTS.MAX_MESSAGE_SIZE - 100, // Large but safe
        BUFFER_CONSTANTS.MAX_MESSAGE_SIZE - 17   // Almost max (leave 1 byte for ring buffer logic)
    ];

    for (const size of sizes) {
        const payload = createPayload(size, size & 0xFF);
        let result = writer.write(payload);

        if (!result.success) {
            throw new Error(`Failed to write size ${size}: ${result.error} ${result.reason || ''}`);
        }

        // Check for errors BEFORE reading (from previous iteration)
        if (reader.errors.length > 0) {
            throw new Error(`Size ${size}: reader had errors from previous iteration: ${JSON.stringify(reader.errors.slice(0, 3))}`);
        }

        const messages = reader.readAll();

        // Check for errors AFTER reading (from this iteration)
        if (reader.errors.length > 0) {
            const head = Atomics.load(reader.atomicView, CONTROL_INDICES.IN_HEAD);
            const tail = Atomics.load(reader.atomicView, CONTROL_INDICES.IN_TAIL);
            throw new Error(`Size ${size}: reader errors during read. Head=${head} Tail=${tail} WriteResult=${JSON.stringify(result)} Errors=${JSON.stringify(reader.errors.slice(0, 5))}`);
        }

        if (messages.length !== 1) {
            const head = Atomics.load(reader.atomicView, CONTROL_INDICES.IN_HEAD);
            const tail = Atomics.load(reader.atomicView, CONTROL_INDICES.IN_TAIL);
            throw new Error(`Size ${size}: expected 1 message, got ${messages.length}. Head=${head} Tail=${tail} WriteResult=${JSON.stringify(result)}`);
        }

        if (messages[0].payload.length !== size) {
            throw new Error(`Size ${size}: length mismatch ${messages[0].payload.length}`);
        }
    }

    console.log(`  Tested ${sizes.length} edge case sizes`);
}

function testBootSimulation() {
    const buffer = createTestBuffer();
    const writer = new RingBufferWriter(buffer);
    const reader = new RingBufferReader(buffer);

    // Simulate boot scenario: burst of messages without reading
    // Mimics loading synthdefs, samples, creating nodes, etc.

    const bootMessages = [];

    // Load 20 "synthdefs" (varying sizes)
    for (let i = 0; i < 20; i++) {
        bootMessages.push(createPayload(100 + Math.floor(Math.random() * 200), 0x10 + i));
    }

    // Load 30 "samples" (larger messages)
    for (let i = 0; i < 30; i++) {
        bootMessages.push(createPayload(200 + Math.floor(Math.random() * 300), 0x30 + i));
    }

    // Create 50 "nodes" (small messages)
    for (let i = 0; i < 50; i++) {
        bootMessages.push(createPayload(50 + Math.floor(Math.random() * 100), 0x50 + i));
    }

    // Write all boot messages rapidly
    let written = 0;
    for (const payload of bootMessages) {
        const result = writer.write(payload);
        if (result.success) {
            written++;
        } else if (result.error === 'BUFFER_FULL') {
            // Buffer full - read some and continue
            const consumed = reader.readAll();
            if (reader.errors.length > 0) {
                throw new Error(`Boot corruption after ${written} messages: ${JSON.stringify(reader.errors)}`);
            }
            // Retry this message
            const retryResult = writer.write(payload);
            if (retryResult.success) {
                written++;
            }
        } else {
            throw new Error(`Boot write failed: ${result.error}`);
        }
    }

    // Read all remaining
    const remaining = reader.readAll();

    if (reader.errors.length > 0) {
        throw new Error(`Boot final read corruption: ${JSON.stringify(reader.errors)}`);
    }

    console.log(`  Boot simulation: ${written} messages written and read successfully`);
}

function testStressTest() {
    const buffer = createTestBuffer();
    const writer = new RingBufferWriter(buffer);
    const reader = new RingBufferReader(buffer);

    const iterations = 1000;
    let totalWritten = 0;
    let totalRead = 0;

    for (let i = 0; i < iterations; i++) {
        // Random size message
        const size = 1 + Math.floor(Math.random() * 500);
        const payload = createPayload(size, i & 0xFF);

        const result = writer.write(payload);
        if (result.success) {
            totalWritten++;
        }

        // Randomly read (70% of the time)
        if (Math.random() < 0.7) {
            const messages = reader.readAll();
            totalRead += messages.length;

            if (reader.errors.length > 0) {
                throw new Error(`Corruption at iteration ${i}: ${JSON.stringify(reader.errors)}`);
            }
        }
    }

    // Read all remaining
    const remaining = reader.readAll();
    totalRead += remaining.length;

    if (reader.errors.length > 0) {
        throw new Error(`Final read corruption: ${JSON.stringify(reader.errors)}`);
    }

    if (totalRead !== totalWritten) {
        throw new Error(`Mismatch: wrote ${totalWritten}, read ${totalRead}`);
    }

    console.log(`  Stress test: ${totalWritten} messages processed successfully`);
}

// Utility function
function arraysEqual(a, b) {
    if (a.length !== b.length) return false;
    for (let i = 0; i < a.length; i++) {
        if (a[i] !== b[i]) return false;
    }
    return true;
}

// Run tests if executed directly in Node.js
if (typeof module !== 'undefined' && typeof require !== 'undefined' && require.main === module) {
    const success = runTests();
    process.exit(success ? 0 : 1);
}

// Export for Node.js
if (typeof module !== 'undefined' && typeof exports !== 'undefined') {
    module.exports = {
        runTests,
        RingBufferWriter,
        RingBufferReader,
        createTestBuffer,
        BUFFER_CONSTANTS
    };
}

// Export for browser (make functions globally available)
if (typeof window !== 'undefined') {
    window.runTests = runTests;
    window.RingBufferWriter = RingBufferWriter;
    window.RingBufferReader = RingBufferReader;
    window.createTestBuffer = createTestBuffer;
    window.BUFFER_CONSTANTS = BUFFER_CONSTANTS;
}

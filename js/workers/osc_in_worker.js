/*
    SuperSonic - SuperCollider AudioWorklet WebAssembly port
    Copyright (c) 2025 Sam Aaron

    Based on SuperCollider by James McCartney and community
    GPL v3 or later
*/

/**
 * OSC IN Worker - Receives OSC messages from scsynth
 * Uses Atomics.wait() for instant wake when data arrives
 * Reads from OUT ring buffer and forwards to main thread
 * ES5-compatible for Qt WebEngine
 */

// Ring buffer configuration
var sharedBuffer = null;
var ringBufferBase = null;
var atomicView = null;
var dataView = null;
var uint8View = null;

// Ring buffer layout constants (loaded from WASM at initialization)
var bufferConstants = null;

// Control indices (calculated after init)
var CONTROL_INDICES = {};

// Worker state
var running = false;

// Statistics
var stats = {
    messagesReceived: 0,
    lastSequenceReceived: -1,
    droppedMessages: 0,
    wakeups: 0,
    timeouts: 0
};

/**
 * Initialize ring buffer access
 */
function initRingBuffer(buffer, base, constants) {
    sharedBuffer = buffer;
    ringBufferBase = base;
    bufferConstants = constants;
    atomicView = new Int32Array(sharedBuffer);
    dataView = new DataView(sharedBuffer);
    uint8View = new Uint8Array(sharedBuffer);

    // Calculate control indices using constants from WASM
    CONTROL_INDICES = {
        OUT_HEAD: (ringBufferBase + bufferConstants.CONTROL_START + 8) / 4,
        OUT_TAIL: (ringBufferBase + bufferConstants.CONTROL_START + 12) / 4
    };
}

/**
 * Read all available messages from OUT buffer
 */
function readMessages() {
    var head = Atomics.load(atomicView, CONTROL_INDICES.OUT_HEAD);
    var tail = Atomics.load(atomicView, CONTROL_INDICES.OUT_TAIL);

    var messages = [];

    if (head === tail) {
        return messages; // No messages
    }

    var currentTail = tail;
    var messagesRead = 0;
    var maxMessages = 100;

    while (currentTail !== head && messagesRead < maxMessages) {
        var readPos = ringBufferBase + bufferConstants.OUT_BUFFER_START + currentTail;

        // Read message header (now always contiguous due to padding)
        var magic = dataView.getUint32(readPos, true);

        // Check for padding marker - skip to beginning
        if (magic === bufferConstants.PADDING_MAGIC) {
            currentTail = 0;
            continue;
        }

        if (magic !== bufferConstants.MESSAGE_MAGIC) {
            console.error('[OSCInWorker] Corrupted message at position', currentTail);
            stats.droppedMessages++;
            // Skip this byte and continue
            currentTail = (currentTail + 1) % bufferConstants.OUT_BUFFER_SIZE;
            continue;
        }

        var length = dataView.getUint32(readPos + 4, true);
        var sequence = dataView.getUint32(readPos + 8, true);
        var padding = dataView.getUint32(readPos + 12, true);  // unused padding field

        // Validate message length
        if (length < bufferConstants.MESSAGE_HEADER_SIZE || length > bufferConstants.OUT_BUFFER_SIZE) {
            console.error('[OSCInWorker] Invalid message length:', length);
            stats.droppedMessages++;
            currentTail = (currentTail + 1) % bufferConstants.OUT_BUFFER_SIZE;
            continue;
        }

        // Check for dropped messages via sequence
        if (stats.lastSequenceReceived >= 0) {
            var expectedSeq = (stats.lastSequenceReceived + 1) & 0xFFFFFFFF;
            if (sequence !== expectedSeq) {
                var dropped = (sequence - expectedSeq + 0x100000000) & 0xFFFFFFFF;
                if (dropped < 1000) { // Sanity check
                    console.warn('[OSCInWorker] Detected', dropped, 'dropped messages (expected seq', expectedSeq, 'got', sequence, ')');
                    stats.droppedMessages += dropped;
                }
            }
        }
        stats.lastSequenceReceived = sequence;

        // Read payload (OSC binary data) - now contiguous due to padding
        var payloadLength = length - bufferConstants.MESSAGE_HEADER_SIZE;
        var payloadStart = readPos + bufferConstants.MESSAGE_HEADER_SIZE;

        // Create a proper copy (not a view into SharedArrayBuffer)
        var payload = new Uint8Array(payloadLength);
        for (var i = 0; i < payloadLength; i++) {
            payload[i] = uint8View[payloadStart + i];
        }

        messages.push({
            oscData: payload,
            sequence: sequence
        });

        // Move to next message
        currentTail = (currentTail + length) % bufferConstants.OUT_BUFFER_SIZE;
        messagesRead++;
        stats.messagesReceived++;
    }

    // Update tail pointer (consume messages)
    if (messagesRead > 0) {
        Atomics.store(atomicView, CONTROL_INDICES.OUT_TAIL, currentTail);
    }

    return messages;
}

/**
 * Main wait loop using Atomics.wait for instant wake
 */
function waitLoop() {
    while (running) {
        try {
            // Get current OUT_HEAD value
            var currentHead = Atomics.load(atomicView, CONTROL_INDICES.OUT_HEAD);
            var currentTail = Atomics.load(atomicView, CONTROL_INDICES.OUT_TAIL);

            // If buffer is empty, wait for AudioWorklet to notify us
            if (currentHead === currentTail) {
                // Wait for up to 100ms (allows checking stop signal)
                var result = Atomics.wait(atomicView, CONTROL_INDICES.OUT_HEAD, currentHead, 100);

                if (result === 'ok' || result === 'not-equal') {
                    // We were notified or value changed!
                    stats.wakeups++;
                } else if (result === 'timed-out') {
                    stats.timeouts++;
                    continue; // Check running flag
                }
            }

            // Read all available messages
            var messages = readMessages();

            if (messages.length > 0) {
                // Send to main thread
                self.postMessage({
                    type: 'messages',
                    messages: messages,
                    stats: {
                        wakeups: stats.wakeups,
                        timeouts: stats.timeouts,
                        messagesReceived: stats.messagesReceived,
                        droppedMessages: stats.droppedMessages
                    }
                });
            }

        } catch (error) {
            console.error('[OSCInWorker] Error in wait loop:', error);
            self.postMessage({
                type: 'error',
                error: error.message
            });

            // Brief pause on error before retrying (use existing atomicView)
            // Wait on a value that won't change for 10ms as a simple delay
            Atomics.wait(atomicView, 0, atomicView[0], 10);
        }
    }
}

/**
 * Start the wait loop
 */
function start() {
    if (!sharedBuffer) {
        console.error('[OSCInWorker] Cannot start - not initialized');
        return;
    }

    if (running) {
        console.warn('[OSCInWorker] Already running');
        return;
    }

    running = true;
    waitLoop();
}

/**
 * Stop the wait loop
 */
function stop() {
    running = false;
}

/**
 * Handle messages from main thread
 */
self.onmessage = function(event) {
    var data = event.data;

    try {
        switch (data.type) {
            case 'init':
                initRingBuffer(data.sharedBuffer, data.ringBufferBase, data.bufferConstants);
                self.postMessage({ type: 'initialized' });
                break;

            case 'start':
                start();
                break;

            case 'stop':
                stop();
                break;

            case 'getStats':
                self.postMessage({
                    type: 'stats',
                    stats: stats
                });
                break;

            default:
                console.warn('[OSCInWorker] Unknown message type:', data.type);
        }
    } catch (error) {
        console.error('[OSCInWorker] Error:', error);
        self.postMessage({
            type: 'error',
            error: error.message
        });
    }
};

console.log('[OSCInWorker] Script loaded');
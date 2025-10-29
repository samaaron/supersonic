/*
    SuperSonic - SuperCollider AudioWorklet WebAssembly port
    Copyright (c) 2025 Sam Aaron

    Based on SuperCollider by James McCartney and community
    GPL v3 or later
*/

/**
 * DEBUG Worker - Receives debug messages from AudioWorklet
 * Uses Atomics.wait() for instant wake when debug logs arrive
 * Reads from DEBUG ring buffer and forwards to main thread
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
    wakeups: 0,
    timeouts: 0,
    bytesRead: 0
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
        DEBUG_HEAD: (ringBufferBase + bufferConstants.CONTROL_START + 16) / 4,
        DEBUG_TAIL: (ringBufferBase + bufferConstants.CONTROL_START + 20) / 4
    };
}

/**
 * Read debug messages from buffer
 */
function readDebugMessages() {
    var head = Atomics.load(atomicView, CONTROL_INDICES.DEBUG_HEAD);
    var tail = Atomics.load(atomicView, CONTROL_INDICES.DEBUG_TAIL);

    if (head === tail) {
        return null; // No messages
    }

    var messages = [];
    var currentTail = tail;
    var messagesRead = 0;
    var maxMessages = 10; // Process up to 10 messages per wake

    while (currentTail !== head && messagesRead < maxMessages) {
        var readPos = ringBufferBase + bufferConstants.DEBUG_BUFFER_START + currentTail;

        // Read message header (now always contiguous due to padding)
        var magic = dataView.getUint32(readPos, true);

        // Check for padding marker - skip to beginning
        if (magic === bufferConstants.PADDING_MAGIC) {
            currentTail = 0;
            continue;
        }

        // Validate message magic
        if (magic !== bufferConstants.MESSAGE_MAGIC) {
            console.error('[DebugWorker] Corrupted message at position', currentTail);
            // Skip this byte and continue
            currentTail = (currentTail + 1) % bufferConstants.DEBUG_BUFFER_SIZE;
            continue;
        }

        var length = dataView.getUint32(readPos + 4, true);
        var sequence = dataView.getUint32(readPos + 8, true);

        // Validate message length
        if (length < bufferConstants.MESSAGE_HEADER_SIZE || length > bufferConstants.DEBUG_BUFFER_SIZE) {
            console.error('[DebugWorker] Invalid message length:', length);
            currentTail = (currentTail + 1) % bufferConstants.DEBUG_BUFFER_SIZE;
            continue;
        }

        // Read payload (debug text) - now contiguous due to padding
        var payloadLength = length - bufferConstants.MESSAGE_HEADER_SIZE;
        var payloadStart = readPos + bufferConstants.MESSAGE_HEADER_SIZE;

        // Convert bytes to string using TextDecoder for proper UTF-8 handling
        var payloadBytes = uint8View.slice(payloadStart, payloadStart + payloadLength);
        var decoder = new TextDecoder('utf-8');
        var messageText = decoder.decode(payloadBytes);

        // Remove trailing newline if present
        if (messageText.endsWith('\n')) {
            messageText = messageText.slice(0, -1);
        }

        messages.push({
            text: messageText,
            timestamp: performance.now(),
            sequence: sequence
        });

        // Move to next message
        currentTail = (currentTail + length) % bufferConstants.DEBUG_BUFFER_SIZE;
        messagesRead++;
        stats.messagesReceived++;
    }

    // Update tail pointer (consume messages)
    if (messagesRead > 0) {
        Atomics.store(atomicView, CONTROL_INDICES.DEBUG_TAIL, currentTail);
        stats.bytesRead += messagesRead;
    }

    return messages.length > 0 ? messages : null;
}

/**
 * Main wait loop using Atomics.wait for instant wake
 */
function waitLoop() {
    while (running) {
        try {
            // Get current DEBUG_HEAD value
            var currentHead = Atomics.load(atomicView, CONTROL_INDICES.DEBUG_HEAD);
            var currentTail = Atomics.load(atomicView, CONTROL_INDICES.DEBUG_TAIL);

            // If buffer is empty, wait for AudioWorklet to notify us
            if (currentHead === currentTail) {
                // Wait for up to 100ms (allows checking stop signal)
                var result = Atomics.wait(atomicView, CONTROL_INDICES.DEBUG_HEAD, currentHead, 100);

                if (result === 'ok' || result === 'not-equal') {
                    // We were notified or value changed!
                    stats.wakeups++;
                } else if (result === 'timed-out') {
                    stats.timeouts++;
                    continue; // Check running flag
                }
            }

            // Read all available debug messages
            var messages = readDebugMessages();

            if (messages && messages.length > 0) {
                // Send to main thread
                self.postMessage({
                    type: 'debug',
                    messages: messages,
                    stats: {
                        wakeups: stats.wakeups,
                        timeouts: stats.timeouts,
                        messagesReceived: stats.messagesReceived,
                        bytesRead: stats.bytesRead
                    }
                });
            }

        } catch (error) {
            console.error('[DebugWorker] Error in wait loop:', error);
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
        console.error('[DebugWorker] Cannot start - not initialized');
        return;
    }

    if (running) {
        console.warn('[DebugWorker] Already running');
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
 * Clear debug buffer
 */
function clear() {
    if (!sharedBuffer) return;

    // Reset head and tail to 0
    Atomics.store(atomicView, CONTROL_INDICES.DEBUG_HEAD, 0);
    Atomics.store(atomicView, CONTROL_INDICES.DEBUG_TAIL, 0);
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

            case 'clear':
                clear();
                break;

            case 'getStats':
                self.postMessage({
                    type: 'stats',
                    stats: stats
                });
                break;

            default:
                console.warn('[DebugWorker] Unknown message type:', data.type);
        }
    } catch (error) {
        console.error('[DebugWorker] Error:', error);
        self.postMessage({
            type: 'error',
            error: error.message
        });
    }
};

console.log('[DebugWorker] Script loaded');
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

// Ring buffer layout constants
var CONTROL_START = 20480;
var DEBUG_BUFFER_START = 16384;
var DEBUG_BUFFER_SIZE = 4096;
var DEBUG_PADDING_MARKER = 0xFF;

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
function initRingBuffer(buffer, base) {
    sharedBuffer = buffer;
    ringBufferBase = base;
    atomicView = new Int32Array(sharedBuffer);
    dataView = new DataView(sharedBuffer);
    uint8View = new Uint8Array(sharedBuffer);

    // Calculate control indices
    CONTROL_INDICES = {
        DEBUG_HEAD: (ringBufferBase + CONTROL_START + 16) / 4,
        DEBUG_TAIL: (ringBufferBase + CONTROL_START + 20) / 4
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

    // Calculate available bytes
    var available = (head - tail + DEBUG_BUFFER_SIZE) % DEBUG_BUFFER_SIZE;
    if (available === 0) {
        return null;
    }

    // Read all available debug text
    var messages = [];
    var currentMessage = [];
    var currentTail = tail;
    var bytesRead = 0;

    while (currentTail !== head && bytesRead < available) {
        var readPos = ringBufferBase + DEBUG_BUFFER_START + currentTail;
        var byte = uint8View[readPos];

        // Check for padding marker - skip to beginning
        if (byte === DEBUG_PADDING_MARKER) {
            currentTail = 0;
            bytesRead = 0; // Reset to start reading from position 0
            continue;
        }

        if (byte === 10) { // newline (messages are always complete now due to padding)
            if (currentMessage.length > 0) {
                // Convert accumulated bytes to string
                var messageText = '';
                for (var i = 0; i < currentMessage.length; i++) {
                    messageText += String.fromCharCode(currentMessage[i]);
                }

                messages.push({
                    text: messageText,
                    timestamp: performance.now()
                });

                currentMessage = [];
                stats.messagesReceived++;
            }
        } else {
            currentMessage.push(byte);
        }

        currentTail = (currentTail + 1) % DEBUG_BUFFER_SIZE;
        bytesRead++;
    }

    // Update tail pointer (consume messages)
    if (bytesRead > 0) {
        Atomics.store(atomicView, CONTROL_INDICES.DEBUG_TAIL, currentTail);
        stats.bytesRead += bytesRead;
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
                initRingBuffer(data.sharedBuffer, data.ringBufferBase);
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
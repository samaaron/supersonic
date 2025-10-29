/*
    SuperSonic - SuperCollider AudioWorklet WebAssembly port
    Copyright (c) 2025 Sam Aaron

    Based on SuperCollider by James McCartney and community
    GPL v3 or later
*/

/**
 * OSC OUT Worker - Scheduler for sending OSC bundles to scsynth
 * Handles timed bundles with priority queue scheduling
 * Writes directly to SharedArrayBuffer ring buffer
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

// Scheduling state
var scheduledEvents = [];
var currentTimer = null;
var cachedTimeDelta = null;
var minimumScheduleRequirementS = 0.002; // 2ms for audio precision
var latencyS = 0.05; // 50ms latency compensation for scsynth

// Message queue for handling backpressure
var immediateQueue = []; // Queue of messages waiting to be written
var isWriting = false; // Flag to prevent concurrent write attempts
var writeRetryTimer = null;

// Statistics
var stats = {
    bundlesScheduled: 0,
    bundlesWritten: 0,
    bundlesDropped: 0,
    bufferOverruns: 0,
    retries: 0,
    queueDepth: 0,
    maxQueueDepth: 0
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
        IN_HEAD: (ringBufferBase + bufferConstants.CONTROL_START + 0) / 4,
        IN_TAIL: (ringBufferBase + bufferConstants.CONTROL_START + 4) / 4
    };
}

/**
 * Queue a message for writing (handles backpressure)
 */
function queueMessage(oscMessage) {
    immediateQueue.push(oscMessage);
    stats.queueDepth = immediateQueue.length;

    if (stats.queueDepth > stats.maxQueueDepth) {
        stats.maxQueueDepth = stats.queueDepth;
    }

    // Start processing if not already running
    if (!isWriting) {
        processQueue();
    }
}

/**
 * Process the message queue - blocks until space is available
 */
function processQueue() {
    if (isWriting || immediateQueue.length === 0) {
        return;
    }

    isWriting = true;

    function processNext() {
        if (immediateQueue.length === 0) {
            isWriting = false;
            stats.queueDepth = 0;
            return;
        }

        var message = immediateQueue[0]; // Peek at first message

        // Block until there's space, then write
        var success = writeToRingBufferBlocking(message);

        if (success) {
            // Success! Remove from queue
            immediateQueue.shift();
            stats.queueDepth = immediateQueue.length;

            // Process next message
            if (immediateQueue.length > 0) {
                setTimeout(processNext, 0);
            } else {
                isWriting = false;
            }
        } else {
            // Fatal error (message too large or not initialized)
            console.error('[OSCOutWorker] Fatal error, dropping message');
            immediateQueue.shift(); // Remove bad message
            stats.bundlesDropped++;
            stats.queueDepth = immediateQueue.length;

            // Continue with next message
            setTimeout(processNext, 0);
        }
    }

    processNext();
}

/**
 * Write OSC message to ring buffer - blocks until space available
 * Returns true on success, false on fatal error (message too large)
 */
function writeToRingBufferBlocking(oscMessage) {
    if (!sharedBuffer) {
        console.error('[OSCOutWorker] Not initialized');
        return false;
    }

    var payloadSize = oscMessage.length;
    var totalSize = bufferConstants.MESSAGE_HEADER_SIZE + payloadSize;

    // Check if message fits in buffer at all (account for padding at wrap)
    if (totalSize > bufferConstants.IN_BUFFER_SIZE - bufferConstants.MESSAGE_HEADER_SIZE) {
        console.error('[OSCOutWorker] Message too large:', totalSize, 'max:', bufferConstants.IN_BUFFER_SIZE - bufferConstants.MESSAGE_HEADER_SIZE);
        return false;
    }

    // Keep trying until we have space
    while (true) {
        var head = Atomics.load(atomicView, CONTROL_INDICES.IN_HEAD);
        var tail = Atomics.load(atomicView, CONTROL_INDICES.IN_TAIL);

        // Check available space
        var available = (bufferConstants.IN_BUFFER_SIZE - 1 - head + tail) % bufferConstants.IN_BUFFER_SIZE;

        if (available >= totalSize) {
            // Check if message fits contiguously, otherwise write padding and wrap
            var spaceToEnd = bufferConstants.IN_BUFFER_SIZE - head;

            if (totalSize > spaceToEnd) {
                // Message won't fit at end - write padding marker and wrap to beginning
                var paddingPos = ringBufferBase + bufferConstants.IN_BUFFER_START + head;
                dataView.setUint32(paddingPos, bufferConstants.PADDING_MAGIC, true);
                dataView.setUint32(paddingPos + 4, 0, true);
                dataView.setUint32(paddingPos + 8, 0, true);
                dataView.setUint32(paddingPos + 12, 0, true);

                // Wrap head to beginning
                head = 0;
            }

            // We have space! Write the message (now guaranteed contiguous)
            var writePos = ringBufferBase + bufferConstants.IN_BUFFER_START + head;

            // Write message header
            dataView.setUint32(writePos, bufferConstants.MESSAGE_MAGIC, true);
            dataView.setUint32(writePos + 4, totalSize, true);
            dataView.setUint32(writePos + 8, stats.bundlesWritten, true); // sequence
            dataView.setUint32(writePos + 12, 0, true); // padding

            // Write payload
            uint8View.set(oscMessage, writePos + bufferConstants.MESSAGE_HEADER_SIZE);

            // Update head pointer (publish message)
            var newHead = (head + totalSize) % bufferConstants.IN_BUFFER_SIZE;
            Atomics.store(atomicView, CONTROL_INDICES.IN_HEAD, newHead);

            stats.bundlesWritten++;
            return true;
        }

        // Buffer is full - wait for tail to move (scsynth to consume)
        stats.bufferOverruns++;

        // Wait on the tail pointer - will wake when scsynth consumes data
        // Timeout after 100ms to check if worker should stop
        var result = Atomics.wait(atomicView, CONTROL_INDICES.IN_TAIL, tail, 100);

        if (result === 'ok' || result === 'not-equal') {
            // Tail moved! Loop will retry
            stats.retries++;
        }
        // On timeout, loop continues to retry (allows checking for stop signal)
    }
}

/**
 * Get or set cached time delta for synchronization
 */
function getOrSetTimeDelta(delta) {
    if (cachedTimeDelta === null) {
        cachedTimeDelta = delta;
    }
    return cachedTimeDelta;
}

/**
 * Check if data is an OSC bundle (starts with "#bundle\0")
 */
function isBundle(data) {
    if (data.length < 16) return false;
    var bundleTag = String.fromCharCode.apply(null, data.slice(0, 8));
    return bundleTag === '#bundle\0';
}

/**
 * Parse OSC bundle timestamp from binary data
 * OSC bundles start with "#bundle\0" followed by 8-byte NTP timestamp
 */
function parseBundleTimestamp(data) {
    if (!isBundle(data)) return null;

    // Read NTP timestamp (8 bytes, big-endian)
    var view = new DataView(data.buffer, data.byteOffset + 8, 8);
    var seconds = view.getUint32(0, false); // NTP seconds since 1900
    var fraction = view.getUint32(4, false); // NTP fractional seconds

    // Convert NTP to JavaScript time
    // NTP epoch is 1900, JS epoch is 1970 (difference: 2208988800 seconds)
    var NTP_TO_UNIX = 2208988800;

    // Special OSC timestamps
    if (seconds === 0 && fraction === 1) {
        return 0; // Immediate execution
    }

    // Convert to JavaScript timestamp (milliseconds since 1970)
    var unixSeconds = seconds - NTP_TO_UNIX;
    var milliseconds = (fraction / 4294967296) * 1000; // Convert fraction to ms

    return (unixSeconds * 1000) + milliseconds;
}

/**
 * Extract OSC messages from a bundle
 * Returns array of message buffers
 */
function extractMessagesFromBundle(data) {
    var messages = [];

    if (!isBundle(data)) {
        // Not a bundle, return as single message
        return [data];
    }

    // Skip "#bundle\0" (8 bytes) and timestamp (8 bytes)
    var offset = 16;

    while (offset < data.length) {
        // Read message size (4 bytes, big-endian)
        if (offset + 4 > data.length) break;

        var view = new DataView(data.buffer, data.byteOffset + offset, 4);
        var messageSize = view.getInt32(0, false);
        offset += 4;

        if (messageSize <= 0 || offset + messageSize > data.length) break;

        // Extract message data
        var messageData = data.slice(offset, offset + messageSize);

        // Check if this is a nested bundle
        if (isBundle(messageData)) {
            // Recursively extract from nested bundle
            var nestedMessages = extractMessagesFromBundle(messageData);
            messages = messages.concat(nestedMessages);
        } else {
            // It's a message, add it
            messages.push(messageData);
        }

        offset += messageSize;

        // Align to 4-byte boundary if needed
        while (offset % 4 !== 0 && offset < data.length) {
            offset++;
        }
    }

    return messages;
}

/**
 * Process incoming OSC data (message or bundle)
 * Pre-scheduler: waits for calculated time then sends to ring buffer
 * waitTimeMs is calculated by SuperSonic based on AudioContext time
 */
function processOSC(oscData, editorId, runTag, waitTimeMs) {
    stats.bundlesScheduled++;

    // If no wait time provided, or wait time is 0 or negative, send immediately
    if (waitTimeMs === null || waitTimeMs === undefined || waitTimeMs <= 0) {
        queueMessage(oscData);
        return;
    }

    // Schedule to send after waitTimeMs
    setTimeout(function() {
        queueMessage(oscData);
    }, waitTimeMs);
}

/**
 * Process immediate send - forces immediate execution by unpacking bundles
 * Bundles are unpacked to individual messages (stripping timestamps)
 * Messages are sent as-is
 * Used when the caller wants immediate execution without scheduling
 */
function processImmediate(oscData) {
    if (isBundle(oscData)) {
        // Extract all messages from the bundle (removes timestamp wrapper)
        // Send each message individually for immediate execution
        var messages = extractMessagesFromBundle(oscData);
        for (var i = 0; i < messages.length; i++) {
            queueMessage(messages[i]);
        }
    } else {
        // Regular message - send as-is
        queueMessage(oscData);
    }
}

/**
 * Insert event into priority queue
 */
function insertEvent(userId, editorId, runId, runTag, adjustedTimeS, oscBundle) {
    var info = { userId: userId, editorId: editorId, runTag: runTag, runId: runId };
    scheduledEvents.push([adjustedTimeS, info, oscBundle]);
    scheduledEvents.sort(function(a, b) { return a[0] - b[0]; });
    scheduleNextEvent();
}

/**
 * Schedule the next event timer
 */
function scheduleNextEvent() {
    if (scheduledEvents.length === 0) {
        clearCurrentTimer();
        return;
    }

    var nextEvent = scheduledEvents[0];
    var adjustedTimeS = nextEvent[0];

    if (!currentTimer || (currentTimer && currentTimer.timeS > adjustedTimeS)) {
        addRunNextEventTimer(adjustedTimeS);
    }
}

/**
 * Clear current timer
 */
function clearCurrentTimer() {
    if (currentTimer) {
        clearTimeout(currentTimer.timerId);
        currentTimer = null;
    }
}

/**
 * Add timer for next event
 */
function addRunNextEventTimer(adjustedTimeS) {
    clearCurrentTimer();

    var nowS = Date.now() / 1000;
    var timeDeltaS = adjustedTimeS - nowS;

    if (timeDeltaS <= minimumScheduleRequirementS) {
        runNextEvent();
    } else {
        var delayMs = (timeDeltaS - minimumScheduleRequirementS) * 1000;
        currentTimer = {
            timeS: adjustedTimeS,
            timerId: setTimeout(function() {
                currentTimer = null;
                runNextEvent();
            }, delayMs)
        };
    }
}

/**
 * Run the next scheduled event
 */
function runNextEvent() {
    clearCurrentTimer();

    if (scheduledEvents.length === 0) {
        return;
    }

    var event = scheduledEvents.shift();
    var data = event[2];

    // Send the complete bundle unchanged (with original timestamp)
    queueMessage(data);

    scheduleNextEvent();
}

/**
 * Cancel events by editor and tag
 */
function cancelEditorTag(editorId, runTag) {
    scheduledEvents = scheduledEvents.filter(function(e) {
        return e[1].runTag !== runTag || e[1].editorId !== editorId;
    });
    scheduleNextEvent();
}

/**
 * Cancel all events from an editor
 */
function cancelEditor(editorId) {
    scheduledEvents = scheduledEvents.filter(function(e) {
        return e[1].editorId !== editorId;
    });
    scheduleNextEvent();
}

/**
 * Cancel all scheduled events
 */
function cancelAllTags() {
    scheduledEvents = [];
    clearCurrentTimer();
}

/**
 * Reset time delta for resync
 */
function resetTimeDelta() {
    cachedTimeDelta = null;
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

            case 'send':
                // Single send method for both messages and bundles
                // waitTimeMs is calculated by SuperSonic based on AudioContext time
                processOSC(data.oscData, data.editorId, data.runTag, data.waitTimeMs);
                break;

            case 'sendImmediate':
                // Force immediate send, extracting all messages from bundles
                // Ignores timestamps - for apps that don't expect scheduling
                processImmediate(data.oscData);
                break;

            case 'cancelEditorTag':
                cancelEditorTag(data.editorId, data.runTag);
                break;

            case 'cancelEditor':
                cancelEditor(data.editorId);
                break;

            case 'cancelAll':
                cancelAllTags();
                break;

            case 'getStats':
                self.postMessage({
                    type: 'stats',
                    stats: stats
                });
                break;

            default:
                console.warn('[OSCOutWorker] Unknown message type:', data.type);
        }
    } catch (error) {
        console.error('[OSCOutWorker] Error:', error);
        self.postMessage({
            type: 'error',
            error: error.message
        });
    }
};

console.log('[OSCOutWorker] Script loaded');
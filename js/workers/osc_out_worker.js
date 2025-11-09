/*
    SuperSonic - SuperCollider AudioWorklet WebAssembly port
    Copyright (c) 2025 Sam Aaron

    Based on SuperCollider by James McCartney and community
    GPL v3 or later
*/

/**
 * OSC OUT Worker - Scheduler for OSC bundles
 * Handles timed bundles and forwards them to the writer worker
 * NO LONGER writes to ring buffer directly - all writes go through osc_writer_worker.js
 * ES5-compatible for Qt WebEngine
 */

// Reference to the writer worker
var writerWorker = null;

// Scheduling state
var scheduledEvents = [];
var currentTimer = null;
var cachedTimeDelta = null;
var minimumScheduleRequirementS = 0.002; // 2ms for audio precision
var latencyS = 0.05; // 50ms latency compensation for scsynth

var DEBUG_SCHED_LOGS = false;
function schedulerLog() {
    if (DEBUG_SCHED_LOGS) {
        console.log.apply(console, arguments);
    }
}
function schedulerWarn() {
    if (DEBUG_SCHED_LOGS) {
        console.warn.apply(console, arguments);
    }
}

// Statistics
var stats = {
    bundlesScheduled: 0,
    bundlesSentToWriter: 0
};

/**
 * Initialize scheduler
 */
function init(buffer, base, constants) {
    // We don't need the ring buffer anymore, but keep the params for compatibility
    schedulerLog('[OSCSchedulerWorker] Initialized (scheduler only, no ring buffer access)');
}

/**
 * Send message to writer worker
 */
function sendToWriter(oscMessage) {
    if (!writerWorker) {
        console.error('[OSCSchedulerWorker] Writer worker not set');
        return;
    }

    // Send to writer worker via postMessage
    writerWorker.postMessage({
        type: 'write',
        oscData: oscMessage
    });

    stats.bundlesSentToWriter++;
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
 * Pre-scheduler: waits for calculated time then sends to writer
 * waitTimeMs is calculated by SuperSonic based on AudioContext time
 */
function processOSC(oscData, editorId, runTag, waitTimeMs) {
    stats.bundlesScheduled++;

    // If no wait time provided, or wait time is 0 or negative, send immediately
    if (waitTimeMs === null || waitTimeMs === undefined || waitTimeMs <= 0) {
        sendToWriter(oscData);
        return;
    }

    // Schedule to send after waitTimeMs
    setTimeout(function() {
        sendToWriter(oscData);
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
            sendToWriter(messages[i]);
        }
    } else {
        // Regular message - send as-is
        sendToWriter(oscData);
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
    sendToWriter(data);

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
                init(data.sharedBuffer, data.ringBufferBase, data.bufferConstants);
                self.postMessage({ type: 'initialized' });
                break;

            case 'setWriterWorker':
                // Set reference to writer worker (passed as MessagePort)
                writerWorker = data.port;
                schedulerLog('[OSCSchedulerWorker] Writer worker connected');
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
                schedulerWarn('[OSCOutWorker] Unknown message type:', data.type);
        }
    } catch (error) {
        console.error('[OSCOutWorker] Error:', error);
        self.postMessage({
            type: 'error',
            error: error.message
        });
    }
};

schedulerLog('[OSCSchedulerWorker] Script loaded - scheduler only, delegates to writer worker');

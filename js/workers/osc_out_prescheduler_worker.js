/*
    SuperSonic - OSC Pre-Scheduler Worker
    Ports the Bleep pre-scheduler design:
    - Single priority queue of future bundles/events
    - One timer driving dispatch (no per-event setTimeout storm)
    - Tag-based cancellation to drop pending runs before they hit WASM
*/

// Writer worker port (MessagePort from main thread)
var writerWorker = null;

// State propagated for backwards compatibility (not used directly here)
var sharedBuffer = null;
var ringBufferBase = null;
var bufferConstants = null;

// Priority queue (sorted array) of scheduled events
// Each entry: { timeMs, seq, editorId, runTag, oscData }
var scheduledEvents = [];
var nextTimer = null;
var sequenceCounter = 0;

// Statistics
var stats = {
    bundlesScheduled: 0,
    bundlesSentToWriter: 0,
    eventsPending: 0,
    maxEventsPending: 0,
    eventsCancelled: 0
};

// Minimum time window before dispatch (ms) - dispatch immediately when ready
var DISPATCH_LEEWAY_MS = 0.0;

function schedulerLog() {
    // Toggle to true for verbose diagnostics
    var DEBUG = false;
    if (DEBUG) {
        console.log.apply(console, arguments);
    }
}

function sendToWriter(oscMessage) {
    if (!writerWorker) {
        console.error('[OSCPreSchedulerWorker] Writer worker not set');
        return;
    }

    writerWorker.postMessage({
        type: 'write',
        oscData: oscMessage
    });

    stats.bundlesSentToWriter++;
}

function scheduleEvent(waitTimeMs, editorId, runTag, oscData) {
    var targetTimeMs = performance.now() + waitTimeMs;
    var event = {
        timeMs: targetTimeMs,
        seq: sequenceCounter++,
        editorId: editorId,
        runTag: runTag || '',
        oscData: oscData
    };

    scheduledEvents.push(event);
    scheduledEvents.sort(eventComparator);

    stats.bundlesScheduled++;
    stats.eventsPending = scheduledEvents.length;
    if (stats.eventsPending > stats.maxEventsPending) {
        stats.maxEventsPending = stats.eventsPending;
    }

    scheduleNextDispatch();
}

function eventComparator(a, b) {
    if (a.timeMs === b.timeMs) {
        return a.seq - b.seq;
    }
    return a.timeMs - b.timeMs;
}

function scheduleNextDispatch() {
    if (nextTimer) {
        clearTimeout(nextTimer);
        nextTimer = null;
    }

    if (scheduledEvents.length === 0) {
        return;
    }

    var now = performance.now();
    var nextEvent = scheduledEvents[0];
    var delay = nextEvent.timeMs - now;

    if (delay <= DISPATCH_LEEWAY_MS) {
        dispatchDueEvents();
    } else {
        nextTimer = setTimeout(dispatchDueEvents, delay);
    }
}

function dispatchDueEvents() {
    nextTimer = null;
    var now = performance.now();

    while (scheduledEvents.length > 0) {
        var nextEvent = scheduledEvents[0];
        if (nextEvent.timeMs - now > DISPATCH_LEEWAY_MS) {
            break;
        }

        scheduledEvents.shift();
        stats.eventsPending = scheduledEvents.length;
        sendToWriter(nextEvent.oscData);
    }

    scheduleNextDispatch();
}

function cancelBy(predicate) {
    if (scheduledEvents.length === 0) {
        return;
    }

    var before = scheduledEvents.length;
    scheduledEvents = scheduledEvents.filter(function(event) {
        return !predicate(event);
    });
    var removed = before - scheduledEvents.length;

    if (removed > 0) {
        stats.eventsCancelled += removed;
        stats.eventsPending = scheduledEvents.length;
        scheduleNextDispatch();
    }
}

function cancelEditorTag(editorId, runTag) {
    cancelBy(function(event) {
        return event.editorId === editorId && event.runTag === runTag;
    });
}

function cancelEditor(editorId) {
    cancelBy(function(event) {
        return event.editorId === editorId;
    });
}

function cancelAllTags() {
    if (scheduledEvents.length === 0) {
        return;
    }
    stats.eventsCancelled += scheduledEvents.length;
    scheduledEvents = [];
    stats.eventsPending = 0;
    if (nextTimer) {
        clearTimeout(nextTimer);
        nextTimer = null;
    }
}

// Helpers reused from legacy worker for immediate send
function isBundle(data) {
    if (!data || data.length < 8) {
        return false;
    }
    return data[0] === 0x23 && data[1] === 0x62 && data[2] === 0x75 && data[3] === 0x6e &&
        data[4] === 0x64 && data[5] === 0x6c && data[6] === 0x65 && data[7] === 0x00;
}

function extractMessagesFromBundle(data) {
    var messages = [];
    var view = new DataView(data.buffer, data.byteOffset, data.byteLength);
    var offset = 16; // skip "#bundle\0" + timetag

    while (offset < data.length) {
        var messageSize = view.getInt32(offset, false);
        offset += 4;

        if (messageSize <= 0 || offset + messageSize > data.length) {
            break;
        }

        var messageData = data.slice(offset, offset + messageSize);
        messages.push(messageData);
        offset += messageSize;

        while (offset % 4 !== 0 && offset < data.length) {
            offset++;
        }
    }

    return messages;
}

function processImmediate(oscData) {
    if (isBundle(oscData)) {
        var messages = extractMessagesFromBundle(oscData);
        for (var i = 0; i < messages.length; i++) {
            sendToWriter(messages[i]);
        }
    } else {
        sendToWriter(oscData);
    }
}

// Message handling
self.onmessage = function(event) {
    var data = event.data;

    try {
        switch (data.type) {
            case 'init':
                sharedBuffer = data.sharedBuffer;
                ringBufferBase = data.ringBufferBase;
                bufferConstants = data.bufferConstants;
                schedulerLog('[OSCPreSchedulerWorker] Initialized');
                self.postMessage({ type: 'initialized' });
                break;

            case 'setWriterWorker':
                writerWorker = data.port;
                schedulerLog('[OSCPreSchedulerWorker] Writer worker connected');
                break;

            case 'send':
                if (data.waitTimeMs === null || data.waitTimeMs === undefined || data.waitTimeMs <= 0) {
                    processImmediate(data.oscData);
                } else {
                    scheduleEvent(
                        data.waitTimeMs,
                        data.editorId || 0,
                        data.runTag || '',
                        data.oscData
                    );
                }
                break;

            case 'sendImmediate':
                processImmediate(data.oscData);
                break;

            case 'cancelEditorTag':
                if (data.runTag !== undefined && data.runTag !== null && data.runTag !== '') {
                    cancelEditorTag(data.editorId || 0, data.runTag);
                }
                break;

            case 'cancelEditor':
                cancelEditor(data.editorId || 0);
                break;

            case 'cancelAll':
                cancelAllTags();
                break;

            case 'getStats':
                self.postMessage({
                    type: 'stats',
                    stats: {
                        bundlesScheduled: stats.bundlesScheduled,
                        bundlesSentToWriter: stats.bundlesSentToWriter,
                        eventsPending: stats.eventsPending,
                        maxEventsPending: stats.maxEventsPending,
                        eventsCancelled: stats.eventsCancelled
                    }
                });
                break;

            default:
                console.warn('[OSCPreSchedulerWorker] Unknown message type:', data.type);
        }
    } catch (error) {
        console.error('[OSCPreSchedulerWorker] Error:', error);
        self.postMessage({
            type: 'error',
            error: error.message
        });
    }
};

schedulerLog('[OSCPreSchedulerWorker] Script loaded');

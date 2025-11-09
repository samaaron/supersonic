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

// Priority queue implemented as binary min-heap
// Entries: { timeMs, seq, editorId, runTag, oscData }
var eventHeap = [];
var nextTimer = null;
var sequenceCounter = 0;
var isDispatching = false;  // Prevent reentrancy into dispatch loop

// Statistics
var stats = {
    bundlesScheduled: 0,
    bundlesSentToWriter: 0,
    eventsPending: 0,
    maxEventsPending: 0,
    eventsCancelled: 0,
    totalDispatches: 0,
    totalLateDispatchMs: 0,
    maxLateDispatchMs: 0,
    totalSendTasks: 0,
    totalSendProcessMs: 0,
    maxSendProcessMs: 0
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

function getBundleTimestamp(oscMessage) {
    if (oscMessage.length >= 16 && oscMessage[0] === 0x23) {
        var view = new DataView(oscMessage.buffer, oscMessage.byteOffset);
        var ntpSeconds = view.getUint32(8, false);
        var ntpFraction = view.getUint32(12, false);
        return ntpSeconds + ntpFraction / 0x100000000;
    }
    return null;
}

function sendToWriter(oscMessage) {
    if (!writerWorker) {
        console.error('[OSCPreSchedulerWorker] Writer worker not set');
        return;
    }

    var bundleTimestamp = getBundleTimestamp(oscMessage);
    var sendTime = performance.now();

    if (bundleTimestamp !== null) {
        console.log(`[PreScheduler] Dispatching bundle NTP=${bundleTimestamp.toFixed(3)} at perf=${sendTime.toFixed(2)}ms`);
    }

    writerWorker.postMessage({
        type: 'write',
        oscData: oscMessage
    });

    stats.bundlesSentToWriter++;
}

function scheduleEvent(waitTimeMs, editorId, runTag, oscData) {
    var targetTimeMs = performance.now() + waitTimeMs;
    var bundleTimestamp = getBundleTimestamp(oscData);
    var event = {
        timeMs: targetTimeMs,
        seq: sequenceCounter++,
        editorId: editorId,
        runTag: runTag || '',
        oscData: oscData
    };

    var wasEmpty = eventHeap.length === 0;
    heapPush(event);

    stats.bundlesScheduled++;
    stats.eventsPending = eventHeap.length;
    if (stats.eventsPending > stats.maxEventsPending) {
        stats.maxEventsPending = stats.eventsPending;
    }

    if (bundleTimestamp !== null) {
        console.log(`[PreScheduler] Scheduled bundle NTP=${bundleTimestamp.toFixed(3)} for perf=${targetTimeMs.toFixed(2)}ms (+${waitTimeMs.toFixed(0)}ms), queue=${stats.eventsPending}`);
    } else if (wasEmpty || stats.eventsPending % 10 === 0) {
        console.log(`[PreScheduler] Scheduled event for +${waitTimeMs.toFixed(0)}ms, queue now ${stats.eventsPending} events`);
    }

    scheduleNextDispatch();
}

function heapPush(event) {
    eventHeap.push(event);
    siftUp(eventHeap.length - 1);
}

function heapPeek() {
    return eventHeap.length > 0 ? eventHeap[0] : null;
}

function heapPop() {
    if (eventHeap.length === 0) {
        return null;
    }
    var top = eventHeap[0];
    var last = eventHeap.pop();
    if (eventHeap.length > 0) {
        eventHeap[0] = last;
        siftDown(0);
    }
    return top;
}

function siftUp(index) {
    while (index > 0) {
        var parent = Math.floor((index - 1) / 2);
        if (compareEvents(eventHeap[index], eventHeap[parent]) >= 0) {
            break;
        }
        swap(index, parent);
        index = parent;
    }
}

function siftDown(index) {
    var length = eventHeap.length;
    while (true) {
        var left = 2 * index + 1;
        var right = 2 * index + 2;
        var smallest = index;

        if (left < length && compareEvents(eventHeap[left], eventHeap[smallest]) < 0) {
            smallest = left;
        }
        if (right < length && compareEvents(eventHeap[right], eventHeap[smallest]) < 0) {
            smallest = right;
        }
        if (smallest === index) {
            break;
        }
        swap(index, smallest);
        index = smallest;
    }
}

function compareEvents(a, b) {
    if (a.timeMs === b.timeMs) {
        return a.seq - b.seq;
    }
    return a.timeMs - b.timeMs;
}

function swap(i, j) {
    var tmp = eventHeap[i];
    eventHeap[i] = eventHeap[j];
    eventHeap[j] = tmp;
}

function scheduleNextDispatch() {
    // Don't reschedule while dispatch is running - it will reschedule itself when done
    if (isDispatching) {
        return;
    }

    var wasTimerSet = nextTimer !== null;
    if (nextTimer) {
        clearTimeout(nextTimer);
        nextTimer = null;
    }

    if (eventHeap.length === 0) {
        return;
    }

    var now = performance.now();
    var nextEvent = heapPeek();
    var delay = nextEvent.timeMs - now;

    if (delay <= DISPATCH_LEEWAY_MS) {
        if (wasTimerSet) {
            console.log(`[PreScheduler] Timer was active, now dispatching immediately (${delay.toFixed(2)}ms early/late)`);
        }
        dispatchDueEvents();
    } else {
        nextTimer = setTimeout(dispatchDueEvents, delay);
        if (wasTimerSet) {
            console.log(`[PreScheduler] Rescheduled timer for +${delay.toFixed(0)}ms`);
        }
    }
}

function dispatchDueEvents() {
    nextTimer = null;
    isDispatching = true;  // Set flag to prevent reentrancy

    var now = performance.now();
    var dispatchCount = 0;
    var dispatchStart = now;

    while (eventHeap.length > 0) {
        var nextEvent = heapPeek();
        if (nextEvent.timeMs - now > DISPATCH_LEEWAY_MS) {
            break;
        }

        heapPop();
        stats.eventsPending = eventHeap.length;
        var lateness = now - nextEvent.timeMs;
        if (lateness < 0) {
            lateness = 0;
        }
        stats.totalDispatches++;
        stats.totalLateDispatchMs += lateness;
        if (lateness > stats.maxLateDispatchMs) {
            stats.maxLateDispatchMs = lateness;
        }
        sendToWriter(nextEvent.oscData);
        dispatchCount++;
    }

    var dispatchDuration = performance.now() - dispatchStart;
    if (dispatchCount > 0) {
        console.log(`[PreScheduler] Dispatched ${dispatchCount} events in ${dispatchDuration.toFixed(2)}ms, ${eventHeap.length} remaining`);
    }

    isDispatching = false;  // Clear flag before rescheduling
    scheduleNextDispatch();
}

function cancelBy(predicate) {
    if (eventHeap.length === 0) {
        return;
    }

    var before = eventHeap.length;
    var remaining = [];

    for (var i = 0; i < eventHeap.length; i++) {
        var event = eventHeap[i];
        if (!predicate(event)) {
            remaining.push(event);
        }
    }

    var removed = before - remaining.length;
    if (removed > 0) {
        eventHeap = remaining;
        heapify();
        stats.eventsCancelled += removed;
        stats.eventsPending = eventHeap.length;
        scheduleNextDispatch();
    }
}

function heapify() {
    for (var i = Math.floor(eventHeap.length / 2) - 1; i >= 0; i--) {
        siftDown(i);
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
    if (eventHeap.length === 0) {
        return;
    }
    stats.eventsCancelled += eventHeap.length;
    eventHeap = [];
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
                var sendStart = performance.now();

                // Handle both old (waitTimeMs) and new (audioTimeS) formats for backwards compatibility
                var waitTimeMs = data.waitTimeMs;

                if (data.audioTimeS !== null && data.audioTimeS !== undefined && data.currentTimeS !== null && data.currentTimeS !== undefined) {
                    // New format: calculate wait time from audio times
                    var deltaS = data.audioTimeS - data.currentTimeS;
                    var lookaheadS = 0.100; // 100ms lookahead (web-audio-scheduler default)
                    waitTimeMs = (deltaS - lookaheadS) * 1000;

                    // Ensure we don't schedule in the past
                    if (waitTimeMs < 0) {
                        waitTimeMs = 0;
                    }
                }

                if (waitTimeMs === null || waitTimeMs === undefined || waitTimeMs <= 0) {
                    processImmediate(data.oscData);
                } else {
                    scheduleEvent(
                        waitTimeMs,
                        data.editorId || 0,
                        data.runTag || '',
                        data.oscData
                    );
                }
                var sendDuration = performance.now() - sendStart;
                stats.totalSendTasks++;
                stats.totalSendProcessMs += sendDuration;
                if (sendDuration > stats.maxSendProcessMs) {
                    stats.maxSendProcessMs = sendDuration;
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
                        eventsCancelled: stats.eventsCancelled,
                        totalDispatches: stats.totalDispatches,
                        totalLateDispatchMs: stats.totalLateDispatchMs,
                        maxLateDispatchMs: stats.maxLateDispatchMs,
                        totalSendTasks: stats.totalSendTasks,
                        totalSendProcessMs: stats.totalSendProcessMs,
                        maxSendProcessMs: stats.maxSendProcessMs
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

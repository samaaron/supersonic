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
// Entries: { ntpTime, seq, editorId, runTag, oscData }
var eventHeap = [];
var periodicTimer = null;    // Single periodic timer (25ms interval)
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

// Timing constants
var NTP_EPOCH_OFFSET = 2208988800;  // Seconds from 1900-01-01 to 1970-01-01
var POLL_INTERVAL_MS = 25;           // Check every 25ms
var LOOKAHEAD_S = 0.100;             // 100ms lookahead window

function schedulerLog() {
    // Toggle to true for verbose diagnostics
    var DEBUG = true;  // Enable for debugging
    if (DEBUG) {
        console.log.apply(console, arguments);
    }
}

// ============================================================================
// NTP TIME HELPERS
// ============================================================================

/**
 * Get current NTP time from system clock
 *
 * Bundles contain full NTP timestamps. We just need to compare them against
 * current NTP time (system clock) to know when to dispatch.
 *
 * AudioContext timing, drift correction, etc. are handled by the C++ side.
 * The prescheduler only needs to know "what time is it now in NTP?"
 */
function getCurrentNTP() {
    // Convert current system time to NTP
    var perfTimeMs = performance.timeOrigin + performance.now();
    return (perfTimeMs / 1000) + NTP_EPOCH_OFFSET;
}

/**
 * Extract NTP timestamp from OSC bundle
 * Returns NTP time in seconds (double), or null if not a bundle
 */
function extractNTPFromBundle(oscData) {
    if (oscData.length >= 16 && oscData[0] === 0x23) {  // '#bundle'
        var view = new DataView(oscData.buffer, oscData.byteOffset);
        var ntpSeconds = view.getUint32(8, false);
        var ntpFraction = view.getUint32(12, false);
        return ntpSeconds + ntpFraction / 0x100000000;
    }
    return null;
}

/**
 * Legacy wrapper for backwards compatibility
 */
function getBundleTimestamp(oscMessage) {
    return extractNTPFromBundle(oscMessage);
}

// ============================================================================
// SHARED ARRAY BUFFER ACCESS
// ============================================================================

/**
 * Initialize SharedArrayBuffer (kept for compatibility but not used for timing)
 */
function initSharedBuffer() {
    if (!sharedBuffer || !bufferConstants) {
        console.error('[PreScheduler] Cannot init - missing buffer or constants');
        return;
    }

    console.log('[PreScheduler] SharedArrayBuffer initialized');
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

/**
 * Schedule an OSC bundle by its NTP timestamp
 * Non-bundles or bundles without timestamps are dispatched immediately
 */
function scheduleEvent(oscData, editorId, runTag) {
    var ntpTime = extractNTPFromBundle(oscData);

    if (ntpTime === null) {
        // Not a bundle - dispatch immediately to writer
        schedulerLog('[PreScheduler] Non-bundle message, dispatching immediately');
        sendToWriter(oscData);
        return;
    }

    var currentNTP = getCurrentNTP();
    var timeUntilExec = ntpTime - currentNTP;

    // Create event with NTP timestamp
    var event = {
        ntpTime: ntpTime,
        seq: sequenceCounter++,
        editorId: editorId || 0,
        runTag: runTag || '',
        oscData: oscData
    };

    heapPush(event);

    stats.bundlesScheduled++;
    stats.eventsPending = eventHeap.length;
    if (stats.eventsPending > stats.maxEventsPending) {
        stats.maxEventsPending = stats.eventsPending;
    }

    schedulerLog('[PreScheduler] Scheduled bundle:',
                 'NTP=' + ntpTime.toFixed(3),
                 'current=' + currentNTP.toFixed(3),
                 'wait=' + (timeUntilExec * 1000).toFixed(1) + 'ms',
                 'pending=' + stats.eventsPending);
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
    if (a.ntpTime === b.ntpTime) {
        return a.seq - b.seq;
    }
    return a.ntpTime - b.ntpTime;
}

function swap(i, j) {
    var tmp = eventHeap[i];
    eventHeap[i] = eventHeap[j];
    eventHeap[j] = tmp;
}

/**
 * Start periodic polling (called once on init)
 */
function startPeriodicPolling() {
    if (periodicTimer !== null) {
        console.warn('[PreScheduler] Polling already started');
        return;
    }

    console.log('[PreScheduler] Starting periodic polling (every ' + POLL_INTERVAL_MS + 'ms)');
    checkAndDispatch();  // Start immediately
}

/**
 * Stop periodic polling
 */
function stopPeriodicPolling() {
    if (periodicTimer !== null) {
        clearTimeout(periodicTimer);
        periodicTimer = null;
        console.log('[PreScheduler] Stopped periodic polling');
    }
}

/**
 * Periodic check and dispatch function
 * Uses NTP timestamps and global offset for drift-free timing
 */
function checkAndDispatch() {
    isDispatching = true;

    var currentNTP = getCurrentNTP();
    var lookaheadTime = currentNTP + LOOKAHEAD_S;
    var dispatchCount = 0;
    var dispatchStart = performance.now();

    // Dispatch all bundles that are ready
    while (eventHeap.length > 0) {
        var nextEvent = heapPeek();

        if (nextEvent.ntpTime <= lookaheadTime) {
            // Ready to dispatch
            heapPop();
            stats.eventsPending = eventHeap.length;

            var timeUntilExec = nextEvent.ntpTime - currentNTP;
            stats.totalDispatches++;

            schedulerLog('[PreScheduler] Dispatching bundle:',
                        'NTP=' + nextEvent.ntpTime.toFixed(3),
                        'current=' + currentNTP.toFixed(3),
                        'early=' + (timeUntilExec * 1000).toFixed(1) + 'ms',
                        'remaining=' + stats.eventsPending);

            sendToWriter(nextEvent.oscData);
            dispatchCount++;
        } else {
            // Rest aren't ready yet (heap is sorted)
            break;
        }
    }

    if (dispatchCount > 0 || eventHeap.length > 0) {
        schedulerLog('[PreScheduler] Dispatch cycle complete:',
                    'dispatched=' + dispatchCount,
                    'pending=' + eventHeap.length);
    }

    isDispatching = false;

    // Reschedule for next check (fixed interval)
    periodicTimer = setTimeout(checkAndDispatch, POLL_INTERVAL_MS);
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
        console.log('[PreScheduler] Cancelled ' + removed + ' events, ' + eventHeap.length + ' remaining');
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
    var cancelled = eventHeap.length;
    stats.eventsCancelled += cancelled;
    eventHeap = [];
    stats.eventsPending = 0;
    console.log('[PreScheduler] Cancelled all ' + cancelled + ' events');
    // Note: Periodic timer continues running (it will just find empty queue)
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

                // Initialize SharedArrayBuffer views (including offset)
                initSharedBuffer();

                // Start periodic polling
                startPeriodicPolling();

                schedulerLog('[OSCPreSchedulerWorker] Initialized with NTP-based scheduling');
                self.postMessage({ type: 'initialized' });
                break;

            case 'setWriterWorker':
                writerWorker = data.port;
                schedulerLog('[OSCPreSchedulerWorker] Writer worker connected');
                break;

            case 'send':
                var sendStart = performance.now();

                // New NTP-based scheduling: extract NTP from bundle
                // scheduleEvent() will dispatch immediately if not a bundle
                scheduleEvent(
                    data.oscData,
                    data.editorId || 0,
                    data.runTag || ''
                );

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

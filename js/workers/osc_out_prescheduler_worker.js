/*
    SuperSonic - OSC Pre-Scheduler Worker
    Ports the Bleep pre-scheduler design:
    - Single priority queue of future bundles/events
    - One timer driving dispatch (no per-event setTimeout storm)
    - Tag-based cancellation to drop pending runs before they hit WASM
*/

import * as MetricsOffsets from '../lib/metrics_offsets.js';
import { writeToRingBuffer } from '../lib/ring_buffer_writer.js';

// Shared memory for ring buffer writing
let sharedBuffer = null;
let ringBufferBase = null;
let bufferConstants = null;
let atomicView = null;
let dataView = null;
let uint8View = null;

// Ring buffer control indices
let CONTROL_INDICES = {};

// Metrics view (for writing stats to SAB)
let metricsView = null;

// Priority queue implemented as binary min-heap
// Entries: { ntpTime, seq, editorId, runTag, oscData }
let eventHeap = [];
let periodicTimer = null;    // Single periodic timer (25ms interval)
let sequenceCounter = 0;
let isDispatching = false;  // Prevent reentrancy into dispatch loop

// Message sequence counter now lives in SAB at CONTROL_INDICES.IN_SEQUENCE
// Shared between prescheduler worker and main thread (for direct writes)

// Retry queue for failed writes
let retryQueue = [];
const MAX_RETRIES_PER_MESSAGE = 5;

// Backpressure: max total pending messages (heap + retry queue combined)
// Default 65536, can be overridden via init config
let maxPendingMessages = 65536;

// Timing constants
const NTP_EPOCH_OFFSET = 2208988800;  // Seconds from 1900-01-01 to 1970-01-01
const POLL_INTERVAL_MS = 25;           // Check every 25ms
const LOOKAHEAD_S = 0.200;             // 200ms lookahead window

const schedulerLog = (...args) => {
    if (__DEV__) {
        console.log(...args);
    }
};

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
const getCurrentNTP = () => {
    // Convert current system time to NTP
    const perfTimeMs = performance.timeOrigin + performance.now();
    return (perfTimeMs / 1000) + NTP_EPOCH_OFFSET;
};

/**
 * Extract NTP timestamp from OSC bundle
 * Returns NTP time in seconds (double), or null if not a bundle
 */
const extractNTPFromBundle = (oscData) => {
    if (oscData.length >= 16 && oscData[0] === 0x23) {  // '#bundle'
        const view = new DataView(oscData.buffer, oscData.byteOffset);
        const ntpSeconds = view.getUint32(8, false);
        const ntpFraction = view.getUint32(12, false);
        return ntpSeconds + ntpFraction / 0x100000000;
    }
    return null;
};

/**
 * Legacy wrapper for backwards compatibility
 */
const getBundleTimestamp = (oscMessage) => extractNTPFromBundle(oscMessage);

// ============================================================================
// SHARED ARRAY BUFFER ACCESS
// ============================================================================

/**
 * Initialize ring buffer access for writing directly to SharedArrayBuffer
 */
const initSharedBuffer = () => {
    if (!sharedBuffer || !bufferConstants) {
        console.error('[PreScheduler] Cannot init - missing buffer or constants');
        return;
    }

    atomicView = new Int32Array(sharedBuffer);
    dataView = new DataView(sharedBuffer);
    uint8View = new Uint8Array(sharedBuffer);

    // Calculate control indices for ring buffer
    // Offsets: in_head=0, in_tail=4, out_head=8, out_tail=12, debug_head=16, debug_tail=20,
    //          in_sequence=24, out_sequence=28, debug_sequence=32, status_flags=36, in_write_lock=40
    CONTROL_INDICES = {
        IN_HEAD: (ringBufferBase + bufferConstants.CONTROL_START + 0) / 4,
        IN_TAIL: (ringBufferBase + bufferConstants.CONTROL_START + 4) / 4,
        IN_SEQUENCE: (ringBufferBase + bufferConstants.CONTROL_START + 24) / 4,
        IN_WRITE_LOCK: (ringBufferBase + bufferConstants.CONTROL_START + 40) / 4
    };

    // Initialize metrics view
    const metricsBase = ringBufferBase + bufferConstants.METRICS_START;
    metricsView = new Uint32Array(sharedBuffer, metricsBase, bufferConstants.METRICS_SIZE / 4);

    schedulerLog('[PreScheduler] SharedArrayBuffer initialized with direct ring buffer writing and metrics');
};

/**
 * Write metrics to SharedArrayBuffer
 * Increments use Atomics.add() for thread safety, stores use Atomics.store()
 */
const updateMetrics = () => {
    if (!metricsView) return;

    // Update current values (use Atomics.store for absolute values)
    Atomics.store(metricsView, MetricsOffsets.PRESCHEDULER_PENDING, eventHeap.length);

    // Update max if current exceeds it
    const currentPending = eventHeap.length;
    const currentMax = Atomics.load(metricsView, MetricsOffsets.PRESCHEDULER_PEAK);
    if (currentPending > currentMax) {
        Atomics.store(metricsView, MetricsOffsets.PRESCHEDULER_PEAK, currentPending);
    }
};

/**
 * Write OSC message to ring buffer using shared module
 * Returns true if successful, false if failed (caller should queue for retry)
 */
const writeOSCToRingBuffer = (oscMessage, isRetry) => {
    if (!sharedBuffer || !atomicView) {
        console.error('[PreScheduler] Not initialized for ring buffer writing');
        return false;
    }

    const payloadSize = oscMessage.length;
    const totalSize = bufferConstants.MESSAGE_HEADER_SIZE + payloadSize;

    // Check if message fits in buffer at all
    if (totalSize > bufferConstants.IN_BUFFER_SIZE - bufferConstants.MESSAGE_HEADER_SIZE) {
        console.error('[PreScheduler] Message too large:', totalSize);
        return false;
    }

    // Use shared ring buffer writer (worker can spin briefly for lock)
    const success = writeToRingBuffer({
        atomicView,
        dataView,
        uint8View,
        bufferConstants,
        ringBufferBase,
        controlIndices: CONTROL_INDICES,
        oscMessage,
        maxSpins: 10  // Worker can afford brief spinning
    });

    if (!success) {
        // Buffer full - return false so caller can queue for retry
        if (!isRetry) {
            console.warn('[PreScheduler] Ring buffer full, message will be queued for retry');
        }
        return false;
    }

    // Update SAB metrics
    if (metricsView) Atomics.add(metricsView, MetricsOffsets.PRESCHEDULER_SENT, 1);
    return true;
};

/**
 * Add a message to the retry queue
 */
const queueForRetry = (oscData, context) => {
    // Use same holistic limit as scheduleEvent
    const totalPending = eventHeap.length + retryQueue.length;
    if (totalPending >= maxPendingMessages) {
        console.error('[PreScheduler] Backpressure: dropping retry (' + totalPending + ' pending)');
        if (metricsView) Atomics.add(metricsView, MetricsOffsets.RETRIES_FAILED, 1);
        return;
    }

    retryQueue.push({
        oscData,
        retryCount: 0,
        context: context || 'unknown',
        queuedAt: performance.now()
    });

    // Update SAB metrics
    if (metricsView) {
        Atomics.store(metricsView, MetricsOffsets.RETRY_QUEUE_SIZE, retryQueue.length);
        const currentMax = Atomics.load(metricsView, MetricsOffsets.RETRY_QUEUE_MAX);
        if (retryQueue.length > currentMax) {
            Atomics.store(metricsView, MetricsOffsets.RETRY_QUEUE_MAX, retryQueue.length);
        }
    }

    schedulerLog('[PreScheduler] Queued message for retry:', context, 'queue size:', retryQueue.length);
};

/**
 * Attempt to retry queued messages
 * Called periodically from checkAndDispatch
 */
const processRetryQueue = () => {
    if (retryQueue.length === 0) {
        return;
    }

    let i = 0;
    while (i < retryQueue.length) {
        const item = retryQueue[i];

        // Try to write
        const success = writeOSCToRingBuffer(item.oscData, true);

        if (success) {
            // Success - remove from queue
            retryQueue.splice(i, 1);
            if (metricsView) {
                Atomics.add(metricsView, MetricsOffsets.RETRIES_SUCCEEDED, 1);
                Atomics.add(metricsView, MetricsOffsets.MESSAGES_RETRIED, 1);
                Atomics.store(metricsView, MetricsOffsets.RETRY_QUEUE_SIZE, retryQueue.length);
            }
            schedulerLog('[PreScheduler] Retry succeeded for:', item.context,
                        'after', item.retryCount + 1, 'attempts');
            // Don't increment i - we removed an item
        } else {
            // Failed - increment retry count
            item.retryCount++;
            if (metricsView) Atomics.add(metricsView, MetricsOffsets.MESSAGES_RETRIED, 1);

            if (item.retryCount >= MAX_RETRIES_PER_MESSAGE) {
                // Give up on this message
                const errorMsg = `Ring buffer full - dropped message after ${MAX_RETRIES_PER_MESSAGE} retries (${item.context})`;
                console.error('[PreScheduler]', errorMsg);
                retryQueue.splice(i, 1);
                if (metricsView) {
                    Atomics.add(metricsView, MetricsOffsets.RETRIES_FAILED, 1);
                    Atomics.store(metricsView, MetricsOffsets.RETRY_QUEUE_SIZE, retryQueue.length);
                }
                // Notify main thread so onError callback fires
                self.postMessage({ type: 'error', error: errorMsg });
                // Don't increment i - we removed an item
            } else {
                // Keep in queue, try again next cycle
                i++;
            }
        }
    }
};

/**
 * Schedule an OSC bundle by its NTP timestamp
 * Non-bundles or bundles without timestamps are dispatched immediately
 * Returns false if rejected due to backpressure
 */
const scheduleEvent = (oscData, editorId, runTag) => {
    // Backpressure: reject if total pending work exceeds limit
    const totalPending = eventHeap.length + retryQueue.length;
    if (totalPending >= maxPendingMessages) {
        console.warn('[PreScheduler] Backpressure: rejecting message (' + totalPending + ' pending)');
        return false;
    }

    const ntpTime = extractNTPFromBundle(oscData);

    if (ntpTime === null) {
        // Not a bundle - dispatch immediately to ring buffer
        schedulerLog('[PreScheduler] Non-bundle message, dispatching immediately');
        const success = writeOSCToRingBuffer(oscData, false);
        if (!success) {
            // Queue for retry
            queueForRetry(oscData, 'immediate message');
        }
        return true;
    }

    const currentNTP = getCurrentNTP();
    const timeUntilExec = ntpTime - currentNTP;

    // Create event with NTP timestamp
    const event = {
        ntpTime,
        seq: sequenceCounter++,
        editorId: editorId || 0,
        runTag: runTag || '',
        oscData
    };

    heapPush(event);

    if (metricsView) Atomics.add(metricsView, MetricsOffsets.BUNDLES_SCHEDULED, 1);
    updateMetrics();  // Update SAB with current queue depth and peak

    schedulerLog('[PreScheduler] Scheduled bundle:',
                 'NTP=' + ntpTime.toFixed(3),
                 'current=' + currentNTP.toFixed(3),
                 'wait=' + (timeUntilExec * 1000).toFixed(1) + 'ms',
                 'pending=' + eventHeap.length);

    return true;
};

const heapPush = (event) => {
    eventHeap.push(event);
    siftUp(eventHeap.length - 1);
};

const heapPeek = () => eventHeap.length > 0 ? eventHeap[0] : null;

const heapPop = () => {
    if (eventHeap.length === 0) {
        return null;
    }
    const top = eventHeap[0];
    const last = eventHeap.pop();
    if (eventHeap.length > 0) {
        eventHeap[0] = last;
        siftDown(0);
    }
    return top;
};

const siftUp = (index) => {
    while (index > 0) {
        const parent = Math.floor((index - 1) / 2);
        if (compareEvents(eventHeap[index], eventHeap[parent]) >= 0) {
            break;
        }
        swap(index, parent);
        index = parent;
    }
};

const siftDown = (index) => {
    const length = eventHeap.length;
    while (true) {
        const left = 2 * index + 1;
        const right = 2 * index + 2;
        let smallest = index;

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
};

const compareEvents = (a, b) => {
    if (a.ntpTime === b.ntpTime) {
        return a.seq - b.seq;
    }
    return a.ntpTime - b.ntpTime;
};

const swap = (i, j) => {
    const tmp = eventHeap[i];
    eventHeap[i] = eventHeap[j];
    eventHeap[j] = tmp;
};

/**
 * Start periodic polling (called once on init)
 */
const startPeriodicPolling = () => {
    if (periodicTimer !== null) {
        console.warn('[PreScheduler] Polling already started');
        return;
    }

    schedulerLog('[PreScheduler] Starting periodic polling (every ' + POLL_INTERVAL_MS + 'ms)');
    checkAndDispatch();  // Start immediately
};

/**
 * Stop periodic polling
 */
const stopPeriodicPolling = () => {
    if (periodicTimer !== null) {
        clearTimeout(periodicTimer);
        periodicTimer = null;
        schedulerLog('[PreScheduler] Stopped periodic polling');
    }
};

/**
 * Periodic check and dispatch function
 * Uses NTP timestamps and global offset for drift-free timing
 */
const checkAndDispatch = () => {
    isDispatching = true;

    // First, try to process any queued retries
    processRetryQueue();

    const currentNTP = getCurrentNTP();
    const lookaheadTime = currentNTP + LOOKAHEAD_S;
    let dispatchCount = 0;

    // Dispatch all bundles that are ready
    while (eventHeap.length > 0) {
        const nextEvent = heapPeek();

        if (nextEvent.ntpTime <= lookaheadTime) {
            // Ready to dispatch
            heapPop();
            updateMetrics();  // Update SAB with current queue depth

            const timeUntilExec = nextEvent.ntpTime - currentNTP;
            if (metricsView) Atomics.add(metricsView, MetricsOffsets.TOTAL_DISPATCHES, 1);

            schedulerLog('[PreScheduler] Dispatching bundle:',
                        'NTP=' + nextEvent.ntpTime.toFixed(3),
                        'current=' + currentNTP.toFixed(3),
                        'early=' + (timeUntilExec * 1000).toFixed(1) + 'ms',
                        'remaining=' + eventHeap.length);

            const success = writeOSCToRingBuffer(nextEvent.oscData, false);
            if (!success) {
                // Queue for retry
                queueForRetry(nextEvent.oscData, 'scheduled bundle NTP=' + nextEvent.ntpTime.toFixed(3));
            }
            dispatchCount++;
        } else {
            // Rest aren't ready yet (heap is sorted)
            break;
        }
    }

    if (dispatchCount > 0 || eventHeap.length > 0 || retryQueue.length > 0) {
        schedulerLog('[PreScheduler] Dispatch cycle complete:',
                    'dispatched=' + dispatchCount,
                    'pending=' + eventHeap.length,
                    'retrying=' + retryQueue.length);
    }

    isDispatching = false;

    // Reschedule for next check (fixed interval)
    periodicTimer = setTimeout(checkAndDispatch, POLL_INTERVAL_MS);
};

const cancelBy = (predicate) => {
    if (eventHeap.length === 0) {
        return;
    }

    const before = eventHeap.length;
    const remaining = [];

    for (let i = 0; i < eventHeap.length; i++) {
        const event = eventHeap[i];
        if (!predicate(event)) {
            remaining.push(event);
        }
    }

    const removed = before - remaining.length;
    if (removed > 0) {
        eventHeap = remaining;
        heapify();
        if (metricsView) Atomics.add(metricsView, MetricsOffsets.EVENTS_CANCELLED, removed);
        updateMetrics();  // Update SAB with current queue depth
        schedulerLog('[PreScheduler] Cancelled ' + removed + ' events, ' + eventHeap.length + ' remaining');
    }
};

const heapify = () => {
    for (let i = Math.floor(eventHeap.length / 2) - 1; i >= 0; i--) {
        siftDown(i);
    }
};

const cancelEditorTag = (editorId, runTag) => {
    cancelBy((event) => event.editorId === editorId && event.runTag === runTag);
};

const cancelEditor = (editorId) => {
    cancelBy((event) => event.editorId === editorId);
};

const cancelAllTags = () => {
    if (eventHeap.length === 0) {
        return;
    }
    const cancelled = eventHeap.length;
    if (metricsView) Atomics.add(metricsView, MetricsOffsets.EVENTS_CANCELLED, cancelled);
    eventHeap = [];
    updateMetrics();  // Update SAB (sets eventsPending to 0)
    schedulerLog('[PreScheduler] Cancelled all ' + cancelled + ' events');
    // Note: Periodic timer continues running (it will just find empty queue)
};

// Helpers reused from legacy worker for immediate send
const isBundle = (data) => {
    if (!data || data.length < 8) {
        return false;
    }
    return data[0] === 0x23 && data[1] === 0x62 && data[2] === 0x75 && data[3] === 0x6e &&
        data[4] === 0x64 && data[5] === 0x6c && data[6] === 0x65 && data[7] === 0x00;
};

const extractMessagesFromBundle = (data) => {
    const messages = [];
    const view = new DataView(data.buffer, data.byteOffset, data.byteLength);
    let offset = 16; // skip "#bundle\0" + timetag

    while (offset < data.length) {
        const messageSize = view.getInt32(offset, false);
        offset += 4;

        if (messageSize <= 0 || offset + messageSize > data.length) {
            break;
        }

        const messageData = data.slice(offset, offset + messageSize);
        messages.push(messageData);
        offset += messageSize;

        while (offset % 4 !== 0 && offset < data.length) {
            offset++;
        }
    }

    return messages;
};

const processImmediate = (oscData) => {
    if (isBundle(oscData)) {
        const messages = extractMessagesFromBundle(oscData);
        for (let i = 0; i < messages.length; i++) {
            const success = writeOSCToRingBuffer(messages[i], false);
            if (!success) {
                queueForRetry(messages[i], 'immediate bundle message ' + i);
            }
        }
    } else {
        const success = writeOSCToRingBuffer(oscData, false);
        if (!success) {
            queueForRetry(oscData, 'immediate message');
        }
    }
};

// Message handling
self.addEventListener('message', (event) => {
    const { data } = event;

    try {
        switch (data.type) {
            case 'init':
                sharedBuffer = data.sharedBuffer;
                ringBufferBase = data.ringBufferBase;
                bufferConstants = data.bufferConstants;

                // Apply config overrides
                if (data.maxPendingMessages) {
                    maxPendingMessages = data.maxPendingMessages;
                }

                // Initialize SharedArrayBuffer views (including offset)
                initSharedBuffer();

                // Start periodic polling
                startPeriodicPolling();

                schedulerLog('[OSCPreSchedulerWorker] Initialized with NTP-based scheduling, capacity=' + maxPendingMessages);
                self.postMessage({ type: 'initialized' });
                break;

            case 'send':
                // NTP-based scheduling: extract NTP from bundle
                // scheduleEvent() will dispatch immediately if not a bundle
                scheduleEvent(
                    data.oscData,
                    data.editorId || 0,
                    data.runTag || ''
                );
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
});

schedulerLog('[OSCPreSchedulerWorker] Script loaded');

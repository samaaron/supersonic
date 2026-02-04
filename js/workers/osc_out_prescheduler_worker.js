// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2025 Sam Aaron

import * as MetricsOffsets from '../lib/metrics_offsets.js';
import { writeToRingBuffer } from '../lib/ring_buffer_writer.js';
import { calculateInControlIndices } from '../lib/control_offsets.js';

// Transport mode: 'sab' or 'postMessage'
let mode = 'sab';

// Direct port to worklet (postMessage mode only)
let workletPort = null;

// Limits to prevent resource exhaustion
// Scheduler slot size - bundles larger than this can't be scheduled (WASM limitation)
let schedulerSlotSize = 1024;  // Default, updated from bufferConstants if available

// Maximum individual OSC message size (must match SC_Stubs.cpp limit)
// This is different from schedulerSlotSize - this limits messages within bundles
const MAX_OSC_MESSAGE_SIZE = 65536;

// Maximum time in the future a bundle can be scheduled (1 hour in seconds)
// Bundles scheduled further ahead are rejected to prevent scheduler queue buildup
const MAX_FUTURE_SCHEDULE_SECONDS = 3600;

// Sentinel value for "unset" min headroom metric (allows 0ms to be valid)
const HEADROOM_UNSET_SENTINEL = 0xFFFFFFFF;

// Shared memory for ring buffer writing (SAB mode only)
let sharedBuffer = null;
let ringBufferBase = null;
let bufferConstants = null;
let atomicView = null;
let dataView = null;
let uint8View = null;

// Ring buffer control indices (SAB mode only)
let CONTROL_INDICES = {};

// Metrics view (for writing stats)
// SAB mode: view into SharedArrayBuffer
// postMessage mode: view into local ArrayBuffer (sent periodically)
let metricsView = null;
let localMetricsBuffer = null;  // postMessage mode only - local buffer to send

// Metrics send timer (postMessage mode only)
let metricsSendTimer = null;
let metricsSendIntervalMs = 150;  // Default, can be overridden by snapshotIntervalMs config

// ============================================================================
// METRICS HELPERS (work in both SAB and postMessage modes)
// ============================================================================

/**
 * Store a value at a metrics offset
 * Uses Atomics in SAB mode, direct access in postMessage mode
 */
const metricsStore = (offset, value) => {
    if (!metricsView) return;
    if (mode === 'sab') {
        Atomics.store(metricsView, offset, value);
    } else {
        metricsView[offset] = value;
    }
};

/**
 * Load a value from a metrics offset
 * Uses Atomics in SAB mode, direct access in postMessage mode
 */
const metricsLoad = (offset) => {
    if (!metricsView) return 0;
    if (mode === 'sab') {
        return Atomics.load(metricsView, offset);
    } else {
        return metricsView[offset];
    }
};

/**
 * Add to a value at a metrics offset
 * Uses Atomics in SAB mode, direct access in postMessage mode
 */
const metricsAdd = (offset, value) => {
    if (!metricsView) return;
    if (mode === 'sab') {
        Atomics.add(metricsView, offset, value);
    } else {
        metricsView[offset] += value;
    }
};

/**
 * Set a value at a metrics offset (for gauges)
 * Uses Atomics in SAB mode, direct access in postMessage mode
 */
const metricsSet = (offset, value) => {
    if (!metricsView) return;
    if (mode === 'sab') {
        Atomics.store(metricsView, offset, value);
    } else {
        metricsView[offset] = value;
    }
};

/**
 * Get a value at a metrics offset
 */
const metricsGet = (offset) => {
    if (!metricsView) return 0;
    if (mode === 'sab') {
        return Atomics.load(metricsView, offset);
    } else {
        return metricsView[offset];
    }
};

/**
 * Start periodic sending of metrics buffer to main thread (postMessage mode only)
 */
const startMetricsSending = () => {
    if (mode !== 'postMessage' || metricsSendTimer !== null) return;

    const sendMetrics = () => {
        if (localMetricsBuffer && metricsView) {
            // Send a copy of the metrics buffer
            self.postMessage({
                type: 'preschedulerMetrics',
                metrics: new Uint32Array(localMetricsBuffer.slice(0))
            });
        }
        metricsSendTimer = setTimeout(sendMetrics, metricsSendIntervalMs);
    };

    sendMetrics();
    schedulerLog('[PreScheduler] Started metrics sending (every ' + metricsSendIntervalMs + 'ms)');
};

/**
 * Stop periodic sending of metrics (postMessage mode only)
 */
const stopMetricsSending = () => {
    if (metricsSendTimer !== null) {
        clearTimeout(metricsSendTimer);
        metricsSendTimer = null;
    }
};

// Priority queue implemented as binary min-heap
// Entries: { ntpTime, seq, sessionId, runTag, oscData, sourceId }
let eventHeap = [];
let dispatchTimer = null;      // Demand-driven dispatch timer
let nextDispatchAt = Infinity; // NTP time the current timer targets
let sequenceCounter = 0;
let isDispatching = false;  // Prevent reentrancy into dispatch loop

// Message sequence counter now lives in SAB at CONTROL_INDICES.IN_SEQUENCE
// Shared between prescheduler worker and main thread (for direct writes)

// Retry queue for failed writes
let retryQueue = [];
let waitingForBufferSpace = false;

// Backpressure: max total pending messages (heap + retry queue combined)
// Default 65536, can be overridden via init config
let maxPendingMessages = 65536;

// Timing constants
const NTP_EPOCH_OFFSET = 2208988800;  // Seconds from 1900-01-01 to 1970-01-01
let lookaheadS = 0.500;                // Lookahead window (configurable via init)

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
    CONTROL_INDICES = calculateInControlIndices(ringBufferBase, bufferConstants.CONTROL_START);

    // Initialize metrics view
    const metricsBase = ringBufferBase + bufferConstants.METRICS_START;
    metricsView = new Uint32Array(sharedBuffer, metricsBase, bufferConstants.METRICS_SIZE / 4);

    schedulerLog('[PreScheduler] SharedArrayBuffer initialized with direct ring buffer writing and metrics');
};

/**
 * Write metrics to buffer (SAB or local)
 * Uses helper functions that work in both modes
 */
const updateMetrics = () => {
    if (!metricsView) return;

    // Update current values
    metricsStore(MetricsOffsets.PRESCHEDULER_PENDING, eventHeap.length);

    // Update max if current exceeds it
    const currentPending = eventHeap.length;
    const currentMax = metricsLoad(MetricsOffsets.PRESCHEDULER_PENDING_PEAK);
    if (currentPending > currentMax) {
        metricsStore(MetricsOffsets.PRESCHEDULER_PENDING_PEAK, currentPending);
    }
};

/**
 * Dispatch OSC message to WASM
 * In SAB mode: writes to ring buffer
 * In postMessage mode: sends directly to worklet via MessagePort
 * @param {Uint8Array} oscMessage - OSC message data
 * @param {boolean} isRetry - Whether this is a retry attempt (for logging)
 * @param {number} sourceId - Source ID for logging
 * @param {boolean} useWait - If true, use Atomics.wait() for guaranteed delivery (SAB mode only)
 * @returns {boolean} true if successful, false if failed (caller should queue for retry)
 */
const dispatchOSCMessage = (oscMessage, isRetry, sourceId = 0, useWait = false) => {
    if (mode === 'postMessage') {
        // PostMessage mode: send to worklet via MessagePort
        if (!workletPort) {
            console.error('[PreScheduler] No worklet port available');
            return false;
        }
        workletPort.postMessage({
            type: 'osc',
            oscData: oscMessage,
            sourceId,
        });
        metricsAdd(MetricsOffsets.PRESCHEDULER_DISPATCHED, 1);
        return true;  // postMessage doesn't fail
    }

    // SAB mode: write to ring buffer
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

    // Use shared ring buffer writer
    // useWait: true means use Atomics.wait() for guaranteed delivery (blocks until lock available)
    const success = writeToRingBuffer({
        atomicView,
        dataView,
        uint8View,
        bufferConstants,
        ringBufferBase,
        controlIndices: CONTROL_INDICES,
        oscMessage,
        sourceId,
        maxSpins: 10,  // Spin briefly first
        useWait,       // If true, block until lock available (guaranteed delivery)
    });

    if (!success) {
        // Buffer full - return false so caller can queue for retry
        if (!isRetry) {
            console.warn('[PreScheduler] Ring buffer full, message will be queued for retry');
        }
        return false;
    }

    // Update metrics
    metricsAdd(MetricsOffsets.PRESCHEDULER_DISPATCHED, 1);
    return true;
};

/**
 * Add a message to the retry queue
 */
const queueForRetry = (oscData, context, sourceId = 0) => {
    // Use same holistic limit as scheduleEvent
    const totalPending = eventHeap.length + retryQueue.length;
    if (totalPending >= maxPendingMessages) {
        console.error('[PreScheduler] Backpressure: dropping retry (' + totalPending + ' pending)');
        metricsAdd(MetricsOffsets.PRESCHEDULER_RETRIES_FAILED, 1);
        return;
    }

    retryQueue.push({
        oscData,
        context: context || 'unknown',
        queuedAt: performance.now(),
        sourceId
    });

    // Update metrics
    metricsStore(MetricsOffsets.PRESCHEDULER_RETRY_QUEUE_SIZE, retryQueue.length);
    const currentMax = metricsLoad(MetricsOffsets.PRESCHEDULER_RETRY_QUEUE_PEAK);
    if (retryQueue.length > currentMax) {
        metricsStore(MetricsOffsets.PRESCHEDULER_RETRY_QUEUE_PEAK, retryQueue.length);
    }

    schedulerLog('[PreScheduler] Queued message for retry:', context, 'queue size:', retryQueue.length);
    awaitBufferSpace();
};

/**
 * Wait for the worklet to consume from the IN buffer, then process retries.
 * Uses Atomics.waitAsync() for non-blocking, notification-driven waiting.
 */
const awaitBufferSpace = () => {
    if (waitingForBufferSpace || retryQueue.length === 0 || mode !== 'sab') return;

    waitingForBufferSpace = true;
    const currentTail = Atomics.load(atomicView, CONTROL_INDICES.IN_TAIL);
    const result = Atomics.waitAsync(atomicView, CONTROL_INDICES.IN_TAIL, currentTail);

    const onSpaceAvailable = () => {
        waitingForBufferSpace = false;
        processRetryQueue();
        if (retryQueue.length > 0) {
            awaitBufferSpace();
        }
    };

    if (result.async) {
        result.value.then(onSpaceAvailable);
    } else {
        // Value already changed — process on next microtask to avoid recursion
        queueMicrotask(onSpaceAvailable);
    }
};

/**
 * Attempt to retry queued messages
 * Called when buffer space becomes available (via Atomics.waitAsync notification)
 */
const processRetryQueue = () => {
    if (retryQueue.length === 0) return;

    let i = 0;
    while (i < retryQueue.length) {
        const item = retryQueue[i];
        const success = dispatchOSCMessage(item.oscData, true, item.sourceId, true);

        if (success) {
            retryQueue.splice(i, 1);
            metricsAdd(MetricsOffsets.PRESCHEDULER_RETRIES_SUCCEEDED, 1);
            metricsStore(MetricsOffsets.PRESCHEDULER_RETRY_QUEUE_SIZE, retryQueue.length);
            // Don't increment i — we removed an item
        } else {
            // Buffer still full — stop processing, wait for more space
            break;
        }
    }
};

/**
 * Schedule an OSC bundle by its NTP timestamp
 * Non-bundles or bundles without timestamps are dispatched immediately
 * Returns false if rejected due to backpressure
 */
const scheduleEvent = (oscData, sessionId, runTag, sourceId = 0) => {
    // Backpressure: reject if total pending work exceeds limit
    const totalPending = eventHeap.length + retryQueue.length;
    if (totalPending >= maxPendingMessages) {
        const errorMsg = `Prescheduler queue full (${totalPending} >= ${maxPendingMessages} max)`;
        console.warn('[PreScheduler]', errorMsg);
        self.postMessage({ type: 'error', error: errorMsg, code: 'PRESCHEDULER_QUEUE_FULL' });
        return false;
    }

    const ntpTime = extractNTPFromBundle(oscData);

    if (ntpTime === null) {
        // Not a bundle - dispatch immediately to ring buffer
        // Use useWait: true for guaranteed delivery (no lock contention drops)
        schedulerLog('[PreScheduler] Non-bundle message, dispatching immediately');
        const success = dispatchOSCMessage(oscData, false, sourceId, true);
        if (!success) {
            // Queue for retry (only fails if buffer genuinely full)
            queueForRetry(oscData, 'immediate message', sourceId);
        }
        return true;
    }

    const currentNTP = getCurrentNTP();
    const timeUntilExec = ntpTime - currentNTP;

    // Reject bundles too large for scheduler slot (WASM has fixed slot size)
    if (oscData.length > schedulerSlotSize) {
        const errorMsg = `Bundle too large for scheduler (${oscData.length} > ${schedulerSlotSize} bytes)`;
        console.warn('[PreScheduler]', errorMsg);
        self.postMessage({ type: 'error', error: errorMsg, code: 'BUNDLE_TOO_LARGE' });
        return false;
    }

    // Reject bundles scheduled too far in the future (prevents queue buildup)
    if (timeUntilExec > MAX_FUTURE_SCHEDULE_SECONDS) {
        const errorMsg = `Bundle scheduled too far in future (${timeUntilExec.toFixed(0)}s > ${MAX_FUTURE_SCHEDULE_SECONDS}s max)`;
        console.warn('[PreScheduler]', errorMsg);
        self.postMessage({ type: 'error', error: errorMsg, code: 'BUNDLE_TOO_FAR_FUTURE' });
        return false;
    }

    // Create event with NTP timestamp
    const event = {
        ntpTime,
        seq: sequenceCounter++,
        sessionId: sessionId || 0,
        runTag: runTag || '',
        oscData,
        sourceId
    };

    heapPush(event);

    metricsAdd(MetricsOffsets.PRESCHEDULER_BUNDLES_SCHEDULED, 1);
    updateMetrics();  // Update buffer with current queue depth and peak

    schedulerLog('[PreScheduler] Scheduled bundle:',
                 'NTP=' + ntpTime.toFixed(3),
                 'current=' + currentNTP.toFixed(3),
                 'wait=' + (timeUntilExec * 1000).toFixed(1) + 'ms',
                 'pending=' + eventHeap.length);

    rescheduleDispatch();
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
 * Reschedule the dispatch timer to target the next needed dispatch time.
 * Called after any state change (heap push/pop, cancel).
 * Retry queue is handled separately via Atomics.waitAsync notifications.
 * When idle (nothing pending), no timer runs.
 */
const rescheduleDispatch = () => {
    if (eventHeap.length === 0) {
        if (dispatchTimer !== null) {
            clearTimeout(dispatchTimer);
            dispatchTimer = null;
            nextDispatchAt = Infinity;
        }
        return;
    }

    const targetNTP = heapPeek().ntpTime - lookaheadS;
    const nowNTP = getCurrentNTP();

    if (targetNTP < nextDispatchAt) {
        if (dispatchTimer !== null) {
            clearTimeout(dispatchTimer);
        }
        const delayMs = Math.max(0, (targetNTP - nowNTP) * 1000);
        nextDispatchAt = targetNTP;
        dispatchTimer = setTimeout(checkAndDispatch, delayMs);
    }
};

/**
 * Start dispatching (called once on init)
 */
const startDispatching = () => {
    if (dispatchTimer !== null) {
        console.warn('[PreScheduler] Dispatching already started');
        return;
    }

    schedulerLog('[PreScheduler] Starting demand-driven dispatching');
    rescheduleDispatch();  // Sets timer if heap/retry queue non-empty, otherwise idle
};

/**
 * Stop dispatching
 */
const stopDispatching = () => {
    if (dispatchTimer !== null) {
        clearTimeout(dispatchTimer);
        dispatchTimer = null;
        nextDispatchAt = Infinity;
        schedulerLog('[PreScheduler] Stopped dispatching');
    }
    waitingForBufferSpace = false;
};

/**
 * Periodic check and dispatch function
 * Uses NTP timestamps and global offset for drift-free timing
 */
const checkAndDispatch = () => {
    isDispatching = true;

    const currentNTP = getCurrentNTP();
    const lookaheadTime = currentNTP + lookaheadS;
    let dispatchCount = 0;

    // Dispatch all bundles that are ready
    while (eventHeap.length > 0) {
        const nextEvent = heapPeek();

        if (nextEvent.ntpTime <= lookaheadTime) {
            // Ready to dispatch
            heapPop();
            updateMetrics();  // Update buffer with current queue depth

            const timeUntilExec = nextEvent.ntpTime - currentNTP;
            metricsAdd(MetricsOffsets.PRESCHEDULER_TOTAL_DISPATCHES, 1);

            // Track timing: headroom (ms before execution) or lates (dispatched after execution time)
            if (timeUntilExec < 0) {
                // Late dispatch - bundle arrived after its scheduled execution time
                const lateMs = Math.round(-timeUntilExec * 1000);
                metricsAdd(MetricsOffsets.PRESCHEDULER_LATES, 1);

                // Track max lateness
                const currentMaxLate = metricsGet(MetricsOffsets.PRESCHEDULER_MAX_LATE_MS);
                if (lateMs > currentMaxLate) {
                    metricsSet(MetricsOffsets.PRESCHEDULER_MAX_LATE_MS, lateMs);
                }
            } else {
                // On-time dispatch - track min headroom
                const headroomMs = Math.round(timeUntilExec * 1000);

                // Track min headroom
                const currentMin = metricsGet(MetricsOffsets.PRESCHEDULER_MIN_HEADROOM_MS);
                if (currentMin === HEADROOM_UNSET_SENTINEL || headroomMs < currentMin) {
                    metricsSet(MetricsOffsets.PRESCHEDULER_MIN_HEADROOM_MS, headroomMs);
                }
            }

            schedulerLog('[PreScheduler] Dispatching bundle:',
                        'NTP=' + nextEvent.ntpTime.toFixed(3),
                        'current=' + currentNTP.toFixed(3),
                        'early=' + (timeUntilExec * 1000).toFixed(1) + 'ms',
                        'remaining=' + eventHeap.length);

            // Use useWait: true for guaranteed delivery (no lock contention drops)
            const success = dispatchOSCMessage(nextEvent.oscData, false, nextEvent.sourceId, true);
            if (!success) {
                // Queue for retry (only fails if buffer genuinely full)
                queueForRetry(nextEvent.oscData, 'scheduled bundle NTP=' + nextEvent.ntpTime.toFixed(3), nextEvent.sourceId);
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

    // Reschedule based on next needed dispatch time
    dispatchTimer = null;
    nextDispatchAt = Infinity;
    rescheduleDispatch();
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
        metricsAdd(MetricsOffsets.PRESCHEDULER_EVENTS_CANCELLED, removed);
        updateMetrics();  // Update buffer with current queue depth
        schedulerLog('[PreScheduler] Cancelled ' + removed + ' events, ' + eventHeap.length + ' remaining');
        rescheduleDispatch();
    }
};

const heapify = () => {
    for (let i = Math.floor(eventHeap.length / 2) - 1; i >= 0; i--) {
        siftDown(i);
    }
};

const cancelSessionTag = (sessionId, runTag) => {
    cancelBy((event) => event.sessionId === sessionId && event.runTag === runTag);
};

const cancelSession = (sessionId) => {
    cancelBy((event) => event.sessionId === sessionId);
};

const cancelTag = (runTag) => {
    cancelBy((event) => event.runTag === runTag);
};

const cancelAllTags = () => {
    if (eventHeap.length === 0) {
        return;
    }
    const cancelled = eventHeap.length;
    metricsAdd(MetricsOffsets.PRESCHEDULER_EVENTS_CANCELLED, cancelled);
    eventHeap = [];
    updateMetrics();  // Update buffer (sets eventsPending to 0)
    schedulerLog('[PreScheduler] Cancelled all ' + cancelled + ' events');
    rescheduleDispatch();
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

    while (offset + 4 <= data.length) {
        const messageSize = view.getInt32(offset, false);
        offset += 4;

        // Validate message size (must match SC_Stubs.cpp limit)
        if (messageSize <= 0 || messageSize > MAX_OSC_MESSAGE_SIZE || offset + messageSize > data.length) {
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

const processImmediate = (oscData, sourceId = 0) => {
    // Use useWait: true for guaranteed delivery (no lock contention drops)
    if (isBundle(oscData)) {
        const messages = extractMessagesFromBundle(oscData);
        for (let i = 0; i < messages.length; i++) {
            const success = dispatchOSCMessage(messages[i], false, sourceId, true);
            if (!success) {
                queueForRetry(messages[i], 'immediate bundle message ' + i, sourceId);
            }
        }
    } else {
        const success = dispatchOSCMessage(oscData, false, sourceId, true);
        if (!success) {
            queueForRetry(oscData, 'immediate message', sourceId);
        }
    }
};

// Message handling
self.addEventListener('message', (event) => {
    const { data } = event;

    try {
        switch (data.type) {
            case 'init':
                // Set transport mode
                mode = data.mode || 'sab';

                // Apply config overrides
                if (data.maxPendingMessages) {
                    maxPendingMessages = data.maxPendingMessages;
                }
                if (data.snapshotIntervalMs) {
                    metricsSendIntervalMs = data.snapshotIntervalMs;
                }
                if (data.bypassLookaheadS !== undefined) {
                    lookaheadS = data.bypassLookaheadS;
                }

                if (mode === 'sab') {
                    // SAB mode: initialize ring buffer access
                    sharedBuffer = data.sharedBuffer;
                    ringBufferBase = data.ringBufferBase;
                    bufferConstants = data.bufferConstants;
                    initSharedBuffer();

                    // Update scheduler slot size from buffer constants if available
                    if (bufferConstants && bufferConstants.scheduler_slot_size) {
                        schedulerSlotSize = bufferConstants.scheduler_slot_size;
                    }
                } else {
                    // postMessage mode: store worklet port
                    workletPort = data.workletPort;

                    // postMessage mode: create local metrics buffer with same layout as SAB
                    // Size matches METRICS_SIZE (184 bytes = 46 uint32s)
                    const METRICS_SIZE = 184;
                    localMetricsBuffer = new ArrayBuffer(METRICS_SIZE);
                    metricsView = new Uint32Array(localMetricsBuffer);

                    // Start periodic sending of metrics
                    startMetricsSending();
                }

                // Initialize min headroom to sentinel value ("unset")
                // This allows 0ms to be a valid headroom value
                metricsSet(MetricsOffsets.PRESCHEDULER_MIN_HEADROOM_MS, HEADROOM_UNSET_SENTINEL);

                // Initialize max late to 0 (any late value will exceed)
                metricsSet(MetricsOffsets.PRESCHEDULER_MAX_LATE_MS, 0);

                // Start demand-driven dispatching
                startDispatching();

                schedulerLog('[OSCPreSchedulerWorker] Initialized with NTP-based scheduling, mode=' + mode + ', capacity=' + maxPendingMessages);
                self.postMessage({ type: 'initialized' });
                break;

            case 'addOscSource':
                // Handle OSC messages from external sources (OscChannel in workers)
                // The source port is transferred via event.ports
                const sourcePort = event.ports[0];
                if (sourcePort) {
                    sourcePort.onmessage = (e) => {
                        if (e.data.type === 'osc' && e.data.oscData) {
                            // Process through normal scheduling path, preserving sourceId
                            scheduleEvent(e.data.oscData, 0, '', e.data.sourceId || 0);
                        }
                    };
                    schedulerLog('[OSCPreSchedulerWorker] Added external OSC source');
                }
                break;

            case 'send':
                // NTP-based scheduling: extract NTP from bundle
                // scheduleEvent() will dispatch immediately if not a bundle
                scheduleEvent(
                    data.oscData,
                    data.sessionId || 0,
                    data.runTag || '',
                    data.sourceId || 0
                );
                break;

            case 'sendImmediate':
                processImmediate(data.oscData, data.sourceId || 0);
                break;

            case 'directDispatch':
                // Direct dispatch with guaranteed delivery (fallback from main thread SAB contention)
                // Uses Atomics.wait() to block until lock is available - NEVER drops
                {
                    const success = dispatchOSCMessage(data.oscData, false, data.sourceId || 0, true);
                    if (!success) {
                        // Only fails if buffer is genuinely full (not lock contention)
                        queueForRetry(data.oscData, 'directDispatch fallback', data.sourceId || 0);
                    }
                }
                break;

            case 'cancelSessionTag':
                if (data.runTag !== undefined && data.runTag !== null && data.runTag !== '') {
                    cancelSessionTag(data.sessionId || 0, data.runTag);
                }
                break;

            case 'cancelSession':
                cancelSession(data.sessionId || 0);
                break;

            case 'cancelTag':
                if (data.runTag !== undefined && data.runTag !== null && data.runTag !== '') {
                    cancelTag(data.runTag);
                }
                break;

            case 'cancelAll':
                cancelAllTags();
                if (data.ack) {
                    self.postMessage({ type: 'cancelAllAck' });
                }
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

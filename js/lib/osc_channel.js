// SPDX-License-Identifier: MIT OR GPL-3.0-or-later
// Copyright (c) 2025 Sam Aaron

import { writeToRingBuffer, readMessagesFromBuffer } from './ring_buffer_core.js';
import * as MetricsOffsets from './metrics_offsets.js';
import { calculateInControlIndices, calculateReplyChannelIndices } from './control_offsets.js';
import {
    classifyOscMessage,
    shouldBypass,
    getCurrentNTPFromPerformance,
    DEFAULT_BYPASS_LOOKAHEAD_S,
} from './osc_classifier.js';

// Static bypass category → metrics offset map (avoid allocation per send)
const BYPASS_CATEGORY_OFFSETS = {
    nonBundle: MetricsOffsets.BYPASS_NON_BUNDLE,
    immediate: MetricsOffsets.BYPASS_IMMEDIATE,
    nearFuture: MetricsOffsets.BYPASS_NEAR_FUTURE,
    late: MetricsOffsets.BYPASS_LATE,
};

/**
 * OscChannel - Unified dispatch for sending OSC to the audio worklet
 *
 * Handles classification, routing, and metrics for all OSC messages:
 * - nonBundle/immediate/nearFuture/late → direct to worklet (bypass)
 * - farFuture → prescheduler for proper scheduling
 *
 * Works in both SAB and postMessage modes. Can be transferred to Web Workers
 * for direct communication with the AudioWorklet.
 */
export class OscChannel {
    #mode;
    #directPort;         // postMessage mode: MessagePort to worklet
    #sabConfig;          // SAB mode: { sharedBuffer, ringBufferBase, bufferConstants, controlIndices }
    #views;              // SAB mode: { atomicView, dataView, uint8View }
    #preschedulerPort;   // MessagePort to prescheduler (both modes)
    #metricsView;        // SAB mode: Int32Array view into metrics region
    #bypassLookaheadS;   // Threshold for bypass vs prescheduler routing
    #sourceId;           // Numeric source ID (0 = main thread, 1+ = workers)
    #blocking;           // Whether this channel can block with Atomics.wait()
    #getCurrentNTP;      // Function returning current NTP time in seconds

    // Node ID allocation (range-based)
    #nodeIdView;         // SAB mode: Int32Array view for atomic counter
    #nodeIdFrom;         // Start of current range (inclusive)
    #nodeIdTo;           // End of current range (exclusive)
    #nextNodeId;         // Next ID to return within range
    #nodeIdRangeSize;    // Number of IDs per range allocation
    #nodeIdSource;       // PM mode (main thread): function to claim a range directly
    #nodeIdPort;         // PM mode (worker): MessagePort for requesting ranges from main thread
    #pendingNodeIdRange; // PM mode (worker): pre-fetched next range
    #transferNodeIdPort; // PM mode: port to include in transferList (created by transferable getter)

    // Reply reception
    #replyCallback;       // User's reply callback function
    #replySlotIndex = -1; // SAB mode: assigned reply buffer slot (0..7), -1 = unassigned
    #replyPollTimer;      // Legacy: setInterval ID (only if no better mechanism available)
    #replyNotifier;       // SAB main thread: { subscribe, unsubscribe } from transport
    #replyNotifyFn;       // SAB main thread: bound function for unsubscribe
    #replyWaitLoopActive; // SAB worker: whether Atomics.waitAsync loop is running
    #replyPort;           // PM mode: MessagePort receiving replies from worklet
    #replyQueue = [];     // PM AudioWorklet mode: queued replies for pollReplies()
    #replyWorkletPort;    // PM mode: port to worklet for register/unregister
    #replyChannelIndices; // SAB mode: { headIndex, tailIndex, activeIndex, dropsIndex } for this slot

    // Local metrics counters
    // SAB mode: used for tracking, then written atomically to shared memory
    // PM mode: accumulated locally, reported via getMetrics()
    #localMetrics = {
        messagesSent: 0,
        bytesSent: 0,
        nonBundle: 0,
        immediate: 0,
        nearFuture: 0,
        late: 0,
        bypassed: 0,
    };

    /**
     * Private constructor - use static factory methods
     */
    constructor(mode, config) {
        this.#mode = mode;
        this.#preschedulerPort = config.preschedulerPort || null;
        this.#bypassLookaheadS = config.bypassLookaheadS ?? DEFAULT_BYPASS_LOOKAHEAD_S;
        this.#sourceId = config.sourceId ?? 0;
        this.#blocking = config.blocking ?? (this.#sourceId !== 0);
        this.#getCurrentNTP = config.getCurrentNTP ?? getCurrentNTPFromPerformance;
        this.#nodeIdRangeSize = 1000;

        if (mode === 'postMessage') {
            this.#directPort = config.port;
        } else {
            this.#sabConfig = {
                sharedBuffer: config.sharedBuffer,
                ringBufferBase: config.ringBufferBase,
                bufferConstants: config.bufferConstants,
                controlIndices: config.controlIndices,
            };
            this.#initViews();
            this.#replyNotifier = config.replyNotifier || null;

            // Create metrics view at correct offset within SharedArrayBuffer
            if (config.sharedBuffer && config.bufferConstants) {
                const metricsBase = config.ringBufferBase + config.bufferConstants.METRICS_START;
                this.#metricsView = new Int32Array(
                    config.sharedBuffer,
                    metricsBase,
                    config.bufferConstants.METRICS_SIZE / 4
                );
            }

            // Create node ID counter view for atomic range allocation
            if (config.sharedBuffer && config.bufferConstants?.NODE_ID_COUNTER_START !== undefined) {
                const counterBase = config.ringBufferBase + config.bufferConstants.NODE_ID_COUNTER_START;
                this.#nodeIdView = new Int32Array(config.sharedBuffer, counterBase, 1);
                this.#claimNodeIdRange();
            }
        }

        // PM mode: accept a direct range source function (main thread)
        if (config.nodeIdSource) {
            this.#nodeIdSource = config.nodeIdSource;
            this.#claimNodeIdRange();
        }

        // Accept a pre-assigned range (for PM worker channels via fromTransferable)
        if (config.nodeIdRange) {
            this.#nodeIdFrom = config.nodeIdRange.from;
            this.#nodeIdTo = config.nodeIdRange.to;
            this.#nextNodeId = config.nodeIdRange.from;
        }

        // Reply infrastructure
        if (config.replyWorkletPort) {
            this.#replyWorkletPort = config.replyWorkletPort;
        }

        // PM mode (worker): MessagePort for requesting more ranges from main thread
        if (config.nodeIdPort) {
            this.#nodeIdPort = config.nodeIdPort;
            this.#nodeIdPort.onmessage = (e) => {
                if (e.data.type === 'nodeIdRange') {
                    this.#pendingNodeIdRange = { from: e.data.from, to: e.data.to };
                }
            };
            // Pre-request the first extra range
            this.#requestNodeIdRange();
        }
    }

    /**
     * Initialize typed array views for SAB mode
     */
    #initViews() {
        const sab = this.#sabConfig.sharedBuffer;
        const headerSize = this.#sabConfig.bufferConstants.MESSAGE_HEADER_SIZE || 16;
        const headerScratch = new Uint8Array(headerSize);
        this.#views = {
            atomicView: new Int32Array(sab),
            dataView: new DataView(sab),
            uint8View: new Uint8Array(sab),
            headerScratch,
            headerScratchView: new DataView(headerScratch.buffer),
        };
    }

    // =========================================================================
    // Classification
    // =========================================================================

    /**
     * Classify OSC data for routing
     * @param {Uint8Array} oscData
     * @returns {'nonBundle' | 'immediate' | 'nearFuture' | 'late' | 'farFuture'}
     */
    classify(oscData) {
        return classifyOscMessage(oscData, {
            getCurrentNTP: this.#getCurrentNTP,
            bypassLookaheadS: this.#bypassLookaheadS,
        });
    }

    // =========================================================================
    // Metrics
    // =========================================================================

    /**
     * Record a successful send - updates message count, byte count, and category metrics
     * @param {number} byteCount - Size of the message in bytes
     * @param {string} [category] - Bypass category (if applicable)
     */
    #recordSend(byteCount, category = null) {
        if (this.#mode === 'sab' && this.#metricsView) {
            // SAB mode: atomic increment to shared memory
            Atomics.add(this.#metricsView, MetricsOffsets.OSC_OUT_MESSAGES_SENT, 1);
            Atomics.add(this.#metricsView, MetricsOffsets.OSC_OUT_BYTES_SENT, byteCount);

            // Also record category if this was a bypass send
            if (category) {
                const offset = BYPASS_CATEGORY_OFFSETS[category];
                if (offset !== undefined) {
                    Atomics.add(this.#metricsView, offset, 1);
                    Atomics.add(this.#metricsView, MetricsOffsets.PRESCHEDULER_BYPASSED, 1);
                }
            }
        } else {
            // PM mode: local counters
            this.#localMetrics.messagesSent++;
            this.#localMetrics.bytesSent += byteCount;

            if (category && category in this.#localMetrics) {
                this.#localMetrics[category]++;
                this.#localMetrics.bypassed++;
            }
        }
    }

    /**
     * Get and reset local metrics (for periodic reporting)
     * @returns {Object} Metrics snapshot
     */
    getAndResetMetrics() {
        const snapshot = { ...this.#localMetrics };
        this.#localMetrics = {
            messagesSent: 0,
            bytesSent: 0,
            nonBundle: 0,
            immediate: 0,
            nearFuture: 0,
            late: 0,
            bypassed: 0,
        };
        return snapshot;
    }

    /**
     * Get current metrics snapshot.
     * SAB mode: reads aggregated metrics from shared memory
     * PM mode: returns local metrics (aggregated via heartbeat)
     */
    getMetrics() {
        if (this.#mode === 'sab' && this.#metricsView) {
            return {
                messagesSent: Atomics.load(this.#metricsView, MetricsOffsets.OSC_OUT_MESSAGES_SENT),
                bytesSent: Atomics.load(this.#metricsView, MetricsOffsets.OSC_OUT_BYTES_SENT),
                nonBundle: Atomics.load(this.#metricsView, MetricsOffsets.BYPASS_NON_BUNDLE),
                immediate: Atomics.load(this.#metricsView, MetricsOffsets.BYPASS_IMMEDIATE),
                nearFuture: Atomics.load(this.#metricsView, MetricsOffsets.BYPASS_NEAR_FUTURE),
                late: Atomics.load(this.#metricsView, MetricsOffsets.BYPASS_LATE),
                bypassed: Atomics.load(this.#metricsView, MetricsOffsets.PRESCHEDULER_BYPASSED),
            };
        }
        return { ...this.#localMetrics };
    }

    // =========================================================================
    // Sending
    // =========================================================================

    /**
     * Send directly to worklet (bypass path)
     * @param {Uint8Array} oscData
     * @param {string} [bypassCategory] - Category for metrics (PM mode only)
     * @param {boolean} [allowFallback=true] - Allow fallback to prescheduler on SAB contention (main thread only)
     */
    #sendDirect(oscData, bypassCategory = null, allowFallback = true) {
        if (this.#mode === 'postMessage') {
            if (!this.#directPort) return false;
            // Include bypass category and sourceId so worklet can track metrics and log source
            this.#directPort.postMessage({ type: 'osc', oscData, bypassCategory, sourceId: this.#sourceId });
            return true;
        } else {
            // SAB mode - write to ring buffer with sourceId in header
            // Logging is handled by osc_out_log_sab_worker reading from ring buffer
            const canBlock = this.#blocking;

            const success = writeToRingBuffer({
                atomicView: this.#views.atomicView,
                dataView: this.#views.dataView,
                uint8View: this.#views.uint8View,
                bufferConstants: this.#sabConfig.bufferConstants,
                ringBufferBase: this.#sabConfig.ringBufferBase,
                controlIndices: this.#sabConfig.controlIndices,
                oscMessage: oscData,
                sourceId: this.#sourceId,
                // Non-blocking: try once, will fall back to prescheduler
                // Blocking: use Atomics.wait() for guaranteed delivery
                maxSpins: canBlock ? 10 : 0,
                useWait: canBlock,
                headerScratch: this.#views.headerScratch,
                headerScratchView: this.#views.headerScratchView,
            });

            if (!success) {
                // Non-blocking: fall back to prescheduler for guaranteed delivery
                // The prescheduler is a worker and can use Atomics.wait()
                if (!canBlock && allowFallback && this.#preschedulerPort) {
                    if (this.#metricsView) {
                        // Track when optimistic direct write falls back to prescheduler (not an error - message still delivered)
                        Atomics.add(this.#metricsView, MetricsOffsets.RING_BUFFER_DIRECT_WRITE_FAILS, 1);
                    }
                    this.#preschedulerPort.postMessage({
                        type: 'directDispatch',
                        oscData,
                        sourceId: this.#sourceId,
                    });
                    return true;  // Message will be delivered via prescheduler
                }
            }
            return success;
        }
    }

    /**
     * Send to prescheduler (far-future path)
     * Logging happens at dispatch time when message reaches the ring buffer.
     */
    #sendToPrescheduler(oscData) {
        if (!this.#preschedulerPort) {
            // Fallback: send direct if no prescheduler port
            // This shouldn't happen in normal usage
            console.error('[OscChannel] No prescheduler port, sending direct');
            return this.#sendDirect(oscData);
        }
        this.#preschedulerPort.postMessage({ type: 'osc', oscData, sourceId: this.#sourceId });
        return true;
    }

    /**
     * Send OSC message with automatic routing
     *
     * - nonBundle/immediate/nearFuture/late → direct to worklet
     * - farFuture → prescheduler for proper scheduling
     *
     * In SAB mode, the main thread uses an optimistic direct write path. If the
     * ring buffer lock is held by another writer, the message is routed through
     * the prescheduler instead. This is not an error - the message is still
     * delivered. Workers don't need this fallback as they use Atomics.wait()
     * for guaranteed lock acquisition.
     *
     * @param {Uint8Array} oscData - OSC message bytes
     * @returns {boolean} true if sent successfully
     */
    send(oscData) {
        const category = this.classify(oscData);

        if (!shouldBypass(category)) {
            // Far-future: route to prescheduler
            const success = this.#sendToPrescheduler(oscData);
            if (success) {
                // Record send without bypass category (prescheduler will track its own metrics)
                this.#recordSend(oscData.length, null);
            }
            return success;
        } else {
            // Bypass: send direct to worklet
            const success = this.#sendDirect(oscData, category);
            if (success) {
                // Record send with bypass category
                this.#recordSend(oscData.length, category);
            }
            return success;
        }
    }

    /**
     * Send directly without classification (for callers who already know routing)
     * Does not increment metrics.
     * @param {Uint8Array} oscData
     * @returns {boolean}
     */
    sendDirect(oscData) {
        return this.#sendDirect(oscData);
    }

    /**
     * Send to prescheduler without classification (for callers who already know routing)
     * @param {Uint8Array} oscData
     * @returns {boolean}
     */
    sendToPrescheduler(oscData) {
        return this.#sendToPrescheduler(oscData);
    }

    // =========================================================================
    // Node ID Allocation
    // =========================================================================

    /**
     * Get the next unique node ID.
     *
     * SAB mode: single atomic increment — always correct, no batching needed.
     * PM mode: range-based allocation with async pre-fetching from main thread.
     *
     * @returns {number} A unique node ID (>= 1000)
     */
    nextNodeId() {
        // SAB mode: direct atomic increment, no ranges
        if (this.#nodeIdView) {
            return Atomics.add(this.#nodeIdView, 0, 1);
        }

        // PM mode: range-based allocation
        if (this.#nextNodeId >= this.#nodeIdTo) {
            this.#claimNodeIdRange();
        }
        const id = this.#nextNodeId++;
        // Pre-request next range at halfway through current range (PM worker only)
        if (this.#nodeIdPort && !this.#pendingNodeIdRange &&
            (this.#nodeIdTo - this.#nextNodeId) <= (this.#nodeIdRangeSize >>> 1)) {
            this.#requestNodeIdRange();
        }
        return id;
    }

    /**
     * Claim a new range of node IDs (PM mode only).
     * Main thread: use the direct source function.
     * Worker: use pre-fetched range from main thread.
     */
    #claimNodeIdRange() {
        if (this.#nodeIdSource) {
            // PM mode (main thread): direct range allocation
            const range = this.#nodeIdSource(this.#nodeIdRangeSize);
            this.#nodeIdFrom = range.from;
            this.#nodeIdTo = range.to;
            this.#nextNodeId = range.from;
        } else if (this.#pendingNodeIdRange) {
            // PM mode (worker): use pre-fetched range
            this.#nodeIdFrom = this.#pendingNodeIdRange.from;
            this.#nodeIdTo = this.#pendingNodeIdRange.to;
            this.#nextNodeId = this.#pendingNodeIdRange.from;
            this.#pendingNodeIdRange = null;
            // Pre-request the next range
            this.#requestNodeIdRange();
        } else if (this.#nodeIdPort) {
            // PM mode (worker): pre-fetched range hasn't arrived yet.
            // This only happens if IDs are consumed faster than the postMessage
            // round-trip — i.e. a tight synchronous loop of >9000 IDs without
            // yielding to the event loop.
            throw new Error(
                '[OscChannel] Node ID range exhausted before async refill arrived. ' +
                'Yield to the event loop between large batches of nextNodeId() calls.'
            );
        }
    }

    /**
     * Request a new node ID range from the main thread via MessagePort.
     * Used by PM mode worker channels.
     */
    #requestNodeIdRange() {
        if (this.#nodeIdPort) {
            this.#nodeIdPort.postMessage({ type: 'requestNodeIdRange' });
        }
    }

    // =========================================================================
    // Properties
    // =========================================================================

    /**
     * Set the NTP time source for classification.
     * Use in AudioWorklet where performance.timeOrigin is unavailable.
     * @param {Function} fn - Returns current NTP time in seconds
     */
    set getCurrentNTP(fn) {
        this.#getCurrentNTP = fn;
    }

    /**
     * Get the transport mode
     * @returns {'sab' | 'postMessage'}
     */
    get mode() {
        return this.#mode;
    }

    /**
     * Number of reply messages dropped because the reply buffer was full.
     * SAB mode only — returns undefined in PM mode or before activateReplies().
     * Counter resets each time the slot is (re)claimed.
     * @returns {number|undefined}
     */
    get replyDrops() {
        if (this.#mode !== 'sab' || this.#replySlotIndex < 0) return undefined;
        return Atomics.load(this.#views.atomicView, this.#replyChannelIndices.dropsIndex);
    }

    /**
     * Get data needed to transfer this channel to a worker
     * Use with: worker.postMessage({ channel: oscChannel.transferable }, oscChannel.transferList)
     * @returns {Object} Serializable config object
     */
    get transferable() {
        const base = {
            mode: this.#mode,
            preschedulerPort: this.#preschedulerPort,
            bypassLookaheadS: this.#bypassLookaheadS,
            sourceId: this.#sourceId,
            blocking: this.#blocking,
        };

        if (this.#mode === 'postMessage') {
            // Claim a large initial range for the worker channel.
            // PM workers can't claim ranges synchronously (no SAB),
            // so we give them a generous initial allocation.
            // They can request more async via nodeIdPort.
            const workerRangeSize = this.#nodeIdRangeSize * 10;
            let nodeIdRange;
            let nodeIdPort;
            if (this.#nodeIdSource) {
                const range = this.#nodeIdSource(workerRangeSize);
                nodeIdRange = { from: range.from, to: range.to };

                // Create a MessageChannel for the worker to request more ranges
                const nodeIdChannel = new MessageChannel();
                const source = this.#nodeIdSource;
                const rangeSize = this.#nodeIdRangeSize;
                nodeIdChannel.port1.onmessage = (e) => {
                    if (e.data.type === 'requestNodeIdRange') {
                        const r = source(rangeSize);
                        nodeIdChannel.port1.postMessage({ type: 'nodeIdRange', from: r.from, to: r.to });
                    }
                };
                nodeIdPort = nodeIdChannel.port2;
                this.#transferNodeIdPort = nodeIdPort;
            }
            return {
                ...base,
                port: this.#directPort,
                nodeIdRange,
                nodeIdPort,
            };
        } else {
            return {
                ...base,
                sharedBuffer: this.#sabConfig.sharedBuffer,
                ringBufferBase: this.#sabConfig.ringBufferBase,
                bufferConstants: this.#sabConfig.bufferConstants,
                controlIndices: this.#sabConfig.controlIndices,
            };
        }
    }

    /**
     * Get the list of transferable objects for postMessage
     * @returns {Array} Array of transferable objects
     */
    get transferList() {
        const list = [];
        if (this.#mode === 'postMessage' && this.#directPort) {
            list.push(this.#directPort);
        }
        if (this.#preschedulerPort) {
            list.push(this.#preschedulerPort);
        }
        if (this.#transferNodeIdPort) {
            list.push(this.#transferNodeIdPort);
            this.#transferNodeIdPort = null;
        }
        return list;
    }

    // =========================================================================
    // Reply Reception
    // =========================================================================

    /**
     * Activate reply reception — claim a reply channel slot.
     * Usually called for you by setReplyHandler(); only call directly if
     * you want to claim the slot before installing a handler.
     * In SAB mode, atomically claims one of 8 reply buffer slots.
     * In PM mode, registers a MessagePort with the worklet for reply delivery.
     */
    activateReplies() {
        if (this.#mode === 'sab') {
            this.#activateSabReply();
        } else {
            this.#activatePmReply();
        }
    }

    /**
     * Deactivate reply reception — release the reply channel slot.
     */
    deactivateReplies() {
        if (this.#mode === 'sab') {
            this.#deactivateSabReply();
        } else {
            this.#deactivatePmReply();
        }
    }

    /**
     * Register a handler for OSC replies from scsynth. Idempotent — replaces
     * any previously-registered handler. In AudioWorklet contexts the worklet
     * must call pollReplies() from process() to drain; in all other contexts
     * delivery is automatic.
     *
     * SAB mode (zero-copy): handler receives (view, offset, length, sequence).
     *   `view` is the shared SAB Uint8Array; read bytes from view[offset..offset+length].
     *   Data is valid only for the duration of the handler call.
     * PM mode: handler receives (oscData, sequence). `oscData` is a copy.
     *
     * @param {Function} handler
     */
    setReplyHandler(handler) {
        this.#replyCallback = handler;
        this.activateReplies();
        this.#setupAutoNotification();
    }

    /**
     * Clear the reply handler and release the reply channel. Idempotent.
     */
    clearReplyHandler() {
        this.#replyCallback = null;
        this.deactivateReplies();
    }

    /**
     * Drain pending replies, calling the registered handler (or `handler`
     * argument, if given) once per message. Returns the number of messages
     * processed. Zero-allocation on the hot path.
     *
     * Call from AudioWorklet process() to receive replies on the audio thread.
     * In non-worklet contexts, automatic delivery already calls this for you.
     *
     * @param {Function} [handler] - Optional override for this call only
     * @returns {number} Number of messages drained
     */
    pollReplies(handler) {
        const cb = handler || this.#replyCallback;
        if (!cb) return 0;
        if (this.#mode === 'sab') {
            if (this.#replySlotIndex < 0) return 0;
            return this.#pollRepliesSab(cb);
        } else {
            if (!this.#replyPort) return 0;
            return this.#pollRepliesPm(cb);
        }
    }

    #activateSabReply() {
        if (this.#replySlotIndex >= 0) return;  // Already active

        const bc = this.#sabConfig.bufferConstants;
        if (!bc.REPLY_CHANNEL_COUNT) {
            throw new Error('WASM does not support reply channels');
        }
        const atomicView = this.#views.atomicView;

        // Atomically claim a slot via compareExchange on the active flag.
        // Head/tail/drops are reset BEFORE setting active to avoid a race
        // where the osc_in_worker sees active=1 and reads stale values.
        for (let i = 0; i < bc.REPLY_CHANNEL_COUNT; i++) {
            const idx = calculateReplyChannelIndices(
                this.#sabConfig.ringBufferBase,
                bc.REPLY_CHANNELS_CONTROL_START,
                bc.REPLY_CHANNEL_CONTROL_SIZE,
                i,
            );
            Atomics.store(atomicView, idx.headIndex, 0);
            Atomics.store(atomicView, idx.tailIndex, 0);
            Atomics.store(atomicView, idx.dropsIndex, 0);
            if (Atomics.compareExchange(atomicView, idx.activeIndex, 0, 1) === 0) {
                this.#replyChannelIndices = idx;
                this.#replySlotIndex = i;
                return;
            }
        }
        throw new Error('All ' + bc.REPLY_CHANNEL_COUNT + ' reply channel slots are in use — cannot register for replies');
    }

    #setupAutoNotification() {
        // Idempotent: skip if already wired up.
        if (this.#replyNotifyFn || this.#replyWaitLoopActive || this.#replyPollTimer) return;

        const isAudioWorklet = typeof AudioWorkletGlobalScope !== 'undefined';
        if (isAudioWorklet) {
            // AudioWorklet has no event loop available to process(); the worklet
            // must call pollReplies() itself from process(). No-op here.
            return;
        }
        if (this.#mode !== 'sab') {
            // PM mode wires its own port-based delivery in #activatePmReply().
            return;
        }
        if (this.#replyNotifier) {
            this.#replyNotifyFn = () => this.pollReplies();
            this.#replyNotifier.subscribe(this.#replyNotifyFn);
        } else if (typeof Atomics !== 'undefined' && typeof Atomics.waitAsync === 'function') {
            this.#startReplyWaitLoop();
        } else {
            // Fallback: setInterval polling (should not happen in modern browsers)
            this.#replyPollTimer = setInterval(() => this.pollReplies(), 5);
        }
    }

    #deactivateSabReply() {
        if (this.#replySlotIndex < 0) return;

        // Deactivate — osc_in_worker stops writing
        Atomics.store(this.#views.atomicView, this.#replyChannelIndices.activeIndex, 0);

        if (this.#replyPollTimer) {
            clearInterval(this.#replyPollTimer);
            this.#replyPollTimer = null;
        }

        if (this.#replyNotifyFn && this.#replyNotifier) {
            this.#replyNotifier.unsubscribe(this.#replyNotifyFn);
            this.#replyNotifyFn = null;
        }

        // Stop Atomics.waitAsync loop (callback check handles this)
        this.#replyWaitLoopActive = false;

        this.#replySlotIndex = -1;
        this.#replyChannelIndices = null;
    }

    #startReplyWaitLoop() {
        const atomicView = this.#views.atomicView;
        const { headIndex: headIdx, tailIndex: tailIdx } = this.#replyChannelIndices;
        this.#replyWaitLoopActive = true;

        const waitAndPoll = () => {
            if (!this.#replyWaitLoopActive) return;

            const currentHead = Atomics.load(atomicView, headIdx);
            const currentTail = Atomics.load(atomicView, tailIdx);

            // If there's data, drain it first
            if (currentHead !== currentTail) {
                this.pollReplies();
            }

            if (!this.#replyWaitLoopActive) return;

            // Wait for head to change (non-blocking, Promise-based)
            const newHead = Atomics.load(atomicView, headIdx);
            const result = Atomics.waitAsync(atomicView, headIdx, newHead);
            if (result.async) {
                result.value.then(() => {
                    if (this.#replyWaitLoopActive) {
                        this.pollReplies();
                        waitAndPoll();
                    }
                });
            } else {
                // Value already changed, poll and continue
                this.pollReplies();
                setTimeout(waitAndPoll, 0);
            }
        };
        waitAndPoll();
    }

    #pollRepliesSab(callback) {
        const atomicView = this.#views.atomicView;
        const { headIndex: headIdx, tailIndex: tailIdx } = this.#replyChannelIndices;

        const head = Atomics.load(atomicView, headIdx);
        const tail = Atomics.load(atomicView, tailIdx);
        if (head === tail) return 0;

        const bc = this.#sabConfig.bufferConstants;
        const bufferStart = this.#sabConfig.ringBufferBase +
            bc.REPLY_CHANNELS_BUFFER_START +
            (this.#replySlotIndex * bc.REPLY_CHANNEL_BUFFER_SIZE);
        const bufferSize = bc.REPLY_CHANNEL_BUFFER_SIZE;

        const uint8View = this.#views.uint8View;

        const { newTail, messagesRead } = readMessagesFromBuffer({
            uint8View,
            dataView: this.#views.dataView,
            bufferStart, bufferSize,
            head, tail,
            messageMagic: bc.MESSAGE_MAGIC,
            paddingMagic: bc.PADDING_MAGIC,
            headerSize: bc.MESSAGE_HEADER_SIZE || 16,
            maxMessages: 64,
            onMessage: (payloadOffset, payloadLength, sequence) => {
                // Zero-copy: pass shared buffer view + offset/length directly.
                // Data is valid for the duration of the callback only — caller
                // must read or copy what they need before returning.
                callback(uint8View, payloadOffset, payloadLength, sequence);
            },
        });

        if (messagesRead > 0) {
            Atomics.store(atomicView, tailIdx, newTail);
        }
        return messagesRead;
    }

    #activatePmReply() {
        if (this.#replyPort) return;  // Already active

        // Use the worklet port (main thread) or direct port (worker) for registration
        const registrationPort = this.#replyWorkletPort || this.#directPort;
        if (!registrationPort) {
            throw new Error('No port available for reply registration');
        }

        // Create a MessageChannel — send one end to the worklet
        const channel = new MessageChannel();
        registrationPort.postMessage(
            { type: 'addReplyPort', sourceId: this.#sourceId },
            [channel.port1]
        );
        this.#replyPort = channel.port2;

        const isAudioWorklet = typeof AudioWorkletGlobalScope !== 'undefined';
        this.#replyPort.onmessage = (e) => {
            if (e.data.type !== 'oscReplies' || !e.data.count) return;
            const { count, buffer, messages } = e.data;
            if (!messages || !buffer) return;
            const bufferView = new Uint8Array(buffer);

            if (isAudioWorklet) {
                // Queue for pollReplies()
                for (let i = 0; i < count; i++) {
                    const entry = messages[i];
                    if (!entry) continue;
                    const oscData = bufferView.slice(entry.offset, entry.offset + entry.length);
                    this.#replyQueue.push({ oscData, sequence: entry.sequence });
                }
            } else {
                // Worker: fire callback directly
                const callback = this.#replyCallback;
                if (!callback) return;
                for (let i = 0; i < count; i++) {
                    const entry = messages[i];
                    if (!entry) continue;
                    const oscData = bufferView.slice(entry.offset, entry.offset + entry.length);
                    callback(oscData, entry.sequence);
                }
            }
        };
    }

    #deactivatePmReply() {
        if (this.#replyPort) {
            const registrationPort = this.#replyWorkletPort || this.#directPort;
            if (registrationPort) {
                registrationPort.postMessage({ type: 'removeReplyPort', sourceId: this.#sourceId });
            }
            this.#replyPort.close();
            this.#replyPort = null;
        }
        this.#replyQueue.length = 0;
    }

    #pollRepliesPm(callback) {
        const queue = this.#replyQueue;
        const count = queue.length;
        if (count === 0) return 0;
        for (let i = 0; i < count; i++) {
            callback(queue[i].oscData, queue[i].sequence);
        }
        queue.length = 0;
        return count;
    }

    /**
     * Close the channel
     */
    close() {
        this.clearReplyHandler();
        if (this.#mode === 'postMessage' && this.#directPort) {
            this.#directPort.close();
            this.#directPort = null;
        }
        if (this.#preschedulerPort) {
            this.#preschedulerPort.close();
            this.#preschedulerPort = null;
        }
    }

    // =========================================================================
    // Static Factory Methods
    // =========================================================================

    /**
     * Create a postMessage-backed OscChannel
     * @private
     * @param {Object} config
     * @param {MessagePort} config.port - MessagePort connected to the worklet
     * @param {MessagePort} config.preschedulerPort - MessagePort to prescheduler
     * @param {number} [config.bypassLookaheadS=0.5] - Threshold for bypass routing (seconds)
     * @param {number} [config.sourceId=0] - Source ID (0 = main, 1+ = workers)
     * @param {boolean} [config.blocking] - Unused in postMessage mode (only applies to SAB transport)
     * @returns {OscChannel}
     */
    static createPostMessage(config) {
        // Support old API: createPostMessage(port)
        if (config instanceof MessagePort) {
            return new OscChannel('postMessage', { port: config });
        }
        return new OscChannel('postMessage', config);
    }

    /**
     * Create a SAB-backed OscChannel
     * @private
     * @param {Object} config
     * @param {SharedArrayBuffer} config.sharedBuffer
     * @param {number} config.ringBufferBase
     * @param {Object} config.bufferConstants
     * @param {Object} [config.controlIndices] - If not provided, will be calculated
     * @param {MessagePort} config.preschedulerPort - MessagePort to prescheduler
     * @param {number} [config.bypassLookaheadS=0.5] - Threshold for bypass routing (seconds)
     * @param {number} [config.sourceId=0] - Source ID (0 = main, 1+ = workers)
     * @param {boolean} [config.blocking] - Whether to use Atomics.wait() (default: true for sourceId !== 0)
     * @param {Object} [config.replyNotifier] - { subscribe(fn), unsubscribe(fn) } for event-driven reply notification
     * @returns {OscChannel}
     */
    static createSAB(config) {
        // Calculate control indices if not provided
        let controlIndices = config.controlIndices;
        if (!controlIndices) {
            controlIndices = calculateInControlIndices(
                config.ringBufferBase,
                config.bufferConstants.CONTROL_START
            );
        }

        return new OscChannel('sab', {
            sharedBuffer: config.sharedBuffer,
            ringBufferBase: config.ringBufferBase,
            bufferConstants: config.bufferConstants,
            controlIndices,
            preschedulerPort: config.preschedulerPort,
            bypassLookaheadS: config.bypassLookaheadS,
            sourceId: config.sourceId,
            blocking: config.blocking,
            replyNotifier: config.replyNotifier,
        });
    }

    /**
     * Reconstruct an OscChannel from transferred data
     * Use in worker: const channel = OscChannel.fromTransferable(event.data.channel)
     * @param {Object} data - Data from transferable getter
     * @returns {OscChannel}
     */
    static fromTransferable(data) {
        if (data.mode === 'postMessage') {
            return new OscChannel('postMessage', {
                port: data.port,
                preschedulerPort: data.preschedulerPort,
                bypassLookaheadS: data.bypassLookaheadS,
                sourceId: data.sourceId,
                blocking: data.blocking,
                nodeIdRange: data.nodeIdRange,
                nodeIdPort: data.nodeIdPort,
            });
        } else {
            return new OscChannel('sab', {
                sharedBuffer: data.sharedBuffer,
                ringBufferBase: data.ringBufferBase,
                bufferConstants: data.bufferConstants,
                controlIndices: data.controlIndices,
                preschedulerPort: data.preschedulerPort,
                bypassLookaheadS: data.bypassLookaheadS,
                sourceId: data.sourceId,
                blocking: data.blocking,
            });
        }
    }
}

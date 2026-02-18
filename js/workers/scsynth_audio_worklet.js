// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2025 Sam Aaron

/**
 * AudioWorklet Processor for scsynth WASM
 * Runs in AudioWorkletGlobalScope with real-time priority
 */

import * as MetricsOffsets from '../lib/metrics_offsets.js';
import { writeMessageToBuffer, calculateAvailableSpace, readMessagesFromBuffer } from '../lib/ring_buffer_core.js';
import { calculateAllControlIndices } from '../lib/control_offsets.js';

// PM Mode Pool Configuration - pre-allocated buffers for allocation-free process()
const PM_POOL_CONFIG = {
    // Outgoing pools
    MAX_REPLY_MESSAGES: 64,
    MAX_DEBUG_MESSAGES: 32,
    MAX_LOG_ENTRIES: 100,

    // Shared buffer sizes (packed approach)
    REPLY_BUFFER_SIZE: 128 * 1024,   // 128KB - matches OUT_BUFFER_SIZE
    DEBUG_BUFFER_SIZE: 64 * 1024,    // 64KB - matches DEBUG_BUFFER_SIZE
    LOG_BUFFER_SIZE: 256 * 1024,     // 256KB for log entries

    // Message size limits
    LOG_MAX_MESSAGE_SIZE: 16 * 1024, // 16KB - truncate larger messages

    // Incoming path
    MAX_OSC_MESSAGE_SIZE: 8192,      // For header assembly on wrap
};

class ScsynthProcessor extends AudioWorkletProcessor {
    constructor() {
        super();

        // Transport mode: 'sab' or 'postMessage'
        this.mode = 'sab';

        this.sharedBuffer = null;
        this.wasmModule = null;
        this.wasmInstance = null;
        this.isInitialized = false;
        this.processCallCount = 0;
        this.lastStatusCheck = 0;
        this.ringBufferBase = null;
        this.pendingClearSched = false;

        // Pre-allocated audio view to avoid per-frame allocations
        this.audioView = null;
        this.lastAudioBufferPtr = 0;
        this.lastWasmBufferSize = 0;

        // Node tree version tracking (for postMessage mode)
        this.lastTreeVersion = -1;
        this.treeSnapshotsSent = 0;
        this.lastTreeSendTime = -1; // AudioContext time of last send (-1 = never)
        this.treeSnapshotMinInterval = 0.150; // Default, can be overridden by snapshotIntervalMs config

        // Views into SharedArrayBuffer (or WASM memory in postMessage mode)
        this.atomicView = null;
        this.uint8View = null;
        this.dataView = null;
        this.localClockOffsetView = null;  // Float64Array for reading local clock offset

        // Buffer constants (loaded from WASM at initialization)
        this.bufferConstants = null;

        // Control region indices (Int32Array indices) - will be calculated dynamically
        this.CONTROL_INDICES = null;

        // Metrics view (Uint32Array into metrics region of SharedArrayBuffer)
        this.metricsView = null;

        // Status flag masks
        this.STATUS_FLAGS = {
            OK: 0,
            BUFFER_FULL: 1 << 0,
            OVERRUN: 1 << 1,
            WASM_ERROR: 1 << 2,
            FRAGMENTED_MSG: 1 << 3
        };

        // Additional OSC input ports (for prescheduler and user workers)
        // These allow workers to send OSC directly to the worklet
        this.oscPorts = [];

        // Map of port -> sourceId for worker ports (postMessage mode)
        this.portSourceIds = new Map();

        // Pre-allocated channel views to avoid per-frame subarray() allocations
        this.channelViews = null;
        this.lastNumSamples = 0;
        this.lastNumChannels = 0;

        // PM Mode pools - pre-allocated for allocation-free process()
        // Initialized in initPMPools() after mode is known
        this.pmPools = null;

        // Pre-allocated objects for checkStatus() to avoid allocation on audio thread
        this._statusObj = {
            bufferFull: false,
            overrun: false,
            wasmError: false,
            fragmented: false
        };
        this._metricsObj = {
            processCount: 0,
            messagesProcessed: 0,
            messagesDropped: 0,
            schedulerQueueDepth: 0,
            schedulerQueueMax: 0,
            schedulerQueueDropped: 0
        };
        this._statusMessage = {
            type: 'status',
            flags: 0,
            status: this._statusObj,
            metrics: this._metricsObj
        };

        // Listen for messages from main thread
        this.port.onmessage = this.handleMessage.bind(this);
    }

    // Load buffer constants from WASM module
    // Reads the BufferLayout struct exported by C++
    loadBufferConstants() {
        if (!this.wasmInstance || !this.wasmInstance.exports.get_buffer_layout) {
            throw new Error('WASM instance does not export get_buffer_layout');
        }

        // Get pointer to BufferLayout struct
        const layoutPtr = this.wasmInstance.exports.get_buffer_layout();

        // Get WASM memory (imported, not exported - stored in this.wasmMemory)
        const memory = this.wasmMemory;
        if (!memory) {
            throw new Error('WASM memory not available');
        }

        // Read the struct (34 uint32_t fields + 1 uint8_t + 3 padding bytes = 140 bytes)
        const uint32View = new Uint32Array(memory.buffer, layoutPtr, 34);
        const uint8View = new Uint8Array(memory.buffer, layoutPtr, 140);

        // Extract constants (order matches BufferLayout struct in shared_memory.h)
        // NOTE: NODE_TREE is now contiguous with METRICS for efficient postMessage copying
        this.bufferConstants = {
            IN_BUFFER_START: uint32View[0],
            IN_BUFFER_SIZE: uint32View[1],
            OUT_BUFFER_START: uint32View[2],
            OUT_BUFFER_SIZE: uint32View[3],
            DEBUG_BUFFER_START: uint32View[4],
            DEBUG_BUFFER_SIZE: uint32View[5],
            CONTROL_START: uint32View[6],
            CONTROL_SIZE: uint32View[7],
            METRICS_START: uint32View[8],
            METRICS_SIZE: uint32View[9],
            // NODE_TREE now immediately follows METRICS
            NODE_TREE_START: uint32View[10],
            NODE_TREE_SIZE: uint32View[11],
            NODE_TREE_HEADER_SIZE: uint32View[12],
            NODE_TREE_ENTRY_SIZE: uint32View[13],
            NODE_TREE_DEF_NAME_SIZE: uint32View[14],
            NODE_TREE_MIRROR_MAX_NODES: uint32View[15],
            NTP_START_TIME_START: uint32View[16],
            NTP_START_TIME_SIZE: uint32View[17],
            DRIFT_OFFSET_START: uint32View[18],
            DRIFT_OFFSET_SIZE: uint32View[19],
            GLOBAL_OFFSET_START: uint32View[20],
            GLOBAL_OFFSET_SIZE: uint32View[21],
            AUDIO_CAPTURE_START: uint32View[22],
            AUDIO_CAPTURE_SIZE: uint32View[23],
            AUDIO_CAPTURE_HEADER_SIZE: uint32View[24],
            AUDIO_CAPTURE_FRAMES: uint32View[25],
            AUDIO_CAPTURE_CHANNELS: uint32View[26],
            AUDIO_CAPTURE_SAMPLE_RATE: uint32View[27],
            TOTAL_BUFFER_SIZE: uint32View[28],
            MAX_MESSAGE_SIZE: uint32View[29],
            MESSAGE_MAGIC: uint32View[30],
            PADDING_MAGIC: uint32View[31],
            scheduler_slot_size: uint32View[32],
            scheduler_slot_count: uint32View[33],
            DEBUG_PADDING_MARKER: uint8View[136],  // After 34 uint32s = 136 bytes
            MESSAGE_HEADER_SIZE: 16  // sizeof(Message) - 4 x uint32_t (magic, length, sequence, padding)
        };

        // Validate
        if (this.bufferConstants.MESSAGE_MAGIC !== 0xDEADBEEF) {
            throw new Error('Invalid buffer constants from WASM');
        }
    }

    // Calculate buffer indices based on dynamic ring buffer base address
    // Uses constants loaded from WASM via loadBufferConstants()
    calculateBufferIndices(ringBufferBase) {
        if (!this.bufferConstants) {
            throw new Error('Buffer constants not loaded. Call loadBufferConstants() first.');
        }

        const CONTROL_START = this.bufferConstants.CONTROL_START;
        const METRICS_START = this.bufferConstants.METRICS_START;

        // Calculate Int32Array indices (divide byte offsets by 4)
        this.CONTROL_INDICES = calculateAllControlIndices(ringBufferBase, CONTROL_START);

        // Create views - source depends on mode
        if (this.mode === 'sab') {
            // SAB mode: views into SharedArrayBuffer
            const metricsBase = ringBufferBase + METRICS_START;
            this.metricsView = new Uint32Array(this.sharedBuffer, metricsBase, this.bufferConstants.METRICS_SIZE / 4);
        } else {
            // PostMessage mode: views into WASM memory
            // Note: atomicView/uint8View/dataView are set up here for ring buffer access
            this.atomicView = new Int32Array(this.wasmMemory.buffer);
            this.uint8View = new Uint8Array(this.wasmMemory.buffer);
            this.dataView = new DataView(this.wasmMemory.buffer);
            const metricsBase = ringBufferBase + METRICS_START;
            this.metricsView = new Uint32Array(this.wasmMemory.buffer, metricsBase, this.bufferConstants.METRICS_SIZE / 4);

            // Initialize IN_LOG_TAIL to current IN_HEAD value for postMessage mode logging
            // This ensures we only log messages from this point forward
            const currentHead = this.atomicLoad(this.CONTROL_INDICES.IN_HEAD);
            this.atomicStore(this.CONTROL_INDICES.IN_LOG_TAIL, currentHead);
        }
    }

    // Write worldOptions to SharedArrayBuffer for C++ to read
    // WorldOptions are written after ring buffer storage (65536 bytes)
    writeWorldOptionsToMemory() {
        if (!this.worldOptions || !this.wasmMemory) {
            return;
        }

        // WorldOptions location: ringBufferBase + 65536 (after ring_buffer_storage)
        const WORLD_OPTIONS_OFFSET = this.ringBufferBase + 65536;
        const uint32View = new Uint32Array(this.wasmMemory.buffer, WORLD_OPTIONS_OFFSET, 32);
        const float32View = new Float32Array(this.wasmMemory.buffer, WORLD_OPTIONS_OFFSET, 32);

        // Write worldOptions as uint32/float32 values
        // Order must match C++ reading code in audio_processor.cpp
        uint32View[0] = this.worldOptions.numBuffers || 1024;
        uint32View[1] = this.worldOptions.maxNodes || 1024;
        uint32View[2] = this.worldOptions.maxGraphDefs || 1024;
        uint32View[3] = this.worldOptions.maxWireBufs || 64;
        uint32View[4] = this.worldOptions.numAudioBusChannels || 128;
        uint32View[5] = this.worldOptions.numInputBusChannels || 0;
        uint32View[6] = this.worldOptions.numOutputBusChannels || 0;
        uint32View[7] = this.worldOptions.numControlBusChannels || 4096;
        uint32View[8] = this.worldOptions.bufLength || 128;
        uint32View[9] = this.worldOptions.realTimeMemorySize || 16384;
        uint32View[10] = this.worldOptions.numRGens || 64;
        uint32View[11] = this.worldOptions.realTime ? 1 : 0;
        uint32View[12] = this.worldOptions.memoryLocking ? 1 : 0;
        uint32View[13] = this.worldOptions.loadGraphDefs || 0;
        uint32View[14] = this.worldOptions.preferredSampleRate || 0;
        uint32View[15] = this.worldOptions.verbosity || 0;
        uint32View[16] = this.mode === 'postMessage' ? 1 : 0;  // 0 = SAB, 1 = PM
    }

    // Write debug message to DEBUG ring buffer
    js_debug(message) {
        if (!this.uint8View || !this.atomicView || !this.CONTROL_INDICES || !this.ringBufferBase) {
            return;
        }

        try {
            // Use constants from WASM
            const DEBUG_BUFFER_START = this.bufferConstants.DEBUG_BUFFER_START;
            const DEBUG_BUFFER_SIZE = this.bufferConstants.DEBUG_BUFFER_SIZE;
            const DEBUG_PADDING_MARKER = this.bufferConstants.DEBUG_PADDING_MARKER;

            const prefixedMessage = '[JS] ' + message + '\n';
            const encoder = new TextEncoder();
            const bytes = encoder.encode(prefixedMessage);

            // Drop message if too large for buffer
            if (bytes.length > DEBUG_BUFFER_SIZE) {
                return;
            }

            const debugHeadIndex = this.CONTROL_INDICES.DEBUG_HEAD;
            const currentHead = this.atomicLoad(debugHeadIndex);
            const spaceToEnd = DEBUG_BUFFER_SIZE - currentHead;

            let writePos = currentHead;
            if (bytes.length > spaceToEnd) {
                // Message won't fit - write padding marker and wrap to beginning
                this.uint8View[this.ringBufferBase + DEBUG_BUFFER_START + currentHead] = DEBUG_PADDING_MARKER;
                writePos = 0;
            }

            // Write message (now guaranteed to fit contiguously)
            const debugBufferStart = this.ringBufferBase + DEBUG_BUFFER_START;
            for (let i = 0; i < bytes.length; i++) {
                this.uint8View[debugBufferStart + writePos + i] = bytes[i];
            }

            // Update head pointer (publish message)
            const newHead = writePos + bytes.length;
            this.atomicStore(debugHeadIndex, newHead);
            // Notify waiting debug worker that new data is available
            if (this.mode === 'sab') {
                Atomics.notify(this.atomicView, debugHeadIndex, 1);
            }
        } catch (err) {
            // Silently fail in real-time audio context
        }
    }

    // Atomic-safe load - uses Atomics in SAB mode, regular access in postMessage mode
    atomicLoad(index) {
        if (this.mode === 'sab') {
            return Atomics.load(this.atomicView, index);
        } else {
            return this.atomicView[index];
        }
    }

    // Atomic-safe store - uses Atomics in SAB mode, regular access in postMessage mode
    atomicStore(index, value) {
        if (this.mode === 'sab') {
            Atomics.store(this.atomicView, index, value);
        } else {
            this.atomicView[index] = value;
        }
    }

    // Initialize pre-allocated pools for allocation-free PM mode
    // Called once after mode is set and bufferConstants are loaded
    initPMPools() {
        if (this.mode !== 'postMessage') return;

        const C = PM_POOL_CONFIG;

        this.pmPools = {
            // === OUTGOING POOLS ===

            // OSC replies from scsynth
            replies: {
                message: { type: 'oscReplies', messages: null, count: 0 },
                buffer: new ArrayBuffer(C.REPLY_BUFFER_SIZE),
                bufferView: null,
                entries: new Array(C.MAX_REPLY_MESSAGES).fill(null).map(() => ({
                    offset: 0,
                    length: 0,
                    sequence: 0
                })),
            },

            // Debug messages
            debug: {
                message: { type: 'debugRawBatch', messages: null, count: 0 },
                buffer: new ArrayBuffer(C.DEBUG_BUFFER_SIZE),
                bufferView: null,
                entries: new Array(C.MAX_DEBUG_MESSAGES).fill(null).map(() => ({
                    offset: 0,
                    length: 0,
                    sequence: 0
                })),
            },

            // Metrics + node tree snapshot
            snapshot: {
                message: { type: 'snapshot', buffer: null, snapshotsSent: 0 },
                buffer: null,       // Sized after bufferConstants known
                bufferView: null,
                size: 0,
            },

            // OSC log entries
            log: {
                message: { type: 'oscLog', entries: null, count: 0, buffer: null },
                buffer: new ArrayBuffer(C.LOG_BUFFER_SIZE),
                bufferView: null,
                entries: new Array(C.MAX_LOG_ENTRIES).fill(null).map(() => ({
                    offset: 0,
                    length: 0,
                    originalLength: 0,
                    sourceId: 0,
                    sequence: 0
                })),
            },

            // === INCOMING POOLS ===
            incoming: {
                headerBytes: new Uint8Array(16),  // MESSAGE_HEADER_SIZE
                headerView: null,
            },
        };

        const p = this.pmPools;

        // Create views for outgoing buffers
        p.replies.bufferView = new Uint8Array(p.replies.buffer);
        p.debug.bufferView = new Uint8Array(p.debug.buffer);
        p.log.bufferView = new Uint8Array(p.log.buffer);

        // Wire up message.messages to entries arrays
        p.replies.message.messages = p.replies.entries;
        p.debug.message.messages = p.debug.entries;
        p.log.message.entries = p.log.entries;

        // Incoming header view
        p.incoming.headerView = new DataView(p.incoming.headerBytes.buffer);

        // Snapshot buffer (needs bufferConstants and wasmMemory)
        if (this.bufferConstants && this.wasmMemory) {
            const bc = this.bufferConstants;
            const size = bc.METRICS_SIZE + bc.NODE_TREE_SIZE;
            p.snapshot.buffer = new ArrayBuffer(size);
            p.snapshot.bufferView = new Uint8Array(p.snapshot.buffer);
            p.snapshot.size = size;
            p.snapshot.message.buffer = p.snapshot.buffer;

            // Pre-allocate source view for the METRICS+NODE_TREE region
            // This view remains valid as long as WASM memory doesn't grow
            // (SuperSonic uses fixed memory size, so this is safe)
            const metricsBase = this.ringBufferBase + bc.METRICS_START;
            p.snapshot.sourceView = new Uint8Array(this.wasmMemory.buffer, metricsBase, size);
        }
    }

    // Write a single OSC message directly to the IN ring buffer (postMessage mode)
    // Called from onmessage handlers - uses pre-allocated header scratch for wrap-around case
    // Note: new Uint8Array(oscData) creates a view (no copy), which is required to access ArrayBuffer bytes
    writeOscToRingBuffer(oscData, sourceId = 0) {
        if (!this.bufferConstants || !this.uint8View || !this.pmPools) return false;

        const IN_BUFFER_SIZE = this.bufferConstants.IN_BUFFER_SIZE;
        const MESSAGE_HEADER_SIZE = this.bufferConstants.MESSAGE_HEADER_SIZE;
        const bufferStart = this.ringBufferBase + this.bufferConstants.IN_BUFFER_START;

        const messageLength = oscData.byteLength;
        const totalLength = MESSAGE_HEADER_SIZE + messageLength;
        const alignedLength = (totalLength + 3) & ~3;  // Align to 4 bytes

        // Get current head/tail
        const head = this.atomicLoad(this.CONTROL_INDICES.IN_HEAD);
        const tail = this.atomicLoad(this.CONTROL_INDICES.IN_TAIL);

        // Calculate available space using shared helper
        const available = calculateAvailableSpace(head, tail, IN_BUFFER_SIZE);

        // Check if message fits
        if (alignedLength > available) {
            // Buffer full - message is dropped
            // This shouldn't happen with proper backpressure
            console.error('[AudioWorklet] Ring buffer full, dropping OSC message');
            return false;
        }

        // Get and increment sequence number
        const sequence = this.atomicLoad(this.CONTROL_INDICES.IN_SEQUENCE);
        this.atomicStore(this.CONTROL_INDICES.IN_SEQUENCE, sequence + 1);

        // Create view over incoming ArrayBuffer (view only, no data copy)
        // This allocation is unavoidable - JS requires TypedArray to access ArrayBuffer bytes
        const oscBytes = new Uint8Array(oscData);

        // Write message using shared core logic with pre-allocated header scratch
        const newHead = writeMessageToBuffer({
            uint8View: this.uint8View,
            dataView: this.dataView,
            bufferStart,
            bufferSize: IN_BUFFER_SIZE,
            head,
            payload: oscBytes,
            sequence,
            messageMagic: this.bufferConstants.MESSAGE_MAGIC,
            headerSize: MESSAGE_HEADER_SIZE,
            sourceId,
            headerScratch: this.pmPools.incoming.headerBytes,
            headerScratchView: this.pmPools.incoming.headerView
        });

        // Update head
        this.atomicStore(this.CONTROL_INDICES.IN_HEAD, newHead);
        return true;
    }

    // Note: SAB mode OSC logging is now handled by osc_out_log_sab_worker
    // The worker uses Atomics.wait() on IN_HEAD for instant wake when messages arrive

    // Read OSC replies from OUT ring buffer and send via postMessage
    // Uses pre-allocated pools for allocation-free operation
    readOscReplies() {
        if (!this.pmPools) return;

        const head = this.atomicLoad(this.CONTROL_INDICES.OUT_HEAD);
        const tail = this.atomicLoad(this.CONTROL_INDICES.OUT_TAIL);

        if (head === tail) return;

        const pool = this.pmPools.replies;
        const C = PM_POOL_CONFIG;
        let count = 0;
        let bufferOffset = 0;

        const { newTail, messagesRead } = readMessagesFromBuffer({
            uint8View: this.uint8View,
            dataView: this.dataView,
            bufferStart: this.ringBufferBase + this.bufferConstants.OUT_BUFFER_START,
            bufferSize: this.bufferConstants.OUT_BUFFER_SIZE,
            head,
            tail,
            messageMagic: this.bufferConstants.MESSAGE_MAGIC,
            paddingMagic: this.bufferConstants.PADDING_MAGIC,
            headerSize: this.bufferConstants.MESSAGE_HEADER_SIZE,
            maxMessages: C.MAX_REPLY_MESSAGES,
            onMessage: (payloadOffset, payloadLength, sequence, sourceId) => {
                if (count >= C.MAX_REPLY_MESSAGES) return;
                if (bufferOffset + payloadLength > C.REPLY_BUFFER_SIZE) return;

                // Copy directly from source to pool - NO intermediate allocation
                for (let i = 0; i < payloadLength; i++) {
                    pool.bufferView[bufferOffset + i] = this.uint8View[payloadOffset + i];
                }

                // Update pre-allocated entry
                const entry = pool.entries[count];
                entry.offset = bufferOffset;
                entry.length = payloadLength;
                entry.sequence = sequence;

                bufferOffset += payloadLength;
                count++;
            }
        });

        // Update tail
        if (messagesRead > 0) {
            this.atomicStore(this.CONTROL_INDICES.OUT_TAIL, newTail);
        }

        // Send via postMessage (structured clone - pool remains valid for reuse)
        if (count > 0) {
            pool.message.count = count;
            pool.message.buffer = pool.buffer;
            this.port.postMessage(pool.message);
        }
    }

    // Read node tree from WASM memory and return the tree data
    // Returns null if not ready, otherwise { nodeCount, version, nodes }
    readNodeTree() {
        if (!this.bufferConstants || !this.wasmMemory || this.ringBufferBase === null) {
            return null;
        }

        const bc = this.bufferConstants;
        const treeBase = this.ringBufferBase + bc.NODE_TREE_START;

        // Read header (nodeCount, version)
        const headerView = new Uint32Array(this.wasmMemory.buffer, treeBase, 2);
        const nodeCount = headerView[0];
        const version = headerView[1];

        // Read node entries
        const entriesBase = treeBase + bc.NODE_TREE_HEADER_SIZE;
        const maxNodes = bc.NODE_TREE_MIRROR_MAX_NODES;
        const entrySize = bc.NODE_TREE_ENTRY_SIZE; // 56 bytes
        const defNameSize = bc.NODE_TREE_DEF_NAME_SIZE; // 32 bytes

        const dataView = new DataView(this.wasmMemory.buffer, entriesBase, maxNodes * entrySize);

        // Collect non-empty entries
        const nodes = [];
        let foundCount = 0;
        for (let i = 0; i < maxNodes && foundCount < nodeCount; i++) {
            const byteOffset = i * entrySize;
            const id = dataView.getInt32(byteOffset, true);
            if (id === -1) continue; // Empty slot
            foundCount++;

            // Read def_name (32 bytes starting at byte 24)
            const defNameStart = entriesBase + byteOffset + 24;
            const defNameBytes = new Uint8Array(this.wasmMemory.buffer, defNameStart, defNameSize);
            // Find null terminator and convert to string
            let defName = '';
            for (let j = 0; j < defNameSize && defNameBytes[j] !== 0; j++) {
                defName += String.fromCharCode(defNameBytes[j]);
            }

            nodes.push({
                id,
                parentId: dataView.getInt32(byteOffset + 4, true),
                isGroup: dataView.getInt32(byteOffset + 8, true) === 1,
                prevId: dataView.getInt32(byteOffset + 12, true),
                nextId: dataView.getInt32(byteOffset + 16, true),
                headId: dataView.getInt32(byteOffset + 20, true),
                defName
            });
        }

        return { nodeCount, version, nodes };
    }

    // Read metrics from WASM memory as raw Uint32Array
    // Returns null if not ready, otherwise a copy of the metrics buffer
    // Same layout as SAB - can be used directly with MetricsOffsets
    readMetrics() {
        if (!this.metricsView) {
            return null;
        }
        // Return a copy of the raw buffer (same layout as SAB)
        return new Uint32Array(this.metricsView);
    }

    // Record OSC message received (for postMessage mode metrics)
    // In PM mode, we track what the worklet receives since there's no shared memory
    // bypassCategory is only provided in PM mode when sent via OscChannel
    recordOscReceived(byteLength, bypassCategory = null) {
        if (!this.metricsView) return;

        if (this.mode === 'sab') {
            // SAB mode: use atomic operations
            Atomics.add(this.metricsView, MetricsOffsets.OSC_OUT_MESSAGES_SENT, 1);
            Atomics.add(this.metricsView, MetricsOffsets.OSC_OUT_BYTES_SENT, byteLength);
        } else {
            // PM mode: direct increment (single-threaded context)
            this.metricsView[MetricsOffsets.OSC_OUT_MESSAGES_SENT]++;
            this.metricsView[MetricsOffsets.OSC_OUT_BYTES_SENT] += byteLength;

            // Track bypass category if provided (from OscChannel workers)
            if (bypassCategory) {
                this.metricsView[MetricsOffsets.PRESCHEDULER_BYPASSED]++;
                const categoryOffsets = {
                    nonBundle: MetricsOffsets.BYPASS_NON_BUNDLE,
                    immediate: MetricsOffsets.BYPASS_IMMEDIATE,
                    nearFuture: MetricsOffsets.BYPASS_NEAR_FUTURE,
                    late: MetricsOffsets.BYPASS_LATE,
                };
                const offset = categoryOffsets[bypassCategory];
                if (offset !== undefined) {
                    this.metricsView[offset]++;
                }
            }
        }
    }

    // Read metrics + node tree from WASM memory and send via postMessage
    // Sends immediately on tree version change, OR on interval (for metrics updates)
    // METRICS and NODE_TREE are contiguous in memory - copy as one atomic unit
    // Uses pre-allocated pool for allocation-free operation
    // Returns true if snapshot was sent (used to batch log entries on same interval)
    checkAndSendSnapshot(audioTime) {
        // Check if tree version changed (need to read header to check)
        const bc = this.bufferConstants;
        if (!bc || !this.wasmMemory || this.ringBufferBase === null || !this.pmPools) return false;

        const treeBase = this.ringBufferBase + bc.NODE_TREE_START;
        // Reuse existing atomicView to avoid creating new typed array
        const versionOffset = (treeBase + 4) / 4;  // version is second uint32 in header
        const currentVersion = this.atomicView[versionOffset];
        const versionChanged = currentVersion !== this.lastTreeVersion;

        if (versionChanged) {
            // Tree changed - send immediately and reset timer
            this.lastTreeVersion = currentVersion;
            this.lastTreeSendTime = audioTime;
        } else {
            // No tree change - check if interval has elapsed (for metrics updates)
            if (this.lastTreeSendTime >= 0 && audioTime - this.lastTreeSendTime < this.treeSnapshotMinInterval) {
                return false; // Skip this frame, will send on next interval
            }
            this.lastTreeSendTime = audioTime;
        }

        const pool = this.pmPools.snapshot;
        if (!pool.buffer || !pool.sourceView) return false;

        // Copy METRICS + NODE_TREE using pre-allocated source view - NO allocation
        pool.bufferView.set(pool.sourceView);

        // Send via postMessage (structured clone - pool remains valid for reuse)
        this.treeSnapshotsSent++;
        pool.message.snapshotsSent = this.treeSnapshotsSent;
        this.port.postMessage(pool.message);
        return true;
    }

    // Read metrics + node tree as one contiguous memory copy
    // Returns raw ArrayBuffer (transferable) or null if not ready
    // Used only for initial snapshot during initialization
    readMetricsAndTreeBuffer() {
        if (!this.bufferConstants || !this.wasmMemory || this.ringBufferBase === null) {
            return null;
        }

        const bc = this.bufferConstants;

        // Single memcopy of entire contiguous region: METRICS + NODE_TREE
        const metricsBase = this.ringBufferBase + bc.METRICS_START;
        const totalSize = bc.METRICS_SIZE + bc.NODE_TREE_SIZE;
        const view = new Uint8Array(this.wasmMemory.buffer, metricsBase, totalSize);

        // Copy to new buffer (will be transferred)
        const buffer = new ArrayBuffer(totalSize);
        new Uint8Array(buffer).set(view);

        return buffer;
    }

    // Read and send OSC log entries from IN ring buffer (postMessage mode)
    // Called on snapshot heartbeat (~150ms) to batch entries and reduce postMessage frequency
    // Uses IN_LOG_TAIL to track what's been logged vs IN_HEAD for new messages
    // Uses pre-allocated pools for allocation-free operation, with truncation for large messages
    sendLogEntries() {
        if (!this.CONTROL_INDICES || !this.bufferConstants || !this.pmPools) return;

        const head = this.atomicLoad(this.CONTROL_INDICES.IN_HEAD);
        const logTail = this.atomicLoad(this.CONTROL_INDICES.IN_LOG_TAIL);

        if (head === logTail) return; // No new messages to log

        const pool = this.pmPools.log;
        const C = PM_POOL_CONFIG;
        let count = 0;
        let bufferOffset = 0;

        const { newTail, messagesRead } = readMessagesFromBuffer({
            uint8View: this.uint8View,
            dataView: this.dataView,
            bufferStart: this.ringBufferBase + this.bufferConstants.IN_BUFFER_START,
            bufferSize: this.bufferConstants.IN_BUFFER_SIZE,
            head,
            tail: logTail,
            messageMagic: this.bufferConstants.MESSAGE_MAGIC,
            paddingMagic: this.bufferConstants.PADDING_MAGIC,
            headerSize: this.bufferConstants.MESSAGE_HEADER_SIZE,
            maxMessages: C.MAX_LOG_ENTRIES,
            onMessage: (payloadOffset, payloadLength, sequence, sourceId) => {
                if (count >= C.MAX_LOG_ENTRIES) return;

                // Truncate large messages (e.g., buffer dumps) to LOG_MAX_MESSAGE_SIZE
                const actualLength = Math.min(payloadLength, C.LOG_MAX_MESSAGE_SIZE);

                // Check if pool buffer has space
                if (bufferOffset + actualLength > C.LOG_BUFFER_SIZE) return;

                // Copy (possibly truncated) directly from source - NO intermediate allocation
                for (let i = 0; i < actualLength; i++) {
                    pool.bufferView[bufferOffset + i] = this.uint8View[payloadOffset + i];
                }

                // Update pre-allocated entry with truncation info
                const entry = pool.entries[count];
                entry.offset = bufferOffset;
                entry.length = actualLength;
                entry.originalLength = payloadLength;  // Receiver can detect truncation
                entry.sourceId = sourceId ?? 0;
                entry.sequence = sequence;

                bufferOffset += actualLength;
                count++;
            }
        });

        // Update log tail pointer (mark messages as logged)
        if (messagesRead > 0) {
            this.atomicStore(this.CONTROL_INDICES.IN_LOG_TAIL, newTail);
        }

        // Send via postMessage (structured clone - pool remains valid for reuse)
        if (count > 0) {
            pool.message.count = count;
            pool.message.buffer = pool.buffer;
            this.port.postMessage(pool.message);
        }
    }

    // Read debug messages from DEBUG ring buffer and send via postMessage
    // Uses pre-allocated pools for allocation-free operation
    readDebugMessages() {
        if (!this.pmPools) return;

        const head = this.atomicLoad(this.CONTROL_INDICES.DEBUG_HEAD);
        const tail = this.atomicLoad(this.CONTROL_INDICES.DEBUG_TAIL);

        if (head === tail) return;

        const pool = this.pmPools.debug;
        const C = PM_POOL_CONFIG;
        let count = 0;
        let bufferOffset = 0;

        const { newTail, messagesRead } = readMessagesFromBuffer({
            uint8View: this.uint8View,
            dataView: this.dataView,
            bufferStart: this.ringBufferBase + this.bufferConstants.DEBUG_BUFFER_START,
            bufferSize: this.bufferConstants.DEBUG_BUFFER_SIZE,
            head,
            tail,
            messageMagic: this.bufferConstants.MESSAGE_MAGIC,
            paddingMagic: this.bufferConstants.PADDING_MAGIC,
            headerSize: this.bufferConstants.MESSAGE_HEADER_SIZE,
            maxMessages: C.MAX_DEBUG_MESSAGES,
            onMessage: (payloadOffset, payloadLength, sequence, sourceId) => {
                if (count >= C.MAX_DEBUG_MESSAGES) return;
                if (bufferOffset + payloadLength > C.DEBUG_BUFFER_SIZE) return;

                // Copy directly from source to pool - NO intermediate allocation
                for (let i = 0; i < payloadLength; i++) {
                    pool.bufferView[bufferOffset + i] = this.uint8View[payloadOffset + i];
                }

                // Update pre-allocated entry
                const entry = pool.entries[count];
                entry.offset = bufferOffset;
                entry.length = payloadLength;
                entry.sequence = sequence;

                bufferOffset += payloadLength;
                count++;
            }
        });

        // Update tail
        if (messagesRead > 0) {
            this.atomicStore(this.CONTROL_INDICES.DEBUG_TAIL, newTail);
        }

        // Send via postMessage (structured clone - pool remains valid for reuse)
        if (count > 0) {
            pool.message.count = count;
            pool.message.buffer = pool.buffer;
            this.port.postMessage(pool.message);
        }
    }

    async handleMessage(event) {
        const { data } = event;


        try {
            // Handle OSC messages (postMessage mode)
            if (data.type === 'osc') {
                if (this.mode === 'postMessage') {
                    // Write OSC message directly to ring buffer (no allocation in process())
                    if (data.oscData) {
                        this.writeOscToRingBuffer(data.oscData, data.sourceId ?? 0);
                        this.recordOscReceived(data.oscData.byteLength, data.bypassCategory);
                    }
                }
                return;
            }

            // Handle adding a new OSC input port (for prescheduler or user workers)
            // This allows workers to send OSC directly to the worklet via MessageChannel
            if (data.type === 'addOscPort') {
                const port = event.ports[0];
                if (port) {
                    // Extract sourceId from message (assigned by transport when creating OscChannel)
                    const portSourceId = data.sourceId ?? 0;
                    this.portSourceIds.set(port, portSourceId);

                    port.onmessage = (e) => {
                        if (e.data.type === 'osc' && e.data.oscData) {
                            // Write OSC message directly to ring buffer (no allocation in process())
                            // Use sourceId from message or port's assigned sourceId
                            const msgSourceId = e.data.sourceId ?? this.portSourceIds.get(port) ?? 0;
                            this.writeOscToRingBuffer(e.data.oscData, msgSourceId);
                            // Debug: log bypass category receipt
                            if (__DEV__ && e.data.bypassCategory) {
                                console.log('[Worklet] OSC via addOscPort, bypassCategory:', e.data.bypassCategory);
                            }
                            this.recordOscReceived(e.data.oscData.byteLength, e.data.bypassCategory);
                        }
                    };
                    this.oscPorts.push(port);
                }
                return;
            }

            if (data.type === 'clearSched') {
                // Drain the IN ring buffer immediately — discard all stale messages
                // that accumulated while the AudioContext was suspended (e.g. mobile
                // tab switch). This must happen eagerly (not deferred to process())
                // so that new messages sent after purge() resolves are not affected.
                // handleMessage and process() both run on the audio thread, so there
                // is no race with the C++ ring buffer consumer.
                if (this.CONTROL_INDICES) {
                    const head = this.atomicLoad(this.CONTROL_INDICES.IN_HEAD);
                    this.atomicStore(this.CONTROL_INDICES.IN_TAIL, head);
                }

                // Set flag to clear the WASM scheduler on next process() call.
                // The scheduler may contain bundles already dequeued from the ring
                // buffer before the drain — these must also be discarded.
                this.pendingClearSched = true;

                if (data.ack) {
                    this.port.postMessage({ type: 'clearSchedAck' });
                }
                return;
            }

            if (data.type === 'init') {
                // Set mode from init message
                this.mode = data.mode || 'sab';

                // Set snapshot interval (postMessage mode) - convert ms to seconds for AudioContext time
                if (data.snapshotIntervalMs) {
                    this.treeSnapshotMinInterval = data.snapshotIntervalMs / 1000;
                }

                if (this.mode === 'sab' && data.sharedBuffer) {
                    // SAB mode: receive SharedArrayBuffer
                    this.sharedBuffer = data.sharedBuffer;
                    this.atomicView = new Int32Array(this.sharedBuffer);
                    this.uint8View = new Uint8Array(this.sharedBuffer);
                    this.dataView = new DataView(this.sharedBuffer);
                }
                // PostMessage mode: memory will be created locally in loadWasm
            }

            if (data.type === 'loadWasm') {
                // Load WASM module (standalone version)
                if (data.wasmBytes) {
                    let memory;

                    if (this.mode === 'sab') {
                        // SAB mode: use the memory passed from orchestrator
                        memory = data.wasmMemory;
                        if (!memory) {
                            this.port.postMessage({
                                type: 'error',
                                error: 'No WASM memory provided!'
                            });
                            return;
                        }
                    } else {
                        // PostMessage mode: create memory locally
                        // Note: WASM was compiled with --shared-memory, so we must use shared: true
                        // The memory just isn't shared with the main thread in this mode
                        const memoryPages = data.memoryPages || 1280;  // 80MB default
                        memory = new WebAssembly.Memory({
                            initial: memoryPages,
                            maximum: memoryPages,
                            shared: true
                        });
                    }

                    // Save memory reference for later use (WASM imports memory, doesn't export it)
                    this.wasmMemory = memory;

                    // Store worldOptions and sampleRate for C++ initialization
                    this.worldOptions = data.worldOptions || {};
                    this.sampleRate = data.sampleRate || 48000;  // Fallback to 48000 if not provided

                    // Import object for WASM
                    // scsynth with pthread support requires these imports
                    // (pthread stubs are no-ops - AudioWorklet is single-threaded)
                    const imports = {
                        env: {
                            memory: memory,
                            // Time
                            emscripten_asm_const_double: () => Date.now() * 1000,
                            // Filesystem syscalls
                            __syscall_getdents64: () => 0,
                            __syscall_unlinkat: () => 0,
                            // pthread stubs (no-ops - AudioWorklet doesn't support threading)
                            _emscripten_init_main_thread_js: () => {},
                            _emscripten_thread_mailbox_await: () => {},
                            _emscripten_thread_set_strongref: () => {},
                            emscripten_exit_with_live_runtime: () => {},
                            _emscripten_receive_on_main_thread_js: () => {},
                            emscripten_check_blocking_allowed: () => {},
                            _emscripten_thread_cleanup: () => {},
                            emscripten_num_logical_cores: () => 1,  // Report 1 core
                            _emscripten_notify_mailbox_postmessage: () => {}
                        },
                        wasi_snapshot_preview1: {
                            clock_time_get: (clockid, precision, timestamp_ptr) => {
                                const view = new DataView(memory.buffer);
                                const nanos = BigInt(Math.floor(Date.now() * 1000000));
                                view.setBigUint64(timestamp_ptr, nanos, true);
                                return 0;
                            },
                            environ_sizes_get: () => 0,
                            environ_get: () => 0,
                            fd_close: () => 0,
                            fd_write: () => 0,
                            fd_seek: () => 0,
                            fd_read: () => 0,
                            proc_exit: (code) => {
                                console.error('[AudioWorklet] WASM tried to exit with code:', code);
                            }
                        }
                    };

                    // Compile and instantiate WASM
                    const module = await WebAssembly.compile(data.wasmBytes);
                    this.wasmInstance = await WebAssembly.instantiate(module, imports);

                    // Get the ring buffer base address from WASM
                    if (this.wasmInstance.exports.get_ring_buffer_base) {
                        this.ringBufferBase = this.wasmInstance.exports.get_ring_buffer_base();

                        // Load buffer constants from WASM (single source of truth)
                        this.loadBufferConstants();

                        this.calculateBufferIndices(this.ringBufferBase);

                        // Initialize PM mode pools after bufferConstants are known
                        this.initPMPools();

                        // Write worldOptions to SharedArrayBuffer for C++ to read
                        this.writeWorldOptionsToMemory();

                        // Initialize WASM memory
                        if (this.wasmInstance.exports.init_memory) {
                            // Pass actual sample rate from AudioContext (not hardcoded!)
                            this.wasmInstance.exports.init_memory(this.sampleRate);

                            this.isInitialized = true;

                            // Include initial snapshot buffer for postMessage mode
                            const initialSnapshot = this.mode === 'postMessage' ? this.readMetricsAndTreeBuffer() : undefined;

                            const msg = {
                                type: 'initialized',
                                success: true,
                                ringBufferBase: this.ringBufferBase,
                                bufferConstants: this.bufferConstants,
                                exports: Object.keys(this.wasmInstance.exports),
                                initialSnapshot
                            };
                            // Transfer the buffer if present
                            this.port.postMessage(msg, initialSnapshot ? [initialSnapshot] : []);
                        }
                    }
                } else if (data.wasmInstance) {
                    // Pre-instantiated WASM (from Emscripten)
                    this.wasmInstance = data.wasmInstance;

                    // Get the ring buffer base address from WASM
                    if (this.wasmInstance.exports.get_ring_buffer_base) {
                        this.ringBufferBase = this.wasmInstance.exports.get_ring_buffer_base();

                        // Load buffer constants from WASM (single source of truth)
                        this.loadBufferConstants();

                        this.calculateBufferIndices(this.ringBufferBase);

                        // Initialize PM mode pools after bufferConstants are known
                        this.initPMPools();

                        // Write worldOptions to SharedArrayBuffer for C++ to read
                        this.writeWorldOptionsToMemory();

                        // Initialize WASM memory
                        if (this.wasmInstance.exports.init_memory) {
                            // Pass actual sample rate from AudioContext (not hardcoded!)
                            this.wasmInstance.exports.init_memory(this.sampleRate);

                            this.isInitialized = true;

                            // Include initial snapshot buffer for postMessage mode
                            const initialSnapshot = this.mode === 'postMessage' ? this.readMetricsAndTreeBuffer() : undefined;

                            const msg = {
                                type: 'initialized',
                                success: true,
                                ringBufferBase: this.ringBufferBase,
                                bufferConstants: this.bufferConstants,
                                exports: Object.keys(this.wasmInstance.exports),
                                initialSnapshot
                            };
                            // Transfer the buffer if present
                            this.port.postMessage(msg, initialSnapshot ? [initialSnapshot] : []);
                        }
                    }
                }
            }

            if (data.type === 'getVersion') {
                // Return Supersonic/SuperCollider version string
                if (this.wasmInstance && this.wasmInstance.exports.get_supersonic_version_string) {
                    const versionPtr = this.wasmInstance.exports.get_supersonic_version_string();
                    const memory = new Uint8Array(this.wasmMemory.buffer);
                    // Read null-terminated C string
                    let version = '';
                    for (let i = versionPtr; memory[i] !== 0; i++) {
                        version += String.fromCharCode(memory[i]);
                    }
                    this.port.postMessage({
                        type: 'version',
                        version: version
                    });
                } else {
                    this.port.postMessage({
                        type: 'version',
                        version: 'unknown'
                    });
                }
            }

            if (data.type === 'getTimeOffset') {
                // Return time offset (NTP seconds when AudioContext was at 0)
                if (this.wasmInstance && this.wasmInstance.exports.get_time_offset) {
                    const offset = this.wasmInstance.exports.get_time_offset();
                    this.port.postMessage({
                        type: 'timeOffset',
                        offset: offset
                    });
                } else {
                    console.error('[AudioWorklet] get_time_offset not available! wasmInstance:', !!this.wasmInstance);
                    this.port.postMessage({
                        type: 'error',
                        error: 'get_time_offset function not available in WASM exports'
                    });
                }
            }

            // Timing API for postMessage mode
            if (data.type === 'setNTPStartTime') {
                // Write NTP start time to WASM memory (Float64)
                if (this.wasmMemory && this.ringBufferBase !== null && this.bufferConstants) {
                    const offset = this.ringBufferBase + this.bufferConstants.NTP_START_TIME_START;
                    const view = new Float64Array(this.wasmMemory.buffer, offset, 1);
                    view[0] = data.ntpStartTime;
                }
            }

            if (data.type === 'setDriftOffset') {
                // Write drift offset to WASM memory (Int32, milliseconds)
                if (this.wasmMemory && this.ringBufferBase !== null && this.bufferConstants) {
                    const offset = this.ringBufferBase + this.bufferConstants.DRIFT_OFFSET_START;
                    const view = new Int32Array(this.wasmMemory.buffer, offset, 1);
                    view[0] = data.driftOffsetMs;
                }
            }

            if (data.type === 'setClockOffset') {
                // Write clock offset to WASM memory (Int32, milliseconds)
                if (this.wasmMemory && this.ringBufferBase !== null && this.bufferConstants) {
                    const offset = this.ringBufferBase + this.bufferConstants.GLOBAL_OFFSET_START;
                    const view = new Int32Array(this.wasmMemory.buffer, offset, 1);
                    view[0] = data.clockOffsetMs;
                }
            }

            if (data.type === 'getMetrics') {
                // Return raw metrics buffer for postMessage mode
                // Same layout as SAB - can be used directly with MetricsOffsets
                const metrics = this.metricsView ? new Uint32Array(this.metricsView) : null;
                this.port.postMessage({
                    type: 'metricsSnapshot',
                    requestId: data.requestId,
                    metrics: metrics
                });
            }

            // Handle buffer data copy (postMessage mode sample loading)
            if (data.type === 'copyBufferData') {
                try {
                    const { copyId, ptr, data: bufferData } = data;

                    if (!this.wasmMemory || !this.wasmMemory.buffer) {
                        throw new Error('WASM memory not initialized');
                    }

                    // Copy the Float32 data to WASM memory at the specified offset
                    const floatData = new Float32Array(bufferData);
                    const wasmFloatView = new Float32Array(this.wasmMemory.buffer, ptr, floatData.length);
                    wasmFloatView.set(floatData);

                    if (__DEV__) {
                        console.log(`[AudioWorklet] Copied ${floatData.length} samples to WASM memory at offset ${ptr}`);
                    }

                    this.port.postMessage({
                        type: 'bufferCopied',
                        copyId: copyId,
                        success: true
                    });
                } catch (copyError) {
                    console.error('[AudioWorklet] Buffer copy failed:', copyError);
                    this.port.postMessage({
                        type: 'bufferCopied',
                        copyId: data.copyId,
                        success: false,
                        error: copyError.message
                    });
                }
            }

        } catch (error) {
            console.error('[AudioWorklet] Error handling message:', error);
            this.port.postMessage({
                type: 'error',
                error: error.message,
                stack: error.stack
            });
        }
    }

    process(inputs, outputs, parameters) {
        this.processCallCount++;

        if (!this.isInitialized) {
            return true;
        }

        try {
            if (this.wasmInstance && this.wasmInstance.exports.process_audio) {

                // Clear WASM scheduler if flagged (before process_audio runs scheduled bundles)
                if (this.pendingClearSched) {
                    this.pendingClearSched = false;
                    if (this.wasmInstance.exports.clear_scheduler) {
                        this.wasmInstance.exports.clear_scheduler();
                    }
                }

                // CRITICAL: Access AudioContext currentTime correctly
                // In AudioWorkletGlobalScope, currentTime is a bare global variable (not on globalThis)
                // We use a different variable name to avoid shadowing
                const audioContextTime = currentTime;  // Access the global currentTime directly

                // Copy WebAudio input to scsynth input buses (before processing)
                const inputChannels = inputs[0]?.length || 0;
                const outputChannels = outputs[0]?.length || 0;

                if (inputChannels > 0 && this.wasmInstance?.exports?.get_audio_input_bus) {
                    try {
                        const inputBusPtr = this.wasmInstance.exports.get_audio_input_bus();
                        const numSamples = this.wasmInstance.exports.get_audio_buffer_samples();

                        if (inputBusPtr && inputBusPtr > 0) {
                            const memBuffer = this.sharedBuffer || this.wasmMemory?.buffer;
                            if (memBuffer) {
                                // Use configured input channels or actual input channels
                                const configuredChannels = this.worldOptions?.numInputBusChannels || 2;
                                const effectiveChannels = Math.min(inputChannels, configuredChannels);

                                // Reuse input view if possible to avoid allocation in hot path
                                if (!this.inputView ||
                                    this.lastInputBusPtr !== inputBusPtr ||
                                    this.lastInputChannels !== configuredChannels) {
                                    this.inputView = new Float32Array(memBuffer, inputBusPtr, numSamples * configuredChannels);
                                    this.lastInputBusPtr = inputBusPtr;
                                    this.lastInputChannels = configuredChannels;
                                }

                                // Copy each input channel to scsynth's input buses
                                for (let ch = 0; ch < effectiveChannels; ch++) {
                                    if (inputs[0]?.[ch]) {
                                        this.inputView.set(inputs[0][ch], ch * numSamples);
                                    }
                                }
                            }
                        }
                    } catch (err) {
                        // Silently fail in real-time audio context
                    }
                }

                // C++ process_audio() now calculates NTP time internally from:
                // - NTP_START_TIME (write-once, set during initialization)
                // - DRIFT_OFFSET (updated every 15s by main thread)
                // - GLOBAL_OFFSET (for future multi-system sync)

                const keepAlive = this.wasmInstance.exports.process_audio(
                    audioContextTime,
                    outputChannels,
                    inputChannels
                );

                // Copy scsynth audio output to AudioWorklet outputs
                if (this.wasmInstance.exports.get_audio_output_bus && outputs[0] && outputs[0].length >= 1) {
                    try {
                        const audioBufferPtr = this.wasmInstance.exports.get_audio_output_bus();
                        const numSamples = this.wasmInstance.exports.get_audio_buffer_samples();

                        if (audioBufferPtr && audioBufferPtr > 0) {
                            const wasmMemory = this.wasmInstance.exports.memory || this.wasmMemory;

                            if (!wasmMemory || !wasmMemory.buffer) {
                                return true;
                            }

                            const currentBuffer = wasmMemory.buffer;
                            const bufferSize = currentBuffer.byteLength;
                            const configuredOutputChannels = this.worldOptions?.numOutputBusChannels || 2;
                            const effectiveOutputChannels = Math.min(outputs[0].length, configuredOutputChannels);
                            const requiredBytes = audioBufferPtr + (numSamples * effectiveOutputChannels * 4);

                            if (audioBufferPtr < 0 || audioBufferPtr > bufferSize || requiredBytes > bufferSize) {
                                return true;
                            }

                            // Reuse Float32Array view if possible (avoid allocation in hot path)
                            if (!this.audioView ||
                                this.lastAudioBufferPtr !== audioBufferPtr ||
                                this.lastWasmBufferSize !== bufferSize ||
                                this.lastNumChannels !== effectiveOutputChannels ||
                                currentBuffer !== this.audioView.buffer) {
                                this.audioView = new Float32Array(currentBuffer, audioBufferPtr, numSamples * effectiveOutputChannels);
                                this.lastAudioBufferPtr = audioBufferPtr;
                                this.lastWasmBufferSize = bufferSize;
                            }

                            // Recreate channel views only when parameters change
                            // (avoids per-frame subarray() allocation)
                            if (!this.channelViews ||
                                this.lastNumSamples !== numSamples ||
                                this.lastNumChannels !== effectiveOutputChannels ||
                                this.channelViews[0].buffer !== this.audioView.buffer) {
                                this.channelViews = new Array(effectiveOutputChannels);
                                for (let ch = 0; ch < effectiveOutputChannels; ch++) {
                                    this.channelViews[ch] = this.audioView.subarray(ch * numSamples, (ch + 1) * numSamples);
                                }
                                this.lastNumSamples = numSamples;
                                this.lastNumChannels = effectiveOutputChannels;
                            }

                            // Direct copy using pre-allocated views
                            for (let ch = 0; ch < effectiveOutputChannels; ch++) {
                                outputs[0][ch].set(this.channelViews[ch]);
                            }
                        }
                    } catch (err) {
                        // Silently fail in real-time audio context
                    }
                }

                // PostMessage mode: read OSC replies, debug messages, and node tree updates
                if (this.mode === 'postMessage') {
                    this.readOscReplies();
                    this.readDebugMessages();
                    // Batch log entries with snapshot heartbeat (~150ms) to reduce postMessage frequency
                    if (this.checkAndSendSnapshot(audioContextTime)) {
                        this.sendLogEntries();
                    }
                } else {
                    // SAB mode: Notify waiting workers when there's data to read
                    // Atomics.notify() is cheap when no one is waiting, so notify every frame
                    if (this.atomicView) {
                        const outHead = this.atomicLoad(this.CONTROL_INDICES.OUT_HEAD);
                        const outTail = this.atomicLoad(this.CONTROL_INDICES.OUT_TAIL);
                        if (outHead !== outTail) {
                            Atomics.notify(this.atomicView, this.CONTROL_INDICES.OUT_HEAD, 1);
                        }
                        // Notify debug worker for C++ debug messages (written directly by WASM)
                        const debugHead = this.atomicLoad(this.CONTROL_INDICES.DEBUG_HEAD);
                        const debugTail = this.atomicLoad(this.CONTROL_INDICES.DEBUG_TAIL);
                        if (debugHead !== debugTail) {
                            Atomics.notify(this.atomicView, this.CONTROL_INDICES.DEBUG_HEAD, 1);
                        }
                        // Notify prescheduler if waiting for buffer space
                        Atomics.notify(this.atomicView, this.CONTROL_INDICES.IN_TAIL, 1);
                    }
                    // SAB mode: OSC logging is handled by osc_out_log_sab_worker
                }

                // Periodic status check - reduced frequency
                if (this.processCallCount % 3750 === 0) {  // Every ~10 seconds instead of 1
                    this.checkStatus();
                }

                return keepAlive !== 0;
            }
        } catch (error) {
            console.error('[AudioWorklet] process() error:', error);
            console.error('[AudioWorklet] Stack:', error.stack);
            if (this.atomicView && this.mode === 'sab') {
                Atomics.or(this.atomicView, this.CONTROL_INDICES.STATUS_FLAGS, this.STATUS_FLAGS.WASM_ERROR);
            }
            // Count WASM errors
            if (this.metricsView) {
                if (this.mode === 'sab') {
                    Atomics.add(this.metricsView, MetricsOffsets.SCSYNTH_WASM_ERRORS, 1);
                } else {
                    this.metricsView[MetricsOffsets.SCSYNTH_WASM_ERRORS]++;
                }
            }
        }

        return true;
    }

    checkStatus() {
        if (!this.atomicView) return;

        const statusFlags = this.atomicLoad(this.CONTROL_INDICES.STATUS_FLAGS);

        if (statusFlags !== this.STATUS_FLAGS.OK) {
            // Update pre-allocated status object (avoids allocation on audio thread)
            this._statusObj.bufferFull = !!(statusFlags & this.STATUS_FLAGS.BUFFER_FULL);
            this._statusObj.overrun = !!(statusFlags & this.STATUS_FLAGS.OVERRUN);
            this._statusObj.wasmError = !!(statusFlags & this.STATUS_FLAGS.WASM_ERROR);
            this._statusObj.fragmented = !!(statusFlags & this.STATUS_FLAGS.FRAGMENTED_MSG);

            // Update pre-allocated metrics object (avoids allocation on audio thread)
            this._metricsObj.processCount = this.metricsView[MetricsOffsets.SCSYNTH_PROCESS_COUNT];
            this._metricsObj.messagesProcessed = this.metricsView[MetricsOffsets.SCSYNTH_MESSAGES_PROCESSED];
            this._metricsObj.messagesDropped = this.metricsView[MetricsOffsets.SCSYNTH_MESSAGES_DROPPED];
            this._metricsObj.schedulerQueueDepth = this.metricsView[MetricsOffsets.SCSYNTH_SCHEDULER_DEPTH];
            this._metricsObj.schedulerQueueMax = this.metricsView[MetricsOffsets.SCSYNTH_SCHEDULER_PEAK_DEPTH];
            this._metricsObj.schedulerQueueDropped = this.metricsView[MetricsOffsets.SCSYNTH_SCHEDULER_DROPPED];

            // Update pre-allocated message object and send
            // Note: postMessage does structured clone, so reusing the object is safe
            this._statusMessage.flags = statusFlags;
            this.port.postMessage(this._statusMessage);

            // Clear non-persistent flags
            const persistentFlags = statusFlags & (this.STATUS_FLAGS.BUFFER_FULL);
            this.atomicStore(this.CONTROL_INDICES.STATUS_FLAGS, persistentFlags);
        }
    }
}

// Register the processor
registerProcessor('scsynth-processor', ScsynthProcessor);

/*
    SuperSonic - SuperCollider AudioWorklet WebAssembly port
    Copyright (c) 2025 Sam Aaron

    Based on SuperCollider by James McCartney and community
    GPL v3 or later
*/

/**
 * AudioWorklet Processor for scsynth WASM
 * Runs in AudioWorkletGlobalScope with real-time priority
 * VERSION: 16 - Reduced atomic operation frequency
 */

class ScsynthProcessor extends AudioWorkletProcessor {
    constructor() {
        super();

        this.sharedBuffer = null;
        this.wasmModule = null;
        this.wasmInstance = null;
        this.isInitialized = false;
        this.processCallCount = 0;
        this.lastStatusCheck = 0;
        this.ringBufferBase = null;

        // Pre-allocated audio view to avoid per-frame allocations
        this.audioView = null;
        this.lastAudioBufferPtr = 0;
        this.lastWasmBufferSize = 0;

        // Views into SharedArrayBuffer
        this.atomicView = null;
        this.uint8View = null;
        this.dataView = null;

        // Buffer constants (loaded from WASM at initialization)
        this.bufferConstants = null;

        // Control region indices (Int32Array indices) - will be calculated dynamically
        this.CONTROL_INDICES = null;

        // Metrics indices - will be calculated dynamically
        this.METRICS_INDICES = null;

        // Status flag masks
        this.STATUS_FLAGS = {
            OK: 0,
            BUFFER_FULL: 1 << 0,
            OVERRUN: 1 << 1,
            WASM_ERROR: 1 << 2,
            FRAGMENTED_MSG: 1 << 3
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

        // Read the struct (14 uint32_t fields + 1 uint8_t + 3 padding bytes)
        const uint32View = new Uint32Array(memory.buffer, layoutPtr, 15);
        const uint8View = new Uint8Array(memory.buffer, layoutPtr, 60);

        // Extract constants (order matches BufferLayout struct in shared_memory.h)
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
            TOTAL_BUFFER_SIZE: uint32View[10],
            MAX_MESSAGE_SIZE: uint32View[11],
            MESSAGE_MAGIC: uint32View[12],
            PADDING_MAGIC: uint32View[13],
            DEBUG_PADDING_MARKER: uint8View[56],
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
        this.CONTROL_INDICES = {
            IN_HEAD: (ringBufferBase + CONTROL_START + 0) / 4,
            IN_TAIL: (ringBufferBase + CONTROL_START + 4) / 4,
            OUT_HEAD: (ringBufferBase + CONTROL_START + 8) / 4,
            OUT_TAIL: (ringBufferBase + CONTROL_START + 12) / 4,
            DEBUG_HEAD: (ringBufferBase + CONTROL_START + 16) / 4,
            DEBUG_TAIL: (ringBufferBase + CONTROL_START + 20) / 4,
            SEQUENCE: (ringBufferBase + CONTROL_START + 24) / 4,
            STATUS_FLAGS: (ringBufferBase + CONTROL_START + 28) / 4
        };

        this.METRICS_INDICES = {
            PROCESS_COUNT: (ringBufferBase + METRICS_START + 0) / 4,
            BUFFER_OVERRUNS: (ringBufferBase + METRICS_START + 4) / 4,
            MESSAGES_PROCESSED: (ringBufferBase + METRICS_START + 8) / 4,
            MESSAGES_DROPPED: (ringBufferBase + METRICS_START + 12) / 4
        };
    }

    // Write debug message to SharedArrayBuffer DEBUG ring buffer
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
            const currentHead = Atomics.load(this.atomicView, debugHeadIndex);
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
            Atomics.store(this.atomicView, debugHeadIndex, newHead);
        } catch (err) {
            // Silently fail in real-time audio context
        }
    }

    async handleMessage(event) {
        const { data } = event;

        try {
            if (data.type === 'init' && data.sharedBuffer) {
                // Receive SharedArrayBuffer
                this.sharedBuffer = data.sharedBuffer;
                this.atomicView = new Int32Array(this.sharedBuffer);
                this.uint8View = new Uint8Array(this.sharedBuffer);
                this.dataView = new DataView(this.sharedBuffer);
            }

            if (data.type === 'loadWasm') {
                // Load WASM module (standalone version)
                if (data.wasmBytes) {
                    // Use the memory passed from orchestrator (already created)
                    const memory = data.wasmMemory;
                    if (!memory) {
                        this.port.postMessage({
                            type: 'error',
                            error: 'No WASM memory provided!'
                        });
                        return;
                    }

                    // Save memory reference for later use (WASM imports memory, doesn't export it)
                    this.wasmMemory = memory;

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

                        // Initialize WASM memory
                        if (this.wasmInstance.exports.init_memory) {
                            this.wasmInstance.exports.init_memory(48000.0);

                            // Set time offset from JavaScript
                            if (this.wasmInstance.exports.set_time_offset && data.timeOffset) {
                                this.wasmInstance.exports.set_time_offset(data.timeOffset);
                            }

                            this.isInitialized = true;

                            this.port.postMessage({
                                type: 'initialized',
                                success: true,
                                ringBufferBase: this.ringBufferBase,
                                bufferConstants: this.bufferConstants,
                                exports: Object.keys(this.wasmInstance.exports)
                            });
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

                        // Initialize WASM memory
                        if (this.wasmInstance.exports.init_memory) {
                            this.wasmInstance.exports.init_memory(48000.0);

                            // Set time offset from JavaScript
                            if (this.wasmInstance.exports.set_time_offset && data.timeOffset) {
                                this.wasmInstance.exports.set_time_offset(data.timeOffset);
                            }

                            this.isInitialized = true;

                            this.port.postMessage({
                                type: 'initialized',
                                success: true,
                                ringBufferBase: this.ringBufferBase,
                                bufferConstants: this.bufferConstants,
                                exports: Object.keys(this.wasmInstance.exports)
                            });
                        }
                    }
                }
            }

            if (data.type === 'getMetrics') {
                // Return current metrics (only if initialized)
                if (this.atomicView && this.METRICS_INDICES && this.CONTROL_INDICES && this.bufferConstants) {
                    // Calculate buffer usage percentages (use constants from WASM)
                    const IN_BUFFER_SIZE = this.bufferConstants.IN_BUFFER_SIZE;
                    const OUT_BUFFER_SIZE = this.bufferConstants.OUT_BUFFER_SIZE;
                    const DEBUG_BUFFER_SIZE = this.bufferConstants.DEBUG_BUFFER_SIZE;

                    const inHead = Atomics.load(this.atomicView, this.CONTROL_INDICES.IN_HEAD);
                    const inTail = Atomics.load(this.atomicView, this.CONTROL_INDICES.IN_TAIL);
                    const inUsed = (inHead - inTail + IN_BUFFER_SIZE) % IN_BUFFER_SIZE;
                    const inPercentage = Math.round((inUsed / IN_BUFFER_SIZE) * 100);

                    const outHead = Atomics.load(this.atomicView, this.CONTROL_INDICES.OUT_HEAD);
                    const outTail = Atomics.load(this.atomicView, this.CONTROL_INDICES.OUT_TAIL);
                    const outUsed = (outHead - outTail + OUT_BUFFER_SIZE) % OUT_BUFFER_SIZE;
                    const outPercentage = Math.round((outUsed / OUT_BUFFER_SIZE) * 100);

                    const debugHead = Atomics.load(this.atomicView, this.CONTROL_INDICES.DEBUG_HEAD);
                    const debugTail = Atomics.load(this.atomicView, this.CONTROL_INDICES.DEBUG_TAIL);
                    const debugUsed = (debugHead - debugTail + DEBUG_BUFFER_SIZE) % DEBUG_BUFFER_SIZE;
                    const debugPercentage = Math.round((debugUsed / DEBUG_BUFFER_SIZE) * 100);

                    const metrics = {
                        processCount: Atomics.load(this.atomicView, this.METRICS_INDICES.PROCESS_COUNT),
                        bufferOverruns: Atomics.load(this.atomicView, this.METRICS_INDICES.BUFFER_OVERRUNS),
                        messagesProcessed: Atomics.load(this.atomicView, this.METRICS_INDICES.MESSAGES_PROCESSED),
                        messagesDropped: Atomics.load(this.atomicView, this.METRICS_INDICES.MESSAGES_DROPPED),
                        statusFlags: Atomics.load(this.atomicView, this.CONTROL_INDICES.STATUS_FLAGS),
                        inBufferUsed: {
                            bytes: inUsed,
                            percentage: inPercentage
                        },
                        outBufferUsed: {
                            bytes: outUsed,
                            percentage: outPercentage
                        },
                        debugBufferUsed: {
                            bytes: debugUsed,
                            percentage: debugPercentage
                        }
                    };

                    this.port.postMessage({
                        type: 'metrics',
                        metrics: metrics
                    });
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
        if (!this.isInitialized) {
            return true;
        }

        try {
            if (this.wasmInstance && this.wasmInstance.exports.process_audio) {
                // CRITICAL: Access AudioContext currentTime correctly
                // In AudioWorkletGlobalScope, currentTime is a bare global variable (not on globalThis)
                // We use a different variable name to avoid shadowing
                const audioContextTime = currentTime;  // Access the global currentTime directly
                const unixSeconds = Date.now() / 1000;  // Current Unix time in seconds
                const keepAlive = this.wasmInstance.exports.process_audio(audioContextTime, unixSeconds);

                // Copy scsynth audio output to AudioWorklet outputs
                if (this.wasmInstance.exports.get_audio_output_bus && outputs[0] && outputs[0].length >= 2) {
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
                            const requiredBytes = audioBufferPtr + (numSamples * 2 * 4);

                            if (audioBufferPtr < 0 || audioBufferPtr > bufferSize || requiredBytes > bufferSize) {
                                return true;
                            }

                            // Reuse Float32Array view if possible (avoid allocation in hot path)
                            if (!this.audioView ||
                                this.lastAudioBufferPtr !== audioBufferPtr ||
                                this.lastWasmBufferSize !== bufferSize ||
                                currentBuffer !== this.audioView.buffer) {
                                this.audioView = new Float32Array(currentBuffer, audioBufferPtr, numSamples * 2);
                                this.lastAudioBufferPtr = audioBufferPtr;
                                this.lastWasmBufferSize = bufferSize;
                            }

                            // Direct copy using pre-allocated view
                            outputs[0][0].set(this.audioView.subarray(0, numSamples));
                            outputs[0][1].set(this.audioView.subarray(numSamples, numSamples * 2));
                        }
                    } catch (err) {
                        // Silently fail in real-time audio context
                    }
                }

                // Notify waiting worker only occasionally to reduce overhead
                // Most of the time the worker isn't waiting anyway
                if (this.atomicView && (this.processCallCount % 16 === 0)) {
                    Atomics.notify(this.atomicView, this.CONTROL_INDICES.OUT_HEAD, 1);
                }

                // Periodic status check - reduced frequency
                this.processCallCount++;
                if (this.processCallCount % 3750 === 0) {  // Every ~10 seconds instead of 1
                    this.checkStatus();
                }

                return keepAlive !== 0;
            }
        } catch (error) {
            if (this.atomicView) {
                Atomics.or(this.atomicView, this.CONTROL_INDICES.STATUS_FLAGS, this.STATUS_FLAGS.WASM_ERROR);
            }
        }

        return true;
    }

    checkStatus() {
        if (!this.atomicView) return;

        const statusFlags = Atomics.load(this.atomicView, this.CONTROL_INDICES.STATUS_FLAGS);

        if (statusFlags !== this.STATUS_FLAGS.OK) {
            const status = {
                bufferFull: !!(statusFlags & this.STATUS_FLAGS.BUFFER_FULL),
                overrun: !!(statusFlags & this.STATUS_FLAGS.OVERRUN),
                wasmError: !!(statusFlags & this.STATUS_FLAGS.WASM_ERROR),
                fragmented: !!(statusFlags & this.STATUS_FLAGS.FRAGMENTED_MSG)
            };

            // Get current metrics
            const metrics = {
                processCount: Atomics.load(this.atomicView, this.METRICS_INDICES.PROCESS_COUNT),
                messagesProcessed: Atomics.load(this.atomicView, this.METRICS_INDICES.MESSAGES_PROCESSED),
                messagesDropped: Atomics.load(this.atomicView, this.METRICS_INDICES.MESSAGES_DROPPED),
                bufferOverruns: Atomics.load(this.atomicView, this.METRICS_INDICES.BUFFER_OVERRUNS)
            };

            this.port.postMessage({
                type: 'status',
                flags: statusFlags,
                status: status,
                metrics: metrics
            });

            // Clear non-persistent flags
            const persistentFlags = statusFlags & (this.STATUS_FLAGS.BUFFER_FULL);
            Atomics.store(this.atomicView, this.CONTROL_INDICES.STATUS_FLAGS, persistentFlags);
        }
    }
}

// Register the processor
registerProcessor('scsynth-processor', ScsynthProcessor);
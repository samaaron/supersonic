/*
    SuperSonic - SuperCollider AudioWorklet WebAssembly port
    Copyright (c) 2025 Sam Aaron

    Based on SuperCollider by James McCartney and community
    GPL v3 or later
*/

/**
 * SuperSonic - WebAssembly SuperCollider synthesis engine
 * Coordinates SharedArrayBuffer, WASM, AudioWorklet, and IO Workers
 */

import ScsynthOSC from './lib/scsynth_osc.js';
import { BufferManager } from './lib/buffer_manager.js';
import oscLib from './vendor/osc.js/osc.js';
import { NTP_EPOCH_OFFSET, DRIFT_UPDATE_INTERVAL_MS } from './timing_constants.js';
import { MemoryLayout } from './memory_layout.js';
import { defaultWorldOptions } from './scsynth_options.js';

/**
 * SuperSonic metrics object - all metrics read synchronously from SharedArrayBuffer
 * @typedef {Object} SuperSonicMetrics
 *
 * Core counters (SuperSonic in-memory):
 * @property {number} messagesSent - OSC messages sent to scsynth
 * @property {number} messagesReceived - OSC messages received from scsynth
 * @property {number} errors - Total errors encountered
 *
 * Worklet metrics (WASM writes to SAB):
 * @property {number} processCount - Audio process() calls
 * @property {number} bufferOverruns - Buffer overrun events
 * @property {number} messagesProcessed - Messages processed by scsynth
 * @property {number} messagesDropped - Messages dropped by scsynth
 * @property {number} schedulerQueueDepth - Current scheduler queue depth
 * @property {number} schedulerQueueMax - Maximum scheduler queue depth reached
 * @property {number} schedulerQueueDropped - Messages dropped from scheduler queue
 *
 * Buffer usage (calculated from SAB head/tail pointers):
 * @property {Object} inBufferUsed - Input buffer usage statistics
 * @property {number} inBufferUsed.bytes - Bytes used in input buffer
 * @property {number} inBufferUsed.percentage - Percentage of input buffer used
 * @property {Object} outBufferUsed - Output buffer usage statistics
 * @property {number} outBufferUsed.bytes - Bytes used in output buffer
 * @property {number} outBufferUsed.percentage - Percentage of output buffer used
 * @property {Object} debugBufferUsed - Debug buffer usage statistics
 * @property {number} debugBufferUsed.bytes - Bytes used in debug buffer
 * @property {number} debugBufferUsed.percentage - Percentage of debug buffer used
 *
 * OSC worker metrics (workers write to SAB):
 *
 * OSC Out (Prescheduler):
 * @property {number} preschedulerPending - Current pending events in queue
 * @property {number} preschedulerPeak - Peak pending events (high water mark)
 * @property {number} preschedulerSent - Total bundles written to ring buffer
 * @property {number} bundlesDropped - Bundles dropped (ring buffer full)
 * @property {number} retriesSucceeded - Successful retry attempts
 * @property {number} retriesFailed - Failed retry attempts (gave up)
 * @property {number} bundlesScheduled - Total bundles scheduled
 * @property {number} eventsCancelled - Total events cancelled
 * @property {number} totalDispatches - Total dispatch cycles executed
 * @property {number} messagesRetried - Total retry attempts (all)
 * @property {number} retryQueueSize - Current retry queue size
 * @property {number} retryQueueMax - Peak retry queue size
 *
 * OSC In:
 * @property {number} oscInMessagesReceived - OSC In messages received
 * @property {number} oscInDroppedMessages - OSC In dropped messages
 * @property {number} oscInWakeups - OSC In worker wakeups
 * @property {number} oscInTimeouts - OSC In worker timeouts
 *
 * Debug:
 * @property {number} debugMessagesReceived - Debug messages received
 * @property {number} debugWakeups - Debug worker wakeups
 * @property {number} debugTimeouts - Debug worker timeouts
 * @property {number} debugBytesRead - Debug bytes read
 */

export class SuperSonic {
    // Expose OSC utilities as static methods
    static osc = {
        encode: (message) => oscLib.writePacket(message),
        decode: (data, options = { metadata: false }) => oscLib.readPacket(data, options)
    };

    // Private implementation
    #audioContext;
    #workletNode;
    #osc;
    #wasmMemory;
    #sharedBuffer;
    #ringBufferBase;
    #bufferConstants;
    #bufferManager;
    #driftOffsetTimer;
    #syncListeners;
    #initialNTPStartTime;
    #sampleBaseURL;
    #synthdefBaseURL;
    #audioPathMap;
    #initialized;
    #initializing;
    #capabilities;

    // Runtime metrics (private counters)
    #metrics_messagesSent = 0;
    #metrics_messagesReceived = 0;
    #metrics_errors = 0;
    #metricsIntervalId = null;
    #metricsGatherInProgress = false;

    constructor(options = {}) {
        this.#initialized = false;
        this.#initializing = false;
        this.#capabilities = {};

        // Core components (private)
        this.#sharedBuffer = null;
        this.#ringBufferBase = null;
        this.#bufferConstants = null;
        this.#audioContext = null;
        this.#workletNode = null;
        this.#osc = null;  // ScsynthOSC instance for OSC communication
        this.#bufferManager = null;
        this.loadedSynthDefs = new Set();

        // Callbacks
        this.onOSC = null;              // Raw binary OSC from scsynth (for display/logging)
        this.onMessage = null;          // Parsed OSC messages from scsynth (for application logic)
        this.onMessageSent = null;
        this.onMetricsUpdate = null;
        this.onDebugMessage = null;
        this.onInitialized = null;
        this.onError = null;

        // Configuration - require explicit base URLs for workers and WASM
        // This ensures SuperSonic works correctly in bundled/vendored environments
        if (!options.workerBaseURL || !options.wasmBaseURL) {
            throw new Error('SuperSonic requires workerBaseURL and wasmBaseURL options. Example:\n' +
                'new SuperSonic({\n' +
                '  workerBaseURL: "/supersonic/workers/",\n' +
                '  wasmBaseURL: "/supersonic/wasm/"\n' +
                '})');
        }

        const workerBaseURL = options.workerBaseURL;
        const wasmBaseURL = options.wasmBaseURL;

        const worldOptions = { ...defaultWorldOptions, ...options.scsynthOptions };

        this.config = {
            wasmUrl: options.wasmUrl || wasmBaseURL + 'scsynth-nrt.wasm',
            wasmBaseURL: wasmBaseURL,
            workletUrl: options.workletUrl || workerBaseURL + 'scsynth_audio_worklet.js',
            workerBaseURL: workerBaseURL,
            development: false,
            audioContextOptions: {
                latencyHint: 'interactive', // hint to push for lowest latency possible
                sampleRate: 48000 // only requested rate - actual rate is determined by hardware
            },
            // Build-time memory layout (constant)
            memory: MemoryLayout,
            // Runtime world options (merged defaults + user overrides)
            worldOptions: worldOptions
        };

        // Resource loading configuration (private)
        this.#sampleBaseURL = options.sampleBaseURL || null;
        this.#synthdefBaseURL = options.synthdefBaseURL || null;
        this.#audioPathMap = options.audioPathMap || {};

        // Boot statistics (one-time metrics)
        this.bootStats = {
            initStartTime: null,
            initDuration: null
        };
    }

    /**
     * Get initialization status (read-only)
     */
    get initialized() {
        return this.#initialized;
    }

    /**
     * Get initialization in-progress status (read-only)
     */
    get initializing() {
        return this.#initializing;
    }

    /**
     * Get browser capabilities (read-only)
     */
    get capabilities() {
        return this.#capabilities;
    }

    /**
     * Set and validate browser capabilities for required features
     */
    setAndValidateCapabilities() {
        this.#capabilities = {
            audioWorklet: typeof AudioWorklet !== 'undefined',
            sharedArrayBuffer: typeof SharedArrayBuffer !== 'undefined',
            crossOriginIsolated: window.crossOriginIsolated === true,
            atomics: typeof Atomics !== 'undefined',
            webWorker: typeof Worker !== 'undefined'
        };

        // Check for required features
        const required = ['audioWorklet', 'sharedArrayBuffer', 'crossOriginIsolated',
                         'atomics', 'webWorker'];
        const missing = required.filter(f => !this.#capabilities[f]);

        if (missing.length > 0) {
            const error = new Error(`Missing required features: ${missing.join(', ')}`);

            // Special case for cross-origin isolation
            if (!this.#capabilities.crossOriginIsolated) {
                if (this.#capabilities.sharedArrayBuffer) {
                    error.message += '\n\nSharedArrayBuffer is available but cross-origin isolation is not enabled. ' +
                                   'Please ensure COOP and COEP headers are set correctly:\n' +
                                   '  Cross-Origin-Opener-Policy: same-origin\n' +
                                   '  Cross-Origin-Embedder-Policy: require-corp';
                } else {
                    error.message += '\n\nSharedArrayBuffer is not available. This may be due to:\n' +
                                   '1. Missing COOP/COEP headers\n' +
                                   '2. Browser doesn\'t support SharedArrayBuffer\n' +
                                   '3. Browser security settings';
                }
            }

            throw error;
        }

        return this.#capabilities;
    }

    /**
     * Merge user-provided world options with defaults
     * @private
     */

    /**
     * Initialize shared WebAssembly memory
     */
    #initializeSharedMemory() {
        // Memory layout (from memory_layout.js):
        // 0-16MB:   WASM heap (scsynth C++ allocations)
        // 16-17MB:  Ring buffers (~1MB):
        //           - OSC IN: 768KB, OSC OUT: 128KB, DEBUG: 64KB
        //           - Control structures, metrics, NTP timing: ~96B
        // 17-80MB:  Buffer pool (audio sample storage, 63MB)
        // Total: 80MB
        const memConfig = this.config.memory;

        this.#wasmMemory = new WebAssembly.Memory({
            initial: memConfig.totalPages,
            maximum: memConfig.totalPages,
            shared: true
        });
        this.#sharedBuffer = this.#wasmMemory.buffer;
    }


    #initializeAudioContext() {
        this.#audioContext = new AudioContext(this.config.audioContextOptions);
        return this.#audioContext;
    }

    #initializeBufferManager() {
        this.#bufferManager = new BufferManager({
            audioContext: this.#audioContext,
            sharedBuffer: this.#sharedBuffer,
            bufferPoolConfig: {
                start: this.config.memory.bufferPoolOffset,
                size: this.config.memory.bufferPoolSize
            },
            sampleBaseURL: this.#sampleBaseURL,
            audioPathMap: this.#audioPathMap,
            maxBuffers: this.config.worldOptions.numBuffers
        });
    }

    async #loadWasmManifest() {
        const manifestUrl = this.config.wasmBaseURL + 'manifest.json';

        try {
            const response = await fetch(manifestUrl);
            if (!response.ok) {
                return;
            }

            const manifest = await response.json();
            this.config.wasmUrl = this.config.wasmBaseURL + manifest.wasmFile;
            if (__DEV__) console.log(`[SuperSonic] WASM: ${manifest.wasmFile} (${manifest.buildId}, git: ${manifest.gitHash})`);
        } catch (error) {
            // Manifest failed to load - use default filename
        }
    }

    /**
     * Load WASM binary from network
     */
    async #loadWasm() {
        // In development mode, load manifest for cache-busted filename
        if (this.config.development) {
            await this.#loadWasmManifest();
        }

        const wasmResponse = await fetch(this.config.wasmUrl);
        if (!wasmResponse.ok) {
            throw new Error(`Failed to load WASM: ${wasmResponse.status} ${wasmResponse.statusText}`);
        }
        return await wasmResponse.arrayBuffer();
    }

    /**
     * Initialize AudioWorklet with WASM
     */
    async #initializeAudioWorklet(wasmBytes) {
        // Load AudioWorklet processor
        await this.#audioContext.audioWorklet.addModule(this.config.workletUrl);

        // Create AudioWorkletNode
        // Configure with numberOfInputs: 0 to act as a source node
        // This ensures process() is called continuously without needing an input source
        this.#workletNode = new AudioWorkletNode(this.#audioContext, 'scsynth-processor', {
            numberOfInputs: 0,
            numberOfOutputs: 1,
            outputChannelCount: [2]
        });

        // Connect to audio graph to trigger process() calls
        this.#workletNode.connect(this.#audioContext.destination);

        // Initialize AudioWorklet with SharedArrayBuffer
        this.#workletNode.port.postMessage({
            type: 'init',
            sharedBuffer: this.#sharedBuffer
        });

        // Send WASM bytes, memory, worldOptions, and actual sample rate
        this.#workletNode.port.postMessage({
            type: 'loadWasm',
            wasmBytes: wasmBytes,
            wasmMemory: this.#wasmMemory,
            worldOptions: this.config.worldOptions,
            sampleRate: this.#audioContext.sampleRate  // Pass actual AudioContext sample rate
        });

        // Wait for worklet initialization
        await this.#waitForWorkletInit();
    }

    /**
     * Initialize OSC communication layer
     */
    async #initializeOSC() {
        // Create ScsynthOSC instance with custom worker base URL if provided
        this.#osc = new ScsynthOSC(this.config.workerBaseURL);

        // Set up ScsynthOSC callbacks
        this.#osc.onRawOSC((msg) => {
            // Forward raw binary OSC to onOSC callback (for display/logging)
            if (this.onOSC) {
                this.onOSC(msg);
            }
        });

        this.#osc.onParsedOSC((msg) => {
            // Handle internal messages
            if (msg.address === '/buffer/freed') {
                this.#bufferManager?.handleBufferFreed(msg.args);
            } else if (msg.address === '/buffer/allocated') {
                // Handle buffer allocation completion with UUID correlation
                this.#bufferManager?.handleBufferAllocated(msg.args);
            } else if (msg.address === '/synced' && msg.args.length > 0) {
                // Handle /synced responses for sync operations
                const syncId = msg.args[0];  // Integer sync ID
                if (this.#syncListeners && this.#syncListeners.has(syncId)) {
                    const listener = this.#syncListeners.get(syncId);
                    listener(msg);
                }
            }

            // Always forward to onMessage (including internal messages)
            if (this.onMessage) {
                this.#metrics_messagesReceived++;
                this.onMessage(msg);
            }
        });

        this.#osc.onDebugMessage((msg) => {
            if (this.onDebugMessage) {
                this.onDebugMessage(msg);
            }
        });

        this.#osc.onError((error, workerName) => {
            console.error(`[SuperSonic] ${workerName} error:`, error);
            this.#metrics_errors++;
            if (this.onError) {
                this.onError(new Error(`${workerName}: ${error}`));
            }
        });

        // Initialize ScsynthOSC with SharedArrayBuffer, ring buffer base, and buffer constants
        await this.#osc.init(this.#sharedBuffer, this.#ringBufferBase, this.#bufferConstants);
    }

    /**
     * Complete initialization and trigger callbacks
     */
    #finishInitialization() {
        this.#initialized = true;
        this.#initializing = false;
        this.bootStats.initDuration = performance.now() - this.bootStats.initStartTime;

        if (__DEV__) console.log(`[SuperSonic] Initialization complete in ${this.bootStats.initDuration.toFixed(2)}ms`);

        if (this.onInitialized) {
            this.onInitialized({
                capabilities: this.#capabilities,
                bootStats: this.bootStats
            });
        }
    }

    /**
     * Initialize the audio worklet system
     * @param {Object} config - Optional configuration overrides
     * @param {boolean} config.development - Use cache-busted WASM files (default: false)
     * @param {string} config.wasmUrl - Custom WASM URL
     * @param {string} config.workletUrl - Custom worklet URL
     * @param {Object} config.audioContextOptions - AudioContext options
     */
    async init(config = {}) {
        if (this.#initialized) {
            console.warn('[SuperSonic] Already initialized');
            return;
        }

        if (this.#initializing) {
            console.warn('[SuperSonic] Initialization already in progress');
            return;
        }

        // Merge config with defaults
        this.config = {
            ...this.config,
            ...config,
            audioContextOptions: {
                ...this.config.audioContextOptions,
                ...(config.audioContextOptions || {})
            }
        };

        this.#initializing = true;
        this.bootStats.initStartTime = performance.now();

        try {
            this.setAndValidateCapabilities();
            this.#initializeSharedMemory();
            this.#initializeAudioContext();
            this.#initializeBufferManager();
            const wasmBytes = await this.#loadWasm();
            await this.#initializeAudioWorklet(wasmBytes);
            await this.#initializeOSC();
            this.#setupMessageHandlers();
            this.#startPerformanceMonitoring();
            this.#finishInitialization();
        } catch (error) {
            this.#initializing = false;
            console.error('[SuperSonic] Initialization failed:', error);

            if (this.onError) {
                this.onError(error);
            }

            throw error;
        }
    }

    /**
     * Wait for AudioWorklet to initialize
     */
    #waitForWorkletInit() {
        return new Promise((resolve, reject) => {
            const timeout = setTimeout(() => {
                reject(new Error('AudioWorklet initialization timeout'));
            }, 5000);

            const messageHandler = async (event) => {
                // Handle debug messages during initialization
                if (event.data.type === 'debug') {
                    // Silent during init
                    return;
                }

                // Handle error messages during initialization
                if (event.data.type === 'error') {
                    console.error('[AudioWorklet] Error:', event.data.error);
                    clearTimeout(timeout);
                    this.#workletNode.port.removeEventListener('message', messageHandler);
                    reject(new Error(event.data.error || 'AudioWorklet error'));
                    return;
                }

                if (event.data.type === 'initialized') {
                    clearTimeout(timeout);
                    this.#workletNode.port.removeEventListener('message', messageHandler);

                    if (event.data.success) {
                        // Store the ring buffer base address and constants from WASM
                        if (event.data.ringBufferBase !== undefined) {
                            this.#ringBufferBase = event.data.ringBufferBase;
                        } else {
                            console.warn('[SuperSonic] Warning: ringBufferBase not provided by worklet');
                        }

                        if (event.data.bufferConstants !== undefined) {
                            if (__DEV__) console.log('[SuperSonic] Received bufferConstants from worklet');
                            this.#bufferConstants = event.data.bufferConstants;

                            // Initialize NTP timing (blocks until audio is flowing)
                            if (__DEV__) console.log('[SuperSonic] Initializing NTP timing (waiting for audio to flow)...');
                            await this.initializeNTPTiming();

                            // Start periodic drift offset updates (small millisecond adjustments)
                            // Measures drift from initial baseline, replaces value (doesn't accumulate)
                            this.#startDriftOffsetTimer();
                        } else {
                            console.warn('[SuperSonic] Warning: bufferConstants not provided by worklet');
                        }

                        if (__DEV__) console.log('[SuperSonic] Calling resolve() for worklet initialization');
                        resolve();
                    } else {
                        reject(new Error(event.data.error || 'AudioWorklet initialization failed'));
                    }
                }
            };

            this.#workletNode.port.addEventListener('message', messageHandler);
            this.#workletNode.port.start();
        });
    }


    /**
     * Set up message handlers for worklet
     */
    #setupMessageHandlers() {
        // ScsynthOSC handles all worker messages internally
        // We only need to handle worklet messages here

        /**
         * Worklet message handler
         *
         * Note: Worklet metrics (processCount, bufferOverruns, etc.) are read directly from
         * SharedArrayBuffer in #getWorkletMetrics(). The worklet no longer sends 'metrics'
         * messages, and 'getMetrics' requests are not used (SAB reads are synchronous).
         */
        this.#workletNode.port.onmessage = (event) => {
            const { data } = event;

            switch (data.type) {
                case 'error':
                    console.error('[Worklet] Error:', data.error);
                    if (data.diagnostics) {
                        console.error('[Worklet] Diagnostics:', data.diagnostics);
                        console.table(data.diagnostics);
                    }
                    this.#metrics_errors++;
                    if (this.onError) {
                        this.onError(new Error(data.error));
                    }
                    break;

                case 'process_debug':
                    // Debug messages - commented out to reduce console noise
                    // console.log('[Worklet] process() called:', data.count, 'initialized:', data.initialized);
                    break;

                case 'debug':
                    // Debug messages from AudioWorklet - silent in production
                    break;

                case 'console':
                    // Console messages from AudioWorklet - forward to callback
                    if (this.onConsoleMessage) {
                        this.onConsoleMessage(data.message);
                    }
                    break;

                case 'version':
                    // Version from worklet (Supersonic + SuperCollider)
                    if (this.onVersion) {
                        this.onVersion(data.version);
                    }
                    break;
            }
        };
    }

    /**
     * Get metrics from SharedArrayBuffer (worklet metrics written by WASM)
     * @returns {Object|null}
     * @private
     */
    #getWorkletMetrics() {
        if (!this.#sharedBuffer || !this.#bufferConstants || !this.#ringBufferBase) {
            return null;
        }

        // Metrics are already in SAB - just read them directly!
        const metricsBase = this.#ringBufferBase + this.#bufferConstants.METRICS_START;
        const metricsCount = this.#bufferConstants.METRICS_SIZE / 4;  // METRICS_SIZE is in bytes, Uint32Array needs element count
        const metricsView = new Uint32Array(this.#sharedBuffer, metricsBase, metricsCount);

        // Read metrics from SAB (layout defined in src/shared_memory.h METRICS_* section)
        return {
            processCount: Atomics.load(metricsView, 0),              // PROCESS_COUNT offset / 4
            bufferOverruns: Atomics.load(metricsView, 1),            // BUFFER_OVERRUNS offset / 4
            messagesProcessed: Atomics.load(metricsView, 2),         // MESSAGES_PROCESSED offset / 4
            messagesDropped: Atomics.load(metricsView, 3),           // MESSAGES_DROPPED offset / 4
            schedulerQueueDepth: Atomics.load(metricsView, 4),       // SCHEDULER_QUEUE_DEPTH offset / 4
            schedulerQueueMax: Atomics.load(metricsView, 5),         // SCHEDULER_QUEUE_MAX offset / 4
            schedulerQueueDropped: Atomics.load(metricsView, 6)      // SCHEDULER_QUEUE_DROPPED offset / 4
        };
    }

    /**
     * Get buffer usage statistics from SAB head/tail pointers
     * @returns {Object|null}
     * @private
     */
    #getBufferUsage() {
        if (!this.#sharedBuffer || !this.#bufferConstants || !this.#ringBufferBase) {
            return null;
        }

        const atomicView = new Int32Array(this.#sharedBuffer);
        const controlBase = this.#ringBufferBase + this.#bufferConstants.CONTROL_START;

        // Read head/tail pointers
        const inHead = Atomics.load(atomicView, (controlBase + 0) / 4);
        const inTail = Atomics.load(atomicView, (controlBase + 4) / 4);
        const outHead = Atomics.load(atomicView, (controlBase + 8) / 4);
        const outTail = Atomics.load(atomicView, (controlBase + 12) / 4);
        const debugHead = Atomics.load(atomicView, (controlBase + 16) / 4);
        const debugTail = Atomics.load(atomicView, (controlBase + 20) / 4);

        // Calculate bytes used (accounting for wrap-around)
        const inUsed = (inHead - inTail + this.#bufferConstants.IN_BUFFER_SIZE) % this.#bufferConstants.IN_BUFFER_SIZE;
        const outUsed = (outHead - outTail + this.#bufferConstants.OUT_BUFFER_SIZE) % this.#bufferConstants.OUT_BUFFER_SIZE;
        const debugUsed = (debugHead - debugTail + this.#bufferConstants.DEBUG_BUFFER_SIZE) % this.#bufferConstants.DEBUG_BUFFER_SIZE;

        return {
            inBufferUsed: {
                bytes: inUsed,
                percentage: Math.round((inUsed / this.#bufferConstants.IN_BUFFER_SIZE) * 100)
            },
            outBufferUsed: {
                bytes: outUsed,
                percentage: Math.round((outUsed / this.#bufferConstants.OUT_BUFFER_SIZE) * 100)
            },
            debugBufferUsed: {
                bytes: debugUsed,
                percentage: Math.round((debugUsed / this.#bufferConstants.DEBUG_BUFFER_SIZE) * 100)
            }
        };
    }

    /**
     * Get OSC worker metrics from SharedArrayBuffer (written by OSC workers)
     * @returns {Object|null}
     * @private
     */
    #getOSCMetrics() {
        if (!this.#sharedBuffer || !this.#bufferConstants || !this.#ringBufferBase) {
            return null;
        }

        const metricsBase = this.#ringBufferBase + this.#bufferConstants.METRICS_START;
        const metricsCount = this.#bufferConstants.METRICS_SIZE / 4;
        const metricsView = new Uint32Array(this.#sharedBuffer, metricsBase, metricsCount);

        // Read OSC worker metrics from SAB
        return {
            // OSC Out (prescheduler) - offsets 7-18
            preschedulerPending: metricsView[7],
            preschedulerPeak: metricsView[8],
            preschedulerSent: metricsView[9],
            bundlesDropped: metricsView[10],
            retriesSucceeded: metricsView[11],
            retriesFailed: metricsView[12],
            bundlesScheduled: metricsView[13],
            eventsCancelled: metricsView[14],
            totalDispatches: metricsView[15],
            messagesRetried: metricsView[16],
            retryQueueSize: metricsView[17],
            retryQueueMax: metricsView[18],

            // OSC In - offsets 19-22
            oscInMessagesReceived: metricsView[19],
            oscInDroppedMessages: metricsView[20],
            oscInWakeups: metricsView[21],
            oscInTimeouts: metricsView[22],

            // Debug - offsets 23-26
            debugMessagesReceived: metricsView[23],
            debugWakeups: metricsView[24],
            debugTimeouts: metricsView[25],
            debugBytesRead: metricsView[26]
        };
    }

    /**
     * Gather metrics from all sources (worklet, OSC, internal counters)
     * All metrics are read synchronously from SAB
     * @returns {SuperSonicMetrics}
     * @private
     */
    #gatherMetrics() {
        const startTime = performance.now();

        const metrics = {
            // SuperSonic counters (in-memory, fast)
            messagesSent: this.#metrics_messagesSent,
            messagesReceived: this.#metrics_messagesReceived,
            errors: this.#metrics_errors
        };

        // Worklet metrics (instant SAB read)
        const workletMetrics = this.#getWorkletMetrics();
        if (workletMetrics) {
            Object.assign(metrics, workletMetrics);
        }

        // Buffer usage (calculated from SAB head/tail pointers)
        const bufferUsage = this.#getBufferUsage();
        if (bufferUsage) {
            Object.assign(metrics, bufferUsage);
        }

        // OSC worker metrics (instant SAB read)
        const oscMetrics = this.#getOSCMetrics();
        if (oscMetrics) {
            Object.assign(metrics, oscMetrics);
        }

        const totalDuration = performance.now() - startTime;
        if (totalDuration > 1) {
            console.warn(`[SuperSonic] Slow metrics gathering: ${totalDuration.toFixed(2)}ms`);
        }

        return metrics;
    }

    /**
     * Start performance monitoring - gathers metrics from all sources
     * and calls onMetricsUpdate with consolidated snapshot
     */
    #startPerformanceMonitoring() {
        // Clear any existing interval (shouldn't happen, but safety first)
        if (this.#metricsIntervalId) {
            clearInterval(this.#metricsIntervalId);
        }

        // Request metrics periodically (100ms = 10Hz)
        // All metrics are read from SAB (<0.1ms) - fully synchronous
        this.#metricsIntervalId = setInterval(() => {
            if (!this.onMetricsUpdate) return;

            // Prevent overlapping executions if gathering takes >100ms
            if (this.#metricsGatherInProgress) {
                console.warn('[SuperSonic] Metrics gathering took >100ms, skipping this interval');
                return;
            }

            this.#metricsGatherInProgress = true;
            try {
                // Gather all metrics from all sources (synchronous SAB reads)
                const metrics = this.#gatherMetrics();

                // Single callback with complete metrics snapshot
                this.onMetricsUpdate(metrics);
            } catch (error) {
                console.error('[SuperSonic] Metrics gathering failed:', error);
            } finally {
                this.#metricsGatherInProgress = false;
            }
        }, 100);
    }

    /**
     * Stop performance monitoring
     * @private
     */
    #stopPerformanceMonitoring() {
        if (this.#metricsIntervalId) {
            clearInterval(this.#metricsIntervalId);
            this.#metricsIntervalId = null;
        }
    }

    /**
     * Send OSC message with simplified syntax (auto-detects types)
     * @param {string} address - OSC address
     * @param {...*} args - Arguments (numbers, strings, Uint8Array)
     * @example
     * sonic.send('/notify', 1);
     * sonic.send('/s_new', 'sonic-pi-beep', -1, 0, 0);
     * sonic.send('/n_set', 1000, 'freq', 440.0, 'amp', 0.5);
     */
    async send(address, ...args) {
        this.#ensureInitialized('send OSC messages');

        const oscArgs = args.map((arg) => {
            if (typeof arg === 'string') {
                return { type: 's', value: arg };
            } else if (typeof arg === 'number') {
                return { type: Number.isInteger(arg) ? 'i' : 'f', value: arg };
            } else if (arg instanceof Uint8Array || arg instanceof ArrayBuffer) {
                return { type: 'b', value: arg instanceof ArrayBuffer ? new Uint8Array(arg) : arg };
            } else {
                throw new Error(`Unsupported argument type: ${typeof arg}`);
            }
        });

        const message = { address, args: oscArgs };
        const oscData = SuperSonic.osc.encode(message);
        return this.sendOSC(oscData);
    }

    #ensureInitialized(actionDescription = 'perform this operation') {
        if (!this.#initialized) {
            throw new Error(`SuperSonic not initialized. Call init() before attempting to ${actionDescription}.`);
        }
    }

    /**
     * Send pre-encoded OSC bytes to scsynth
     * @param {ArrayBuffer|Uint8Array} oscData - Pre-encoded OSC data
     * @param {Object} options - Send options
     */
    async sendOSC(oscData, options = {}) {
        this.#ensureInitialized('send OSC data');

        const uint8Data = this.#toUint8Array(oscData);
        const preparedData = await this.#prepareOutboundPacket(uint8Data);

        this.#metrics_messagesSent++;

        if (this.onMessageSent) {
            this.onMessageSent(preparedData);
        }

        const timing = this.#calculateBundleWait(preparedData);
        const sendOptions = { ...options };

        if (timing) {
            sendOptions.audioTimeS = timing.audioTimeS;
            sendOptions.currentTimeS = timing.currentTimeS;
        }

        this.#osc.send(preparedData, sendOptions);
    }

    /**
     * Get AudioContext instance (read-only)
     * @returns {AudioContext} The AudioContext instance
     */
    get audioContext() {
        return this.#audioContext;
    }

    /**
     * Get AudioWorkletNode instance (read-only)
     * @returns {AudioWorkletNode} The AudioWorkletNode instance
     */
    get workletNode() {
        return this.#workletNode;
    }

    /**
     * Get ScsynthOSC instance (read-only)
     * @returns {ScsynthOSC} The OSC communication layer instance
     */
    get osc() {
        return this.#osc;
    }

    /**
     * Get current status
     */
    getStatus() {
        return {
            initialized: this.#initialized,
            capabilities: this.#capabilities,
            bootStats: this.bootStats,
            audioContextState: this.#audioContext?.state
        };
    }

    /**
     * Get current configuration (merged defaults + user overrides)
     * Useful for debugging and displaying in UI
     * @returns {Object} Current scsynth configuration
     * @example
     * const config = sonic.getConfig();
     * console.log('Buffer limit:', config.worldOptions.numBuffers);
     * console.log('Memory layout:', config.memory);
     */
    getConfig() {
        if (!this.config) {
            return null;
        }

        // Return a deep clone to prevent external mutation
        return {
            memory: { ...this.config.memory },
            worldOptions: { ...this.config.worldOptions }
        };
    }

    /**
     * Destroy the orchestrator and clean up resources
     */
    async destroy() {
        if (__DEV__) console.log('[SuperSonic] Destroying...');

        // Stop timers
        this.#stopDriftOffsetTimer();
        this.#stopPerformanceMonitoring();

        if (this.#osc) {
            this.#osc.terminate();
            this.#osc = null;
        }

        if (this.#workletNode) {
            this.#workletNode.disconnect();
            this.#workletNode = null;
        }

        if (this.#audioContext) {
            await this.#audioContext.close();
            this.#audioContext = null;
        }

        // BufferManager handles its own cleanup
        if (this.#bufferManager) {
            this.#bufferManager.destroy();
            this.#bufferManager = null;
        }

        this.#sharedBuffer = null;
        this.#initialized = false;
        this.loadedSynthDefs.clear();

        if (__DEV__) console.log('[SuperSonic] Destroyed');
    }

    /**
     * Get NTP start time for bundle creation.
     * This is the NTP timestamp when AudioContext.currentTime was 0.
     * Bundles should have timestamp = audioContextTime + ntpStartTime
     */
    waitForTimeSync() {
        this.#ensureInitialized('wait for time sync');
        const ntpStartView = new Float64Array(this.#sharedBuffer, this.#ringBufferBase + this.#bufferConstants.NTP_START_TIME_START, 1);
        return ntpStartView[0];
    }

    /**
     * Load a sample into a buffer and wait for confirmation
     * @param {number} bufnum - Buffer number
     * @param {string} path - Audio file path
     * @returns {Promise} Resolves when buffer is ready
     */
    async loadSample(bufnum, path, startFrame = 0, numFrames = 0) {
        this.#ensureInitialized('load samples');

        const bufferInfo = await this.#requireBufferManager().prepareFromFile({
            bufnum,
            path,
            startFrame,
            numFrames
        });

        await this.send(
            '/b_allocPtr',
            bufnum,
            bufferInfo.ptr,
            bufferInfo.numFrames,
            bufferInfo.numChannels,
            bufferInfo.sampleRate,
            bufferInfo.uuid
        );

        return bufferInfo.allocationComplete;
    }

    /**
     * Load a binary synthdef file and send it to scsynth
     * @param {string} path - Path or URL to the .scsyndef file
     * @returns {Promise<void>}
     * @example
     * await sonic.loadSynthDef('./extra/synthdefs/sonic-pi-beep.scsyndef');
     */
    async loadSynthDef(path) {
        if (!this.#initialized) {
            throw new Error('SuperSonic not initialized. Call init() first.');
        }

        try {
            const response = await fetch(path);

            if (!response.ok) {
                throw new Error(`Failed to load synthdef from ${path}: ${response.status} ${response.statusText}`);
            }

            const arrayBuffer = await response.arrayBuffer();
            const synthdefData = new Uint8Array(arrayBuffer);

            // Send via /d_recv OSC message (fire and forget - will be synced by caller)
            await this.send('/d_recv', synthdefData);

            const synthName = this.#extractSynthDefName(path);
            if (synthName) {
                this.loadedSynthDefs.add(synthName);
            }

            if (__DEV__) console.log(`[SuperSonic] Sent synthdef from ${path} (${synthdefData.length} bytes)`);
        } catch (error) {
            console.error('[SuperSonic] Failed to load synthdef:', error);
            throw error;
        }
    }

    /**
     * Load multiple synthdefs from a directory
     * @param {string[]} names - Array of synthdef names (without .scsyndef extension)
     * @returns {Promise<Object>} Map of name -> success/error
     * @example
     * const results = await sonic.loadSynthDefs(['sonic-pi-beep', 'sonic-pi-tb303']);
     */
    async loadSynthDefs(names) {
        if (!this.#initialized) {
            throw new Error('SuperSonic not initialized. Call init() first.');
        }

        if (!this.#synthdefBaseURL) {
            throw new Error(
                'synthdefBaseURL not configured. Please set it in SuperSonic constructor options.\n' +
                'Example: new SuperSonic({ synthdefBaseURL: "./dist/synthdefs/" })\n' +
                'Or use CDN: new SuperSonic({ synthdefBaseURL: "https://unpkg.com/supersonic-scsynth-synthdefs@latest/synthdefs/" })\n' +
                'Or install: npm install supersonic-scsynth-synthdefs'
            );
        }

        const results = {};

        // Send all /d_recv commands in parallel
        await Promise.all(
            names.map(async (name) => {
                try {
                    const path = `${this.#synthdefBaseURL}${name}.scsyndef`;
                    await this.loadSynthDef(path);
                    results[name] = { success: true };
                } catch (error) {
                    console.error(`[SuperSonic] Failed to load ${name}:`, error);
                    results[name] = { success: false, error: error.message };
                }
            })
        );

        const successCount = Object.values(results).filter(r => r.success).length;
        if (__DEV__) console.log(`[SuperSonic] Sent ${successCount}/${names.length} synthdef loads`);

        return results;
    }

    /**
     * Send /sync command and wait for /synced response
     * Use this to ensure all previous asynchronous commands have completed
     * @param {number} syncId - Unique integer identifier for this sync operation
     * @returns {Promise<void>}
     * @example
     * await sonic.loadSynthDefs(['synth1', 'synth2']);
     * await sonic.sync(12345); // Wait for all synthdefs to be processed
     */
    async sync(syncId) {
        if (!this.#initialized) {
            throw new Error('SuperSonic not initialized. Call init() first.');
        }

        if (!Number.isInteger(syncId)) {
            throw new Error('sync() requires an integer syncId parameter');
        }

        const syncPromise = new Promise((resolve, reject) => {
            const timeout = setTimeout(() => {
                // Clean up listener on timeout
                if (this.#syncListeners) {
                    this.#syncListeners.delete(syncId);
                }
                reject(new Error('Timeout waiting for /synced response'));
            }, 10000); // 10 second timeout

            // Create a one-time message listener for this specific sync ID
            const messageHandler = (msg) => {
                clearTimeout(timeout);
                // Remove this specific listener
                this.#syncListeners.delete(syncId);
                resolve();
            };

            // Store the listener in a map keyed by sync ID
            if (!this.#syncListeners) {
                this.#syncListeners = new Map();
            }
            this.#syncListeners.set(syncId, messageHandler);
        });

        // Send /sync command
        await this.send('/sync', syncId);

        // Wait for /synced response
        await syncPromise;
    }

    /**
     * Allocate memory for an audio buffer (includes guard samples)
     * @param {number} numSamples - Number of Float32 samples to allocate
     * @returns {number} Byte offset into SharedArrayBuffer, or 0 if allocation failed
     * @example
     * const bufferAddr = sonic.allocBuffer(44100);  // Allocate 1 second at 44.1kHz
     */
    allocBuffer(numSamples) {
        this.#ensureInitialized('allocate buffers');
        return this.#bufferManager.allocate(numSamples);
    }

    /**
     * Free a previously allocated buffer
     * @param {number} addr - Buffer address returned by allocBuffer()
     * @returns {boolean} true if freed successfully
     * @example
     * sonic.freeBuffer(bufferAddr);
     */
    freeBuffer(addr) {
        this.#ensureInitialized('free buffers');
        return this.#bufferManager.free(addr);
    }

    /**
     * Get a Float32Array view of an allocated buffer
     * @param {number} addr - Buffer address returned by allocBuffer()
     * @param {number} numSamples - Number of Float32 samples
     * @returns {Float32Array} Typed array view into the buffer
     * @example
     * const view = sonic.getBufferView(bufferAddr, 44100);
     * view[0] = 1.0;  // Write to buffer
     */
    getBufferView(addr, numSamples) {
        this.#ensureInitialized('get buffer views');
        return this.#bufferManager.getView(addr, numSamples);
    }

    /**
     * Get buffer pool statistics
     * @returns {Object} Stats including total, available, used, etc.
     * @example
     * const stats = sonic.getBufferPoolStats();
     * console.log(`Available: ${stats.available} bytes`);
     */
    getBufferPoolStats() {
        this.#ensureInitialized('get buffer pool stats');
        return this.#bufferManager.getStats();
    }

    getDiagnostics() {
        this.#ensureInitialized('get diagnostics');

        return {
            buffers: this.#bufferManager.getDiagnostics(),
            synthdefs: {
                count: this.loadedSynthDefs.size
            }
        };
    }

    /**
     * Initialize NTP timing (write-once)
     * Sets the NTP start time when AudioContext started
     * Blocks until audio is actually flowing (contextTime > 0)
     * @private
     */
    async initializeNTPTiming() {
        if (!this.#bufferConstants || !this.#audioContext) {
            return;
        }

        // Wait for audio to actually be flowing (contextTime > 0)
        let timestamp;
        while (true) {
            timestamp = this.#audioContext.getOutputTimestamp();
            if (timestamp.contextTime > 0) {
                break;
            }
            await new Promise(resolve => setTimeout(resolve, 50));
        }

        // Get synchronized snapshot of both time domains
        const perfTimeMs = performance.timeOrigin + timestamp.performanceTime;
        const currentNTP = (perfTimeMs / 1000) + NTP_EPOCH_OFFSET;

        // NTP time at AudioContext start = current NTP - current AudioContext time
        const ntpStartTime = currentNTP - timestamp.contextTime;

        // Write to SharedArrayBuffer (write-once)
        const ntpStartView = new Float64Array(
            this.#sharedBuffer,
            this.#ringBufferBase + this.#bufferConstants.NTP_START_TIME_START,
            1
        );
        ntpStartView[0] = ntpStartTime;

        // Store for drift calculation
        this.#initialNTPStartTime = ntpStartTime;

        if (__DEV__) console.log(`[SuperSonic] NTP timing initialized: start=${ntpStartTime.toFixed(6)}s (NTP=${currentNTP.toFixed(3)}s, contextTime=${timestamp.contextTime.toFixed(3)}s)`);
    }

    /**
     * Update drift offset (AudioContext → NTP drift correction)
     * CRITICAL: This REPLACES the drift value, does not accumulate
     * @private
     */
    updateDriftOffset() {
        if (!this.#bufferConstants || !this.#audioContext || this.#initialNTPStartTime === undefined) {
            return;
        }

        // Get synchronized snapshot of both time domains (same moment in both clocks)
        const timestamp = this.#audioContext.getOutputTimestamp();
        const perfTimeMs = performance.timeOrigin + timestamp.performanceTime;
        const currentNTP = (perfTimeMs / 1000) + NTP_EPOCH_OFFSET;

        // Calculate where contextTime SHOULD be based on wall clock
        const expectedContextTime = currentNTP - this.#initialNTPStartTime;

        // Compare to actual contextTime to get drift
        // Positive = AudioContext running slow (behind wall clock, needs time added)
        // Negative = AudioContext running fast (ahead of wall clock, needs time subtracted)
        const driftSeconds = expectedContextTime - timestamp.contextTime;
        const driftMs = Math.round(driftSeconds * 1000);

        // Write to SharedArrayBuffer
        const driftView = new Int32Array(
            this.#sharedBuffer,
            this.#ringBufferBase + this.#bufferConstants.DRIFT_OFFSET_START,
            1
        );
        Atomics.store(driftView, 0, driftMs);

        if (__DEV__) console.log(`[SuperSonic] Drift offset: ${driftMs}ms (expected=${expectedContextTime.toFixed(3)}s, actual=${timestamp.contextTime.toFixed(3)}s, NTP=${currentNTP.toFixed(3)}s)`);
    }

    /**
     * Get current drift offset in milliseconds
     * @returns {number} Current drift in milliseconds
     */
    getDriftOffset() {
        if (!this.#bufferConstants) {
            return 0;
        }

        const driftView = new Int32Array(
            this.#sharedBuffer,
            this.#ringBufferBase + this.#bufferConstants.DRIFT_OFFSET_START,
            1
        );
        return Atomics.load(driftView, 0);
    }

    /**
     * Start periodic drift offset updates
     * @private
     */
    #startDriftOffsetTimer() {
        // Clear any existing timer
        this.#stopDriftOffsetTimer();

        // Update every DRIFT_UPDATE_INTERVAL_MS to track drift between AudioContext and performance.now()
        this.#driftOffsetTimer = setInterval(() => {
            this.updateDriftOffset();
        }, DRIFT_UPDATE_INTERVAL_MS);

        if (__DEV__) console.log(`[SuperSonic] Started drift offset correction (every ${DRIFT_UPDATE_INTERVAL_MS}ms)`);
    }

    /**
     * Stop periodic drift offset updates
     * @private
     */
    #stopDriftOffsetTimer() {
        if (this.#driftOffsetTimer) {
            clearInterval(this.#driftOffsetTimer);
            this.#driftOffsetTimer = null;
        }
    }

    #extractSynthDefName(path) {
        if (!path || typeof path !== 'string') {
            return null;
        }
        const lastSegment = path.split('/').filter(Boolean).pop() || path;
        return lastSegment.replace(/\.scsyndef$/i, '');
    }

    #toUint8Array(data) {
        if (data instanceof Uint8Array) {
            return data;
        }
        if (data instanceof ArrayBuffer) {
            return new Uint8Array(data);
        }
        throw new Error('oscData must be ArrayBuffer or Uint8Array');
    }

    async #prepareOutboundPacket(uint8Data) {
        const decodeOptions = { metadata: true, unpackSingleArgs: false };
        try {
            const decodedPacket = SuperSonic.osc.decode(uint8Data, decodeOptions);
            const { packet, changed } = await this.#rewritePacket(decodedPacket);
            if (!changed) {
                return uint8Data;
            }
            return SuperSonic.osc.encode(packet);
        } catch (error) {
            console.error('[SuperSonic] Failed to prepare OSC packet:', error);
            throw error;
        }
    }

    async #rewritePacket(packet) {
        if (packet && packet.address) {
            const { message, changed } = await this.#rewriteMessage(packet);
            return { packet: message, changed };
        }

        if (this.#isBundle(packet)) {
            const subResults = await Promise.all(
                packet.packets.map((subPacket) => this.#rewritePacket(subPacket))
            );

            const changed = subResults.some(result => result.changed);

            if (!changed) {
                return { packet, changed: false };
            }

            const rewrittenPackets = subResults.map(result => result.packet);

            return {
                packet: {
                    timeTag: packet.timeTag,
                    packets: rewrittenPackets
                },
                changed: true
            };
        }

        return { packet, changed: false };
    }

    async #rewriteMessage(message) {
        switch (message.address) {
            case '/b_alloc':
                return {
                    message: await this.#rewriteAlloc(message),
                    changed: true
                };
            case '/b_allocRead':
                return {
                    message: await this.#rewriteAllocRead(message),
                    changed: true
                };
            case '/b_allocReadChannel':
                return {
                    message: await this.#rewriteAllocReadChannel(message),
                    changed: true
                };
            default:
                return { message, changed: false };
        }
    }

    async #rewriteAllocRead(message) {
        const bufferManager = this.#requireBufferManager();
        const bufnum = this.#requireIntArg(message.args, 0, '/b_allocRead requires a buffer number');
        const path = this.#requireStringArg(message.args, 1, '/b_allocRead requires a file path');
        const startFrame = this.#optionalIntArg(message.args, 2, 0);
        const numFrames = this.#optionalIntArg(message.args, 3, 0);

        const bufferInfo = await bufferManager.prepareFromFile({
            bufnum,
            path,
            startFrame,
            numFrames
        });

        this.#detachAllocationPromise(bufferInfo.allocationComplete, `/b_allocRead ${bufnum}`);
        return this.#buildAllocPtrMessage(bufnum, bufferInfo);
    }

    async #rewriteAllocReadChannel(message) {
        const bufferManager = this.#requireBufferManager();
        const bufnum = this.#requireIntArg(message.args, 0, '/b_allocReadChannel requires a buffer number');
        const path = this.#requireStringArg(message.args, 1, '/b_allocReadChannel requires a file path');
        const startFrame = this.#optionalIntArg(message.args, 2, 0);
        const numFrames = this.#optionalIntArg(message.args, 3, 0);

        const channels = [];
        for (let i = 4; i < (message.args?.length || 0); i++) {
            if (!this.#isNumericArg(message.args[i])) {
                break;
            }
            channels.push(Math.floor(this.#getArgValue(message.args[i])));
        }

        const bufferInfo = await bufferManager.prepareFromFile({
            bufnum,
            path,
            startFrame,
            numFrames,
            channels: channels.length > 0 ? channels : null
        });

        this.#detachAllocationPromise(bufferInfo.allocationComplete, `/b_allocReadChannel ${bufnum}`);
        return this.#buildAllocPtrMessage(bufnum, bufferInfo);
    }

    async #rewriteAlloc(message) {
        const bufferManager = this.#requireBufferManager();
        const bufnum = this.#requireIntArg(message.args, 0, '/b_alloc requires a buffer number');
        const numFrames = this.#requireIntArg(message.args, 1, '/b_alloc requires a frame count');

        let argIndex = 2;
        let numChannels = 1;
        let sampleRate = this.#audioContext?.sampleRate || 44100;

        if (this.#isNumericArg(this.#argAt(message.args, argIndex))) {
            numChannels = Math.max(1, this.#optionalIntArg(message.args, argIndex, 1));
            argIndex++;
        }

        if (this.#argAt(message.args, argIndex)?.type === 'b') {
            argIndex++;
        }

        if (this.#isNumericArg(this.#argAt(message.args, argIndex))) {
            sampleRate = this.#getArgValue(this.#argAt(message.args, argIndex));
        }

        const bufferInfo = await bufferManager.prepareEmpty({
            bufnum,
            numFrames,
            numChannels,
            sampleRate
        });

        this.#detachAllocationPromise(bufferInfo.allocationComplete, `/b_alloc ${bufnum}`);
        return this.#buildAllocPtrMessage(bufnum, bufferInfo);
    }

    #buildAllocPtrMessage(bufnum, bufferInfo) {
        return {
            address: '/b_allocPtr',
            args: [
                this.#intArg(bufnum),
                this.#intArg(bufferInfo.ptr),
                this.#intArg(bufferInfo.numFrames),
                this.#intArg(bufferInfo.numChannels),
                this.#floatArg(bufferInfo.sampleRate),
                this.#stringArg(bufferInfo.uuid)
            ]
        };
    }

    #intArg(value) {
        return { type: 'i', value: Math.floor(value) };
    }

    #floatArg(value) {
        return { type: 'f', value };
    }

    #stringArg(value) {
        return { type: 's', value: String(value) };
    }

    #argAt(args, index) {
        if (!Array.isArray(args)) {
            return undefined;
        }
        return args[index];
    }

    #getArgValue(arg) {
        if (arg === undefined || arg === null) {
            return undefined;
        }
        return typeof arg === 'object' && Object.prototype.hasOwnProperty.call(arg, 'value')
            ? arg.value
            : arg;
    }

    #requireIntArg(args, index, errorMessage) {
        const value = this.#getArgValue(this.#argAt(args, index));
        if (!Number.isFinite(value)) {
            throw new Error(errorMessage);
        }
        return Math.floor(value);
    }

    #optionalIntArg(args, index, defaultValue = 0) {
        const value = this.#getArgValue(this.#argAt(args, index));
        if (!Number.isFinite(value)) {
            return defaultValue;
        }
        return Math.floor(value);
    }

    #requireStringArg(args, index, errorMessage) {
        const value = this.#getArgValue(this.#argAt(args, index));
        if (typeof value !== 'string') {
            throw new Error(errorMessage);
        }
        return value;
    }

    #isNumericArg(arg) {
        if (!arg) {
            return false;
        }
        const value = this.#getArgValue(arg);
        return Number.isFinite(value);
    }

    #detachAllocationPromise(promise, context) {
        if (!promise || typeof promise.catch !== 'function') {
            return;
        }

        promise.catch((error) => {
            console.error(`[SuperSonic] ${context} allocation failed:`, error);
        });
    }

    #requireBufferManager() {
        if (!this.#bufferManager) {
            throw new Error('Buffer manager not ready. Call init() before issuing buffer commands.');
        }
        return this.#bufferManager;
    }

    #isBundle(packet) {
        return packet && packet.timeTag !== undefined && Array.isArray(packet.packets);
    }

    #calculateBundleWait(uint8Data) {
        if (uint8Data.length < 16) {
            return null;
        }

        const header = String.fromCharCode.apply(null, uint8Data.slice(0, 8));
        if (header !== '#bundle\0') {
            return null;
        }

        // Read NTP start time (write-once value)
        const ntpStartView = new Float64Array(this.#sharedBuffer, this.#ringBufferBase + this.#bufferConstants.NTP_START_TIME_START, 1);
        const ntpStartTime = ntpStartView[0];

        if (ntpStartTime === 0) {
            console.warn('[SuperSonic] NTP start time not yet initialized');
            return null;
        }

        // Read current drift offset (milliseconds)
        const driftView = new Int32Array(this.#sharedBuffer, this.#ringBufferBase + this.#bufferConstants.DRIFT_OFFSET_START, 1);
        const driftMs = Atomics.load(driftView, 0);
        const driftSeconds = driftMs / 1000.0;

        // Read global offset (milliseconds)
        const globalView = new Int32Array(this.#sharedBuffer, this.#ringBufferBase + this.#bufferConstants.GLOBAL_OFFSET_START, 1);
        const globalMs = Atomics.load(globalView, 0);
        const globalSeconds = globalMs / 1000.0;

        const totalOffset = ntpStartTime + driftSeconds + globalSeconds;

        const view = new DataView(uint8Data.buffer, uint8Data.byteOffset);
        const ntpSeconds = view.getUint32(8, false);
        const ntpFraction = view.getUint32(12, false);

        if (ntpSeconds === 0 && (ntpFraction === 0 || ntpFraction === 1)) {
            return null;
        }

        const ntpTimeS = ntpSeconds + (ntpFraction / 0x100000000);
        const audioTimeS = ntpTimeS - totalOffset;
        const currentTimeS = this.#audioContext.currentTime;

        // Return the target audio time, not the wait time
        // The scheduler will handle lookahead scheduling
        return { audioTimeS, currentTimeS };
    }
}

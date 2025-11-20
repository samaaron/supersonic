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
import oscLib from './vendor/osc.js/osc.js';
import { MemPool } from '@thi.ng/malloc';
import { NTP_EPOCH_OFFSET, DRIFT_UPDATE_INTERVAL_MS } from './timing_constants.js';
import { ScsynthConfig } from './scsynth_options.js';

class BufferManager {
    constructor(options) {
        const {
            audioContext,
            sharedBuffer,
            bufferPool,
            allocatedBuffers,
            resolveAudioPath,
            registerPendingOp,
            maxBuffers = 1024
        } = options;

        this.audioContext = audioContext;
        this.sharedBuffer = sharedBuffer;
        this.bufferPool = bufferPool;
        this.allocatedBuffers = allocatedBuffers;
        this.resolveAudioPath = resolveAudioPath;
        this.registerPendingOp = registerPendingOp;
        this.bufferLocks = new Map(); // bufnum -> promise chain tail

        // Guard samples prevent interpolation artifacts at buffer boundaries.
        // SuperCollider uses 3 samples before and 1 sample after for cubic interpolation.
        this.GUARD_BEFORE = 3;
        this.GUARD_AFTER = 1;

        // Maximum buffer count (from config)
        this.MAX_BUFFERS = maxBuffers;
    }

    #validateBufferNumber(bufnum) {
        if (!Number.isInteger(bufnum) || bufnum < 0 || bufnum >= this.MAX_BUFFERS) {
            throw new Error(`Invalid buffer number ${bufnum} (must be 0-${this.MAX_BUFFERS - 1})`);
        }
    }

    async prepareFromFile(params) {
        const {
            bufnum,
            path,
            startFrame = 0,
            numFrames = 0,
            channels = null
        } = params;

        this.#validateBufferNumber(bufnum);

        let allocatedPtr = null;
        let pendingToken = null;
        let allocationRegistered = false;

        const releaseLock = await this.#acquireBufferLock(bufnum);
        let lockReleased = false;

        try {
            await this.#awaitPendingReplacement(bufnum);

            const resolvedPath = this.resolveAudioPath(path);
            const response = await fetch(resolvedPath);

            if (!response.ok) {
                throw new Error(`Failed to fetch ${resolvedPath}: ${response.status} ${response.statusText}`);
            }

            const arrayBuffer = await response.arrayBuffer();
            const audioBuffer = await this.audioContext.decodeAudioData(arrayBuffer);

            const start = Math.max(0, Math.floor(startFrame || 0));
            const availableFrames = audioBuffer.length - start;
            const framesRequested = numFrames && numFrames > 0
                ? Math.min(Math.floor(numFrames), availableFrames)
                : availableFrames;

            if (framesRequested <= 0) {
                throw new Error(`No audio frames available for buffer ${bufnum} from ${path}`);
            }

            const selectedChannels = this.#normalizeChannels(channels, audioBuffer.numberOfChannels);
            const numChannels = selectedChannels.length;

            const totalSamples = (framesRequested * numChannels) +
                ((this.GUARD_BEFORE + this.GUARD_AFTER) * numChannels);

            allocatedPtr = this.#malloc(totalSamples);
            const interleaved = new Float32Array(totalSamples);
            const dataOffset = this.GUARD_BEFORE * numChannels;

            for (let frame = 0; frame < framesRequested; frame++) {
                for (let ch = 0; ch < numChannels; ch++) {
                    const sourceChannel = selectedChannels[ch];
                    const channelData = audioBuffer.getChannelData(sourceChannel);
                    interleaved[dataOffset + (frame * numChannels) + ch] =
                        channelData[start + frame];
                }
            }

            this.#writeToSharedBuffer(allocatedPtr, interleaved);
            const sizeBytes = interleaved.length * 4;

            const { uuid, allocationComplete } = this.#registerPending(bufnum);
            pendingToken = uuid;
            this.#recordAllocation(bufnum, allocatedPtr, sizeBytes, uuid, allocationComplete);
            allocationRegistered = true;
            const managedCompletion = this.#attachFinalizer(bufnum, uuid, allocationComplete);
            releaseLock();
            lockReleased = true;

            return {
                ptr: allocatedPtr,
                numFrames: framesRequested,
                numChannels,
                sampleRate: audioBuffer.sampleRate,
                uuid,
                allocationComplete: managedCompletion
            };
        } catch (error) {
            if (allocationRegistered && pendingToken) {
                this.#finalizeReplacement(bufnum, pendingToken, false);
            } else if (allocatedPtr) {
                this.bufferPool.free(allocatedPtr);
            }
            throw error;
        } finally {
            if (!lockReleased) {
                releaseLock();
            }
        }
    }

    async prepareEmpty(params) {
        const {
            bufnum,
            numFrames,
            numChannels = 1,
            sampleRate = null
        } = params;

        this.#validateBufferNumber(bufnum);

        let allocationRegistered = false;
        let pendingToken = null;
        let allocatedPtr = null;

        if (!Number.isFinite(numFrames) || numFrames <= 0) {
            throw new Error(`/b_alloc requires a positive number of frames (got ${numFrames})`);
        }

        if (!Number.isFinite(numChannels) || numChannels <= 0) {
            throw new Error(`/b_alloc requires a positive channel count (got ${numChannels})`);
        }

        const roundedFrames = Math.floor(numFrames);
        const roundedChannels = Math.floor(numChannels);
        const totalSamples = (roundedFrames * roundedChannels) +
            ((this.GUARD_BEFORE + this.GUARD_AFTER) * roundedChannels);

        const releaseLock = await this.#acquireBufferLock(bufnum);
        let lockReleased = false;

        try {
            await this.#awaitPendingReplacement(bufnum);

            allocatedPtr = this.#malloc(totalSamples);
            const interleaved = new Float32Array(totalSamples);
            this.#writeToSharedBuffer(allocatedPtr, interleaved);
            const sizeBytes = interleaved.length * 4;

            const { uuid, allocationComplete } = this.#registerPending(bufnum);
            pendingToken = uuid;
            this.#recordAllocation(bufnum, allocatedPtr, sizeBytes, uuid, allocationComplete);
            allocationRegistered = true;
            const managedCompletion = this.#attachFinalizer(bufnum, uuid, allocationComplete);
            releaseLock();
            lockReleased = true;

            return {
                ptr: allocatedPtr,
                numFrames: roundedFrames,
                numChannels: roundedChannels,
                sampleRate: sampleRate || this.audioContext.sampleRate,
                uuid,
                allocationComplete: managedCompletion
            };
        } catch (error) {
            if (allocationRegistered && pendingToken) {
                this.#finalizeReplacement(bufnum, pendingToken, false);
            } else if (allocatedPtr) {
                this.bufferPool.free(allocatedPtr);
            }
            throw error;
        } finally {
            if (!lockReleased) {
                releaseLock();
            }
        }
    }

    #normalizeChannels(requestedChannels, fileChannels) {
        if (!requestedChannels || requestedChannels.length === 0) {
            return Array.from({ length: fileChannels }, (_, i) => i);
        }

        requestedChannels.forEach((channel) => {
            if (!Number.isInteger(channel) || channel < 0 || channel >= fileChannels) {
                throw new Error(`Channel ${channel} is out of range (file has ${fileChannels} channels)`);
            }
        });

        return requestedChannels;
    }

    #malloc(totalSamples) {
        const bytesNeeded = totalSamples * 4;
        const ptr = this.bufferPool.malloc(bytesNeeded);

        if (ptr === 0) {
            const stats = this.bufferPool.stats();
            const availableMB = ((stats.available || 0) / (1024 * 1024)).toFixed(2);
            const totalMB = ((stats.total || 0) / (1024 * 1024)).toFixed(2);
            const requestedMB = (bytesNeeded / (1024 * 1024)).toFixed(2);
            throw new Error(
                `Buffer pool allocation failed: requested ${requestedMB}MB, ` +
                `available ${availableMB}MB of ${totalMB}MB total`
            );
        }

        return ptr;
    }

    #writeToSharedBuffer(ptr, data) {
        const heap = new Float32Array(this.sharedBuffer, ptr, data.length);
        heap.set(data);
    }

    #registerPending(bufnum) {
        if (!this.registerPendingOp) {
            return {
                uuid: crypto.randomUUID(),
                allocationComplete: Promise.resolve()
            };
        }

        const uuid = crypto.randomUUID();
        const allocationComplete = this.registerPendingOp(uuid, bufnum);
        return { uuid, allocationComplete };
    }

    async #acquireBufferLock(bufnum) {
        const prev = this.bufferLocks.get(bufnum) || Promise.resolve();
        let releaseLock;
        const current = new Promise((resolve) => {
            releaseLock = resolve;
        });
        this.bufferLocks.set(bufnum, prev.then(() => current));
        await prev;

        return () => {
            if (releaseLock) {
                releaseLock();
                releaseLock = null;
            }
            if (this.bufferLocks.get(bufnum) === current) {
                this.bufferLocks.delete(bufnum);
            }
        };
    }

    #recordAllocation(bufnum, ptr, sizeBytes, pendingToken, pendingPromise) {
        const previousEntry = this.allocatedBuffers.get(bufnum);
        const entry = {
            ptr,
            size: sizeBytes,
            pendingToken,
            pendingPromise,
            previousAllocation: previousEntry
                ? { ptr: previousEntry.ptr, size: previousEntry.size }
                : null
        };
        this.allocatedBuffers.set(bufnum, entry);
        return entry;
    }

    async #awaitPendingReplacement(bufnum) {
        const existing = this.allocatedBuffers.get(bufnum);
        if (existing && existing.pendingToken && existing.pendingPromise) {
            try {
                await existing.pendingPromise;
            } catch {
                // Ignore failures; finalizer already handled cleanup
            }
        }
    }

    #attachFinalizer(bufnum, pendingToken, promise) {
        if (!promise || typeof promise.then !== 'function') {
            this.#finalizeReplacement(bufnum, pendingToken, true);
            return Promise.resolve();
        }

        return promise.then((value) => {
            this.#finalizeReplacement(bufnum, pendingToken, true);
            return value;
        }).catch((error) => {
            this.#finalizeReplacement(bufnum, pendingToken, false);
            throw error;
        });
    }

    #finalizeReplacement(bufnum, pendingToken, success) {
        const entry = this.allocatedBuffers.get(bufnum);
        if (!entry || entry.pendingToken !== pendingToken) {
            return;
        }

        const previous = entry.previousAllocation;

        if (success) {
            entry.pendingToken = null;
            entry.pendingPromise = null;
            entry.previousAllocation = null;
            if (previous?.ptr) {
                this.bufferPool.free(previous.ptr);
            }
            return;
        }

        if (entry.ptr) {
            this.bufferPool.free(entry.ptr);
        }

        entry.pendingPromise = null;

        if (previous?.ptr) {
            this.allocatedBuffers.set(bufnum, {
                ptr: previous.ptr,
                size: previous.size,
                pendingToken: null,
                previousAllocation: null
            });
        } else {
            this.allocatedBuffers.delete(bufnum);
        }
    }
}

export class SuperSonic {
    // Expose OSC utilities as static methods
    static osc = {
        encode: (message) => oscLib.writePacket(message),
        decode: (data, options = { metadata: false }) => oscLib.readPacket(data, options)
    };
    constructor(options = {}) {
        this.initialized = false;
        this.initializing = false;
        this.capabilities = {};

        // Core components
        this.sharedBuffer = null;
        this.ringBufferBase = null;
        this.bufferConstants = null;
        this.audioContext = null;
        this.workletNode = null;
        this.osc = null;  // ScsynthOSC instance for OSC communication
        this.wasmModule = null;
        this.wasmInstance = null;
        this.bufferPool = null;  // MemPool for audio buffer allocation
        this.bufferManager = null;
        this.loadedSynthDefs = new Set();

        // Pending buffer operations map for UUID correlation
        this.pendingBufferOps = new Map();  // UUID -> {resolve, reject, timeout}

        // Time offset promise (resolves when buffer constants are initialized)
        this._timeOffsetPromise = null;
        this._resolveTimeOffset = null;
        this._localClockOffsetTimer = null;  // Timer for periodic drift correction

        // Callbacks
        this.onOSC = null;              // Raw binary OSC from scsynth (for display/logging)
        this.onMessage = null;          // Parsed OSC messages from scsynth (for application logic)
        this.onMessageSent = null;
        this.onMetricsUpdate = null;
        this.onStatusUpdate = null;
        this.onSendError = null;
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

        // Merge user-provided scsynth options with defaults
        const scsynthConfig = this.#mergeScsynthOptions(options.scsynthOptions || {});

        this.config = {
            wasmUrl: options.wasmUrl || wasmBaseURL + 'scsynth-nrt.wasm',
            workletUrl: options.workletUrl || workerBaseURL + 'scsynth_audio_worklet.js',
            workerBaseURL: workerBaseURL,  // Store for worker creation
            development: false,
            audioContextOptions: {
                latencyHint: 'interactive',
                sampleRate: 48000
            },
            // scsynth configuration (merged defaults + user overrides)
            scsynth: scsynthConfig
        };

        // Resource loading configuration
        this.sampleBaseURL = options.sampleBaseURL || null;
        this.synthdefBaseURL = options.synthdefBaseURL || null;
        this.audioPathMap = options.audioPathMap || {};

        // Track allocated buffers for cleanup
        this.allocatedBuffers = new Map();  // bufnum -> { ptr, size }

        // Stats
        this.stats = {
            initStartTime: null,
            initDuration: null,
            messagesSent: 0,
            messagesReceived: 0,
            errors: 0
        };
    }

    /**
     * Check browser capabilities for required features
     */
    checkCapabilities() {
        this.capabilities = {
            audioWorklet: 'AudioWorklet' in window,
            sharedArrayBuffer: typeof SharedArrayBuffer !== 'undefined',
            crossOriginIsolated: window.crossOriginIsolated === true,
            wasmThreads: typeof WebAssembly !== 'undefined' &&
                        typeof WebAssembly.Memory !== 'undefined' &&
                        WebAssembly.Memory.prototype.hasOwnProperty('shared'),
            atomics: typeof Atomics !== 'undefined',
            webWorker: typeof Worker !== 'undefined'
        };

        // Check for required features (wasmThreads is optional - we can use mock)
        const required = ['audioWorklet', 'sharedArrayBuffer', 'crossOriginIsolated',
                         'atomics', 'webWorker'];
        const missing = required.filter(f => !this.capabilities[f]);

        if (missing.length > 0) {
            const error = new Error(`Missing required features: ${missing.join(', ')}`);

            // Special case for cross-origin isolation
            if (!this.capabilities.crossOriginIsolated) {
                if (this.capabilities.sharedArrayBuffer) {
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

        return this.capabilities;
    }

    /**
     * Merge user-provided scsynth options with defaults
     * @private
     */
    #mergeScsynthOptions(userOptions) {
        // Deep clone defaults to avoid mutation
        const merged = {
            memory: { ...ScsynthConfig.memory },
            worldOptions: { ...ScsynthConfig.worldOptions }
        };

        // Merge user overrides
        if (userOptions.memory) {
            Object.assign(merged.memory, userOptions.memory);
        }
        if (userOptions.worldOptions) {
            Object.assign(merged.worldOptions, userOptions.worldOptions);
        }

        // Also accept top-level worldOptions (shorthand)
        // e.g., { numBuffers: 2048 } instead of { worldOptions: { numBuffers: 2048 } }
        const topLevelKeys = Object.keys(userOptions).filter(
            key => key !== 'memory' && key !== 'worldOptions'
        );
        if (topLevelKeys.length > 0) {
            topLevelKeys.forEach(key => {
                if (key in merged.worldOptions) {
                    merged.worldOptions[key] = userOptions[key];
                }
            });
        }

        return merged;
    }

    /**
     * Initialize shared WebAssembly memory
     */
    #initializeSharedMemory() {
        // Memory layout (from scsynth_options.js):
        // 0-32MB:     Emscripten heap (scsynth objects, stack)
        // 32-64MB:    Ring buffers (OSC in/out, debug, control)
        // 64-192MB:   Buffer pool (128MB for audio buffers)
        const memConfig = this.config.scsynth.memory;

        this.wasmMemory = new WebAssembly.Memory({
            initial: memConfig.totalPages,
            maximum: memConfig.totalPages,
            shared: true
        });
        this.sharedBuffer = this.wasmMemory.buffer;

        // Initialize buffer pool
        this.bufferPool = new MemPool({
            buf: this.sharedBuffer,
            start: memConfig.bufferPoolOffset,
            size: memConfig.bufferPoolSize,
            align: 8  // 8-byte alignment (minimum required by MemPool)
        });

        const poolSizeMB = (memConfig.bufferPoolSize / (1024 * 1024)).toFixed(0);
        const poolOffsetMB = (memConfig.bufferPoolOffset / (1024 * 1024)).toFixed(0);
        console.log(`[SuperSonic] Buffer pool initialized: ${poolSizeMB}MB at offset ${poolOffsetMB}MB`);
    }


    /**
     * Initialize AudioContext and set up time offset calculation
     */
    #initializeAudioContext() {
        this.audioContext = new (window.AudioContext || window.webkitAudioContext)(
            this.config.audioContextOptions
        );

        // Create promise that will resolve when buffer constants are initialized
        // and local clock offset is set up (happens in worklet initialization)
        this._timeOffsetPromise = new Promise((resolve) => {
            this._resolveTimeOffset = resolve;
        });

        // Handle suspended context
        if (this.audioContext.state === 'suspended') {
            // Add one-time listener for user interaction
            const resumeContext = async () => {
                if (this.audioContext.state === 'suspended') {
                    await this.audioContext.resume();
                }
            };

            document.addEventListener('click', resumeContext, { once: true });
            document.addEventListener('touchstart', resumeContext, { once: true });
        }

        return this.audioContext;
    }

    #initializeBufferManager() {
        this.bufferManager = new BufferManager({
            audioContext: this.audioContext,
            sharedBuffer: this.sharedBuffer,
            bufferPool: this.bufferPool,
            allocatedBuffers: this.allocatedBuffers,
            resolveAudioPath: (path) => this._resolveAudioPath(path),
            registerPendingOp: (uuid, bufnum, timeoutMs) =>
                this.#createPendingBufferOperation(uuid, bufnum, timeoutMs),
            maxBuffers: this.config.scsynth.worldOptions.numBuffers
        });
    }

    /**
     * Load WASM manifest to get the current hashed filename
     */
    async #loadWasmManifest() {
        try {
            const wasmBaseURL = this.config.workerBaseURL.replace('/workers/', '/wasm/');
            const manifestUrl = wasmBaseURL + 'manifest.json';
            const response = await fetch(manifestUrl);
            if (response.ok) {
                const manifest = await response.json();

                // Use the WASM file specified in manifest
                const wasmFile = manifest.wasmFile;

                this.config.wasmUrl = wasmBaseURL + wasmFile;
                console.log(`[SuperSonic] Using WASM build: ${wasmFile}`);
                console.log(`[SuperSonic] Build: ${manifest.buildId} (git: ${manifest.gitHash})`);
            }
        } catch (error) {
            // Fallback to non-hashed filename if manifest not found
            console.warn('[SuperSonic] WASM manifest not found, using default filename');
        }
    }

    /**
     * Load WASM binary from network
     */
    async #loadWasm() {
        // Load manifest first to get the hashed filename
        await this.#loadWasmManifest();

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
        await this.audioContext.audioWorklet.addModule(this.config.workletUrl);

        // Create AudioWorkletNode
        // Configure with numberOfInputs: 0 to act as a source node
        // This ensures process() is called continuously without needing an input source
        this.workletNode = new AudioWorkletNode(this.audioContext, 'scsynth-processor', {
            numberOfInputs: 0,
            numberOfOutputs: 1,
            outputChannelCount: [2]
        });

        // Connect to audio graph to trigger process() calls
        this.workletNode.connect(this.audioContext.destination);

        // Initialize AudioWorklet with SharedArrayBuffer
        this.workletNode.port.postMessage({
            type: 'init',
            sharedBuffer: this.sharedBuffer
        });

        // Send WASM bytes, memory, worldOptions, and actual sample rate
        this.workletNode.port.postMessage({
            type: 'loadWasm',
            wasmBytes: wasmBytes,
            wasmMemory: this.wasmMemory,
            worldOptions: this.config.scsynth.worldOptions,
            sampleRate: this.audioContext.sampleRate  // Pass actual AudioContext sample rate
        });

        // Wait for worklet initialization
        await this.#waitForWorkletInit();
    }

    /**
     * Initialize OSC communication layer
     */
    async #initializeOSC() {
        // Create ScsynthOSC instance with custom worker base URL if provided
        this.osc = new ScsynthOSC(this.config.workerBaseURL);

        // Set up ScsynthOSC callbacks
        this.osc.onRawOSC((msg) => {
            // Forward raw binary OSC to onOSC callback (for display/logging)
            if (this.onOSC) {
                this.onOSC(msg);
            }
        });

        this.osc.onParsedOSC((msg) => {
            // Handle internal messages
            if (msg.address === '/buffer/freed') {
                this._handleBufferFreed(msg.args);
            } else if (msg.address === '/buffer/allocated') {
                // Handle buffer allocation completion with UUID correlation
                this._handleBufferAllocated(msg.args);
            } else if (msg.address === '/synced' && msg.args.length > 0) {
                // Handle /synced responses for sync operations
                const syncId = msg.args[0];  // Integer sync ID
                if (this._syncListeners && this._syncListeners.has(syncId)) {
                    const listener = this._syncListeners.get(syncId);
                    listener(msg);
                }
            }

            // Always forward to onMessage (including internal messages)
            if (this.onMessage) {
                this.stats.messagesReceived++;
                this.onMessage(msg);
            }
        });

        this.osc.onDebugMessage((msg) => {
            if (this.onDebugMessage) {
                this.onDebugMessage(msg);
            }
        });

        this.osc.onError((error, workerName) => {
            console.error(`[SuperSonic] ${workerName} error:`, error);
            this.stats.errors++;
            if (this.onError) {
                this.onError(new Error(`${workerName}: ${error}`));
            }
        });

        // Initialize ScsynthOSC with SharedArrayBuffer, ring buffer base, and buffer constants
        await this.osc.init(this.sharedBuffer, this.ringBufferBase, this.bufferConstants);
    }

    /**
     * Complete initialization and trigger callbacks
     */
    #finishInitialization() {
        this.initialized = true;
        this.initializing = false;
        this.stats.initDuration = performance.now() - this.stats.initStartTime;

        console.log(`[SuperSonic] Initialization complete in ${this.stats.initDuration.toFixed(2)}ms`);

        if (this.onInitialized) {
            this.onInitialized({
                capabilities: this.capabilities,
                stats: this.stats
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
        if (this.initialized) {
            console.warn('[SuperSonic] Already initialized');
            return;
        }

        if (this.initializing) {
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

        this.initializing = true;
        this.stats.initStartTime = performance.now();

        try {
            this.checkCapabilities();
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
            this.initializing = false;
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

            const messageHandler = (event) => {
                // Handle debug messages during initialization
                if (event.data.type === 'debug') {
                    // Silent during init
                    return;
                }

                // Handle error messages during initialization
                if (event.data.type === 'error') {
                    console.error('[AudioWorklet] Error:', event.data.error);
                    clearTimeout(timeout);
                    this.workletNode.port.removeEventListener('message', messageHandler);
                    reject(new Error(event.data.error || 'AudioWorklet error'));
                    return;
                }

                if (event.data.type === 'initialized') {
                    clearTimeout(timeout);
                    this.workletNode.port.removeEventListener('message', messageHandler);

                    if (event.data.success) {
                        // Store the ring buffer base address and constants from WASM
                        if (event.data.ringBufferBase !== undefined) {
                            this.ringBufferBase = event.data.ringBufferBase;
                        } else {
                            console.warn('[SuperSonic] Warning: ringBufferBase not provided by worklet');
                        }

                        if (event.data.bufferConstants !== undefined) {
                            console.log('[SuperSonic] Received bufferConstants from worklet');
                            this.bufferConstants = event.data.bufferConstants;

                            // Initialize NTP timing (write-once: NTP time at AudioContext start)
                            console.log('[SuperSonic] Initializing NTP timing');
                            this.initializeNTPTiming();

                            // Start periodic drift offset updates (small millisecond adjustments)
                            // Measures drift from initial baseline, replaces value (doesn't accumulate)
                            this.#startDriftOffsetTimer();

                            // Resolve time offset promise now that local clock offset is initialized
                            console.log('[SuperSonic] Resolving time offset promise, _resolveTimeOffset=', this._resolveTimeOffset);
                            if (this._resolveTimeOffset) {
                                this._resolveTimeOffset();
                                this._resolveTimeOffset = null;
                            }
                        } else {
                            console.warn('[SuperSonic] Warning: bufferConstants not provided by worklet');
                        }

                        console.log('[SuperSonic] Calling resolve() for worklet initialization');
                        resolve();
                    } else {
                        reject(new Error(event.data.error || 'AudioWorklet initialization failed'));
                    }
                }
            };

            this.workletNode.port.addEventListener('message', messageHandler);
            this.workletNode.port.start();
        });
    }


    /**
     * Set up message handlers for worklet
     */
    #setupMessageHandlers() {
        // ScsynthOSC handles all worker messages internally
        // We only need to handle worklet messages here

        // Worklet message handler
        this.workletNode.port.onmessage = (event) => {
            const { data } = event;

            switch (data.type) {
                case 'status':
                    // Status update from worklet
                    if (this.onStatusUpdate) {
                        this.onStatusUpdate(data);
                    }
                    break;

                case 'metrics':
                    // Performance metrics from worklet
                    if (this.onMetricsUpdate) {
                        this.onMetricsUpdate(data.metrics);
                    }
                    break;

                case 'error':
                    console.error('[Worklet] Error:', data.error);
                    if (data.diagnostics) {
                        console.error('[Worklet] Diagnostics:', data.diagnostics);
                        console.table(data.diagnostics);
                    }
                    this.stats.errors++;
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
     * Start performance monitoring
     */
    #startPerformanceMonitoring() {
        // Request metrics periodically
        setInterval(() => {
            if (this.osc) {
                // Get stats from ScsynthOSC
                this.osc.getStats().then(stats => {
                    if (stats && this.onMetricsUpdate) {
                        this.onMetricsUpdate(stats);
                    }
                });
            }
            if (this.workletNode) {
                this.workletNode.port.postMessage({ type: 'getMetrics' });
            }
        }, 50);
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

    /**
     * Resolve audio file path to full URL
     */
    _resolveAudioPath(scPath) {
        // Explicit mapping takes precedence
        if (this.audioPathMap[scPath]) {
            return this.audioPathMap[scPath];
        }

        // Check if sampleBaseURL is configured
        if (!this.sampleBaseURL) {
            throw new Error(
                'sampleBaseURL not configured. Please set it in SuperSonic constructor options.\n' +
                'Example: new SuperSonic({ sampleBaseURL: "./dist/samples/" })\n' +
                'Or use CDN: new SuperSonic({ sampleBaseURL: "https://unpkg.com/supersonic-scsynth-samples@latest/samples/" })\n' +
                'Or install: npm install supersonic-scsynth-samples'
            );
        }

        // Otherwise prepend base URL
        return this.sampleBaseURL + scPath;
    }

    #ensureInitialized(actionDescription = 'perform this operation') {
        if (!this.initialized) {
            throw new Error(`SuperSonic not initialized. Call init() before attempting to ${actionDescription}.`);
        }
    }

    #createPendingBufferOperation(uuid, bufnum, timeoutMs = 30000) {
        return new Promise((resolve, reject) => {
            const timeout = setTimeout(() => {
                this.pendingBufferOps.delete(uuid);
                reject(new Error(`Buffer ${bufnum} allocation timeout (${timeoutMs}ms)`));
            }, timeoutMs);

            this.pendingBufferOps.set(uuid, { resolve, reject, timeout });
        });
    }

    /**
     * Handle /buffer/freed message from WASM
     */
    _handleBufferFreed(args) {
        const bufnum = args[0];
        const freedPtr = args[1];

        const bufferInfo = this.allocatedBuffers.get(bufnum);

        if (!bufferInfo) {
            if (typeof freedPtr === 'number' && freedPtr !== 0) {
                this.bufferPool.free(freedPtr);
            }
            return;
        }

        if (typeof freedPtr === 'number' && freedPtr === bufferInfo.ptr) {
            this.bufferPool.free(bufferInfo.ptr);
            this.allocatedBuffers.delete(bufnum);
            return;
        }

        if (
            typeof freedPtr === 'number' &&
            bufferInfo.previousAllocation &&
            bufferInfo.previousAllocation.ptr === freedPtr
        ) {
            this.bufferPool.free(freedPtr);
            bufferInfo.previousAllocation = null;
            return;
        }

        // Fallback: free whichever pointer we're tracking and clear the entry
        this.bufferPool.free(bufferInfo.ptr);
        this.allocatedBuffers.delete(bufnum);
    }

    /**
     * Handle /buffer/allocated message with UUID correlation
     */
    _handleBufferAllocated(args) {
        const uuid = args[0];  // UUID string
        const bufnum = args[1]; // Buffer number

        // Find and resolve the pending operation
        const pending = this.pendingBufferOps.get(uuid);
        if (pending) {
            clearTimeout(pending.timeout);
            pending.resolve({ bufnum });
            this.pendingBufferOps.delete(uuid);
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

        this.stats.messagesSent++;

        if (this.onMessageSent) {
            this.onMessageSent(preparedData);
        }

        const timing = this.#calculateBundleWait(preparedData);
        const sendOptions = { ...options };

        if (timing) {
            sendOptions.audioTimeS = timing.audioTimeS;
            sendOptions.currentTimeS = timing.currentTimeS;
        }

        this.osc.send(preparedData, sendOptions);
    }

    /**
     * Get current status
     */
    getStatus() {
        return {
            initialized: this.initialized,
            capabilities: this.capabilities,
            stats: this.stats,
            audioContextState: this.audioContext?.state
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
        if (!this.config?.scsynth) {
            return null;
        }

        // Return a deep clone to prevent external mutation
        return {
            memory: { ...this.config.scsynth.memory },
            worldOptions: { ...this.config.scsynth.worldOptions }
        };
    }

    /**
     * Destroy the orchestrator and clean up resources
     */
    async destroy() {
        console.log('[SuperSonic] Destroying...');

        // Stop drift offset timer
        this.#stopDriftOffsetTimer();

        if (this.osc) {
            this.osc.terminate();
            this.osc = null;
        }

        if (this.workletNode) {
            this.workletNode.disconnect();
            this.workletNode = null;
        }

        if (this.audioContext) {
            await this.audioContext.close();
            this.audioContext = null;
        }

        // Cancel all pending buffer operations and their timeouts
        for (const [uuid, pending] of this.pendingBufferOps.entries()) {
            clearTimeout(pending.timeout);
            pending.reject(new Error('SuperSonic instance destroyed'));
        }
        this.pendingBufferOps.clear();

        this.sharedBuffer = null;
        this.initialized = false;
        this.bufferManager = null;
        this.allocatedBuffers.clear();
        this.loadedSynthDefs.clear();

        console.log('[SuperSonic] Destroyed');
    }

    /**
     * Wait until NTP timing has been established.
     * Note: NTP calculation is now done internally in C++ process_audio().
     * Returns 0 for backward compatibility.
     */
    async waitForTimeSync() {
        // Wait for buffer constants to be initialized (which includes NTP timing setup)
        if (!this.bufferConstants) {
            // Wait for the promise that was created in #initializeAudioContext()
            // and will be resolved when bufferConstants are initialized
            if (this._timeOffsetPromise) {
                await this._timeOffsetPromise;
            }
        }

        // Return the NTP start time for bundle creation
        // This is the NTP timestamp when AudioContext.currentTime was 0
        // Bundles should have timestamp = audioContextTime + ntpStartTime
        const ntpStartView = new Float64Array(this.sharedBuffer, this.ringBufferBase + this.bufferConstants.NTP_START_TIME_START, 1);
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
        if (!this.initialized) {
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

            console.log(`[SuperSonic] Sent synthdef from ${path} (${synthdefData.length} bytes)`);
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
        if (!this.initialized) {
            throw new Error('SuperSonic not initialized. Call init() first.');
        }

        if (!this.synthdefBaseURL) {
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
                    const path = `${this.synthdefBaseURL}${name}.scsyndef`;
                    await this.loadSynthDef(path);
                    results[name] = { success: true };
                } catch (error) {
                    console.error(`[SuperSonic] Failed to load ${name}:`, error);
                    results[name] = { success: false, error: error.message };
                }
            })
        );

        const successCount = Object.values(results).filter(r => r.success).length;
        console.log(`[SuperSonic] Sent ${successCount}/${names.length} synthdef loads`);

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
        if (!this.initialized) {
            throw new Error('SuperSonic not initialized. Call init() first.');
        }

        if (!Number.isInteger(syncId)) {
            throw new Error('sync() requires an integer syncId parameter');
        }

        const syncPromise = new Promise((resolve, reject) => {
            const timeout = setTimeout(() => {
                // Clean up listener on timeout
                if (this._syncListeners) {
                    this._syncListeners.delete(syncId);
                }
                reject(new Error('Timeout waiting for /synced response'));
            }, 10000); // 10 second timeout

            // Create a one-time message listener for this specific sync ID
            const messageHandler = (msg) => {
                clearTimeout(timeout);
                // Remove this specific listener
                this._syncListeners.delete(syncId);
                resolve();
            };

            // Store the listener in a map keyed by sync ID
            if (!this._syncListeners) {
                this._syncListeners = new Map();
            }
            this._syncListeners.set(syncId, messageHandler);
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
        if (!this.initialized) {
            throw new Error('SuperSonic not initialized. Call init() first.');
        }

        const sizeBytes = numSamples * 4;  // 4 bytes per Float32
        const addr = this.bufferPool.malloc(sizeBytes);

        if (addr === 0) {
            console.error(`[SuperSonic] Buffer allocation failed: ${numSamples} samples (${sizeBytes} bytes)`);
        }

        return addr;
    }

    /**
     * Free a previously allocated buffer
     * @param {number} addr - Buffer address returned by allocBuffer()
     * @returns {boolean} true if freed successfully
     * @example
     * sonic.freeBuffer(bufferAddr);
     */
    freeBuffer(addr) {
        if (!this.initialized) {
            throw new Error('SuperSonic not initialized. Call init() first.');
        }

        return this.bufferPool.free(addr);
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
        if (!this.initialized) {
            throw new Error('SuperSonic not initialized. Call init() first.');
        }

        return new Float32Array(this.sharedBuffer, addr, numSamples);
    }

    /**
     * Get buffer pool statistics
     * @returns {Object} Stats including total, available, used, etc.
     * @example
     * const stats = sonic.getBufferPoolStats();
     * console.log(`Available: ${stats.available} bytes`);
     */
    getBufferPoolStats() {
        if (!this.initialized) {
            throw new Error('SuperSonic not initialized. Call init() first.');
        }

        return this.bufferPool.stats();
    }

    getDiagnostics() {
        this.#ensureInitialized('get diagnostics');

        const poolStats = this.bufferPool?.stats ? this.bufferPool.stats() : null;
        let bytesActive = 0;
        let pendingCount = 0;

        for (const entry of this.allocatedBuffers.values()) {
            if (!entry) continue;
            bytesActive += entry.size || 0;
            if (entry.pendingToken) {
                pendingCount++;
            }
        }

        return {
            buffers: {
                active: this.allocatedBuffers.size,
                pending: pendingCount,
                bytesActive,
                pool: poolStats
                    ? {
                        total: poolStats.total || 0,
                        available: poolStats.available || 0,
                        freeBytes: poolStats.free?.size || 0,
                        freeBlocks: poolStats.free?.count || 0,
                        usedBytes: poolStats.used?.size || 0,
                        usedBlocks: poolStats.used?.count || 0
                    }
                    : null
            },
            synthdefs: {
                count: this.loadedSynthDefs.size
            }
        };
    }

    /**
     * Initialize NTP timing (write-once)
     * Sets the NTP start time when AudioContext started
     * @private
     */
    initializeNTPTiming() {
        if (!this.bufferConstants || !this.audioContext) {
            return;
        }

        // Calculate NTP time when AudioContext started (currentTime = 0)
        const perfTimeMs = performance.timeOrigin + performance.now();
        const currentNTP = (perfTimeMs / 1000) + NTP_EPOCH_OFFSET;
        const currentAudioCtx = this.audioContext.currentTime;

        // NTP time at AudioContext start = current NTP - current AudioContext time
        const ntpStartTime = currentNTP - currentAudioCtx;

        // Write to SharedArrayBuffer (write-once)
        const ntpStartView = new Float64Array(
            this.sharedBuffer,
            this.ringBufferBase + this.bufferConstants.NTP_START_TIME_START,
            1
        );
        ntpStartView[0] = ntpStartTime;

        // Store for drift calculation
        this._initialNTPStartTime = ntpStartTime;

        console.log(`[SuperSonic] NTP timing initialized: start=${ntpStartTime.toFixed(6)}s (current NTP=${currentNTP.toFixed(3)}, AudioCtx=${currentAudioCtx.toFixed(3)}), ringBufferBase=${this.ringBufferBase}`);
    }

    /**
     * Update drift offset (AudioContext → NTP drift correction)
     * CRITICAL: This REPLACES the drift value, does not accumulate
     * @private
     */
    updateDriftOffset() {
        if (!this.bufferConstants || !this.audioContext || this._initialNTPStartTime === undefined) {
            return;
        }

        // Calculate current NTP time from performance.now()
        const perfTimeMs = performance.timeOrigin + performance.now();
        const currentNTP = (perfTimeMs / 1000) + NTP_EPOCH_OFFSET;
        const currentAudioCtx = this.audioContext.currentTime;

        // Measure drift: current NTP start time vs initial baseline
        // CORRECT: This measures drift from initial baseline, not interval drift
        const currentNTPStartTime = currentNTP - currentAudioCtx;
        const driftSeconds = currentNTPStartTime - this._initialNTPStartTime;
        const driftMs = Math.round(driftSeconds * 1000);

        // Write to SharedArrayBuffer (REPLACE value, don't accumulate)
        const driftView = new Int32Array(
            this.sharedBuffer,
            this.ringBufferBase + this.bufferConstants.DRIFT_OFFSET_START,
            1
        );
        Atomics.store(driftView, 0, driftMs);

        console.log(`[SuperSonic] Drift offset updated: ${driftMs}ms (current NTP start=${currentNTPStartTime.toFixed(6)}, initial=${this._initialNTPStartTime.toFixed(6)})`);
    }

    /**
     * Get current drift offset in milliseconds
     * @returns {number} Current drift in milliseconds
     */
    getDriftOffset() {
        if (!this.bufferConstants) {
            return 0;
        }

        const driftView = new Int32Array(
            this.sharedBuffer,
            this.ringBufferBase + this.bufferConstants.DRIFT_OFFSET_START,
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
        this._driftOffsetTimer = setInterval(() => {
            this.updateDriftOffset();
        }, DRIFT_UPDATE_INTERVAL_MS);

        console.log(`[SuperSonic] Started drift offset correction (every ${DRIFT_UPDATE_INTERVAL_MS}ms)`);
    }

    /**
     * Stop periodic drift offset updates
     * @private
     */
    #stopDriftOffsetTimer() {
        if (this._driftOffsetTimer) {
            clearInterval(this._driftOffsetTimer);
            this._driftOffsetTimer = null;
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
        let sampleRate = this.audioContext?.sampleRate || 44100;

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
        if (!this.bufferManager) {
            throw new Error('Buffer manager not ready. Call init() before issuing buffer commands.');
        }
        return this.bufferManager;
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
        const ntpStartView = new Float64Array(this.sharedBuffer, this.ringBufferBase + this.bufferConstants.NTP_START_TIME_START, 1);
        const ntpStartTime = ntpStartView[0];

        if (ntpStartTime === 0) {
            console.warn('[SuperSonic] NTP start time not yet initialized');
            return null;
        }

        // Read current drift offset (milliseconds)
        const driftView = new Int32Array(this.sharedBuffer, this.ringBufferBase + this.bufferConstants.DRIFT_OFFSET_START, 1);
        const driftMs = Atomics.load(driftView, 0);
        const driftSeconds = driftMs / 1000.0;

        // Read global offset (milliseconds)
        const globalView = new Int32Array(this.sharedBuffer, this.ringBufferBase + this.bufferConstants.GLOBAL_OFFSET_START, 1);
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
        const currentTimeS = this.audioContext.currentTime;

        // Return the target audio time, not the wait time
        // The scheduler will handle lookahead scheduling
        return { audioTimeS, currentTimeS };
    }
}

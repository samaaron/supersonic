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

        // Pending buffer operations map for UUID correlation
        this.pendingBufferOps = new Map();  // UUID -> {resolve, reject, timeout}

        // Time offset (AudioContext → NTP conversion)
        this.wasmTimeOffset = null;
        this._timeOffsetPromise = null;
        this._resolveTimeOffset = null;

        // Callbacks
        this.onMessageReceived = null;
        this.onMessageSent = null;
        this.onMetricsUpdate = null;
        this.onStatusUpdate = null;
        this.onSendError = null;
        this.onDebugMessage = null;
        this.onInitialized = null;
        this.onError = null;

        // Configuration - resolve paths relative to this module
        const moduleUrl = new URL(import.meta.url);
        const basePath = new URL('.', moduleUrl).href;

        // Store basePath for use in methods
        this.basePath = basePath;

        this.config = {
            wasmUrl: new URL('wasm/scsynth-nrt.wasm', basePath).href,
            workletUrl: new URL('workers/scsynth_audio_worklet.js', basePath).href,
            development: false,
            audioContextOptions: {
                latencyHint: 'interactive',
                sampleRate: 48000
            }
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
     * Initialize shared WebAssembly memory
     */
    #initializeSharedMemory() {
        // Memory layout:
        // 0-32MB:     Emscripten heap (scsynth objects, stack)
        // 32-64MB:    Ring buffers (OSC in/out, debug, control)
        // 64-192MB:   Buffer pool (128MB for audio buffers)
        const TOTAL_PAGES = 3072;  // 3072 pages = 192MB

        this.wasmMemory = new WebAssembly.Memory({
            initial: TOTAL_PAGES,
            maximum: TOTAL_PAGES,
            shared: true
        });
        this.sharedBuffer = this.wasmMemory.buffer;

        // Initialize buffer pool (64MB offset, 128MB size)
        const BUFFER_POOL_OFFSET = 64 * 1024 * 1024;  // 64MB
        const BUFFER_POOL_SIZE = 128 * 1024 * 1024;   // 128MB

        this.bufferPool = new MemPool({
            buf: this.sharedBuffer,
            start: BUFFER_POOL_OFFSET,
            size: BUFFER_POOL_SIZE,
            align: 8  // 8-byte alignment (minimum required by MemPool)
        });

        console.log('[SuperSonic] Buffer pool initialized: 128MB at offset 64MB');
    }

    /**
     * Calculate time offset (AudioContext → NTP conversion)
     * Called when AudioContext is in 'running' state to ensure accurate timing
     */
    #calculateTimeOffset() {
        const SECONDS_1900_TO_1970 = 2208988800;
        const audioContextTime = this.audioContext.currentTime;
        const unixSeconds = Date.now() / 1000;
        this.wasmTimeOffset = (SECONDS_1900_TO_1970 + unixSeconds) - audioContextTime;

        // Resolve the promise if it hasn't been resolved yet
        if (this._resolveTimeOffset) {
            this._resolveTimeOffset(this.wasmTimeOffset);
            this._resolveTimeOffset = null;
        }

        return this.wasmTimeOffset;
    }

    /**
     * Initialize AudioContext and set up time offset calculation
     */
    #initializeAudioContext() {
        this.audioContext = new (window.AudioContext || window.webkitAudioContext)(
            this.config.audioContextOptions
        );

        // Create promise that will resolve when time offset is calculated
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

        // Listen for state changes to calculate offset when running
        this.audioContext.addEventListener('statechange', () => {
            if (this.audioContext.state === 'running' && this._resolveTimeOffset) {
                this.#calculateTimeOffset();
            }
        });

        // If already running, calculate immediately
        if (this.audioContext.state === 'running') {
            this.#calculateTimeOffset();
        }
    }

    /**
     * Load WASM manifest to get the current hashed filename
     */
    async #loadWasmManifest() {
        try {
            const manifestUrl = new URL('wasm/manifest.json', this.basePath).href;
            const response = await fetch(manifestUrl);
            if (response.ok) {
                const manifest = await response.json();

                // Use the WASM file specified in manifest
                const wasmFile = manifest.wasmFile;

                this.config.wasmUrl = new URL(`wasm/${wasmFile}`, this.basePath).href;
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

        // Wait for time offset to be calculated (when AudioContext is running)
        const timeOffset = await this._timeOffsetPromise;

        // Send WASM bytes, memory, and time offset
        this.workletNode.port.postMessage({
            type: 'loadWasm',
            wasmBytes: wasmBytes,
            wasmMemory: this.wasmMemory,
            timeOffset: timeOffset
        });

        // Wait for worklet initialization
        await this.#waitForWorkletInit();
    }

    /**
     * Initialize OSC communication layer
     */
    async #initializeOSC() {
        // Create ScsynthOSC instance
        this.osc = new ScsynthOSC();

        // Set up ScsynthOSC callbacks
        this.osc.onOSCMessage((msg) => {
            // Handle internal messages
            if (msg.address === '/buffer/freed') {
                this._handleBufferFreed(msg.args);
            } else if (msg.address === '/buffer/allocated') {
                // Handle buffer allocation completion with UUID correlation
                this._handleBufferAllocated(msg.args);
            }

            // Always forward to onMessageReceived (including internal messages)
            if (this.onMessageReceived) {
                this.stats.messagesReceived++;
                this.onMessageReceived(msg);
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
                            this.bufferConstants = event.data.bufferConstants;
                        } else {
                            console.warn('[SuperSonic] Warning: bufferConstants not provided by worklet');
                        }

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
        if (!this.initialized) {
            throw new Error('SuperSonic not initialized. Call init() first.');
        }

        // Intercept buffer allocation commands BEFORE OSC encoding
        if (this._isBufferAllocationCommand(address)) {
            return await this._handleBufferCommand(address, args);
        }

        // Auto-detect types for normal commands
        const oscArgs = args.map(arg => {
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

        const message = {
            address: address,
            args: oscArgs
        };

        // Encode the message to OSC bytes
        const oscData = oscLib.writePacket(message);

        // Use sendOSC to send the encoded bytes
        this.sendOSC(oscData);
    }

    _isBufferAllocationCommand(address) {
        return [
            '/b_allocRead',
            '/b_allocReadChannel',
            '/b_read',
            '/b_readChannel'
            // NOTE: /b_alloc and /b_free are NOT intercepted
        ].includes(address);
    }

    async _handleBufferCommand(address, args) {
        switch (address) {
            case '/b_allocRead':
                return await this._allocReadBuffer(...args);
            case '/b_allocReadChannel':
                return await this._allocReadChannelBuffer(...args);
            case '/b_read':
                return await this._readBuffer(...args);
            case '/b_readChannel':
                return await this._readChannelBuffer(...args);
        }
    }

    /**
     * /b_allocRead bufnum path [startFrame numFrames completion]
     */
    async _allocReadBuffer(bufnum, path, startFrame = 0, numFrames = 0, completionMsg = null) {
        let allocatedPtr = null;
        const GUARD_BEFORE = 3;
        const GUARD_AFTER = 1;

        try {
            // 1. Resolve path to URL
            const url = this._resolveAudioPath(path);

            // 2. Fetch audio file
            const response = await fetch(url);
            if (!response.ok) {
                throw new Error(`HTTP ${response.status}: ${response.statusText}`);
            }
            const arrayBuffer = await response.arrayBuffer();

            // 3. Decode audio
            const audioBuffer = await this.audioContext.decodeAudioData(arrayBuffer);

            // 4. Extract frame range
            const actualStartFrame = startFrame || 0;
            const actualNumFrames = numFrames || (audioBuffer.length - actualStartFrame);
            const framesToRead = Math.min(actualNumFrames, audioBuffer.length - actualStartFrame);

            if (framesToRead <= 0) {
                throw new Error(`Invalid frame range: start=${actualStartFrame}, numFrames=${actualNumFrames}, fileLength=${audioBuffer.length}`);
            }

            // 5. Interleave channels (SuperCollider expects interleaved format)
            const numChannels = audioBuffer.numberOfChannels;
            const guardSamples = (GUARD_BEFORE + GUARD_AFTER) * numChannels;
            const interleavedData = new Float32Array((framesToRead * numChannels) + guardSamples);

            // Write actual audio data (offset by GUARD_BEFORE samples)
            const dataOffset = GUARD_BEFORE * numChannels;
            for (let frame = 0; frame < framesToRead; frame++) {
                for (let ch = 0; ch < numChannels; ch++) {
                    const channelData = audioBuffer.getChannelData(ch);
                    interleavedData[dataOffset + (frame * numChannels) + ch] =
                        channelData[actualStartFrame + frame];
                }
            }

            // Guard samples are left as zeros (silence)
            // Front guards: indices 0 to (GUARD_BEFORE * numChannels - 1)
            // Back guards: indices (dataOffset + framesToRead * numChannels) to end

            // 6. Allocate buffer memory
            const bytesNeeded = interleavedData.length * 4;  // Float32 = 4 bytes
            allocatedPtr = this.bufferPool.malloc(bytesNeeded);

            if (allocatedPtr === 0) {
                throw new Error('Buffer pool allocation failed (out of memory)');
            }

            // 7. Write audio data to SharedArrayBuffer
            const wasmHeap = new Float32Array(
                this.sharedBuffer,
                allocatedPtr,
                interleavedData.length
            );
            wasmHeap.set(interleavedData);

            // 8. Track allocation for cleanup
            this.allocatedBuffers.set(bufnum, {
                ptr: allocatedPtr,
                size: bytesNeeded
            });

            // 9. Generate UUID for correlation
            const uuid = crypto.randomUUID();

            // 10. Set up promise to wait for allocation confirmation
            const allocationComplete = new Promise((resolve, reject) => {
                const timeout = setTimeout(() => {
                    this.pendingBufferOps.delete(uuid);
                    reject(new Error(`Timeout waiting for buffer ${bufnum} allocation`));
                }, 5000);
                this.pendingBufferOps.set(uuid, { resolve, reject, timeout });
            });

            // 11. Send pointer command to WASM with UUID
            await this.send('/b_allocPtr', bufnum, allocatedPtr, framesToRead,
                           numChannels, audioBuffer.sampleRate, uuid);

            // 12. Wait for allocation confirmation
            await allocationComplete;

            // 13. Handle completion message if provided
            if (completionMsg) {
                // TODO: Handle completion message
            }

        } catch (error) {
            // Clean up allocated memory on error
            if (allocatedPtr) {
                this.bufferPool.free(allocatedPtr);
                this.allocatedBuffers.delete(bufnum);
            }

            console.error(`[SuperSonic] Buffer ${bufnum} load failed:`, error);
            throw error;
        }
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
                'Example: new SuperSonic({ sampleBaseURL: "https://unpkg.com/supersonic-scsynth-samples@latest/samples/" })\n' +
                'Or install sample packages: npm install supersonic-scsynth-samples'
            );
        }

        // Otherwise prepend base URL
        return this.sampleBaseURL + scPath;
    }

    /**
     * Handle /buffer/freed message from WASM
     */
    _handleBufferFreed(args) {
        const bufnum = args[0];
        const offset = args[1];

        const bufferInfo = this.allocatedBuffers.get(bufnum);
        if (bufferInfo) {
            this.bufferPool.free(bufferInfo.ptr);
            this.allocatedBuffers.delete(bufnum);
        }
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
     * /b_allocReadChannel bufnum path [startFrame numFrames channel1 channel2 ... completion]
     * Load specific channels from an audio file
     */
    async _allocReadChannelBuffer(bufnum, path, startFrame = 0, numFrames = 0, ...channelsAndCompletion) {
        let allocatedPtr = null;
        const GUARD_BEFORE = 3;
        const GUARD_AFTER = 1;

        try {
            // Parse channel numbers (all numeric args until we hit a non-number or end)
            const channels = [];
            let completionMsg = null;
            for (let i = 0; i < channelsAndCompletion.length; i++) {
                if (typeof channelsAndCompletion[i] === 'number' && Number.isInteger(channelsAndCompletion[i])) {
                    channels.push(channelsAndCompletion[i]);
                } else {
                    completionMsg = channelsAndCompletion[i];
                    break;
                }
            }

            // 1. Resolve path and fetch
            const url = this._resolveAudioPath(path);
            const response = await fetch(url);
            if (!response.ok) {
                throw new Error(`HTTP ${response.status}: ${response.statusText}`);
            }
            const arrayBuffer = await response.arrayBuffer();

            // 2. Decode audio
            const audioBuffer = await this.audioContext.decodeAudioData(arrayBuffer);

            // 3. Extract frame range
            const actualStartFrame = startFrame || 0;
            const actualNumFrames = numFrames || (audioBuffer.length - actualStartFrame);
            const framesToRead = Math.min(actualNumFrames, audioBuffer.length - actualStartFrame);

            if (framesToRead <= 0) {
                throw new Error(`Invalid frame range: start=${actualStartFrame}, numFrames=${actualNumFrames}, fileLength=${audioBuffer.length}`);
            }

            // 4. Validate and default channels
            const fileChannels = audioBuffer.numberOfChannels;
            const selectedChannels = channels.length > 0 ? channels : Array.from({length: fileChannels}, (_, i) => i);

            // Validate channel indices
            for (const ch of selectedChannels) {
                if (ch < 0 || ch >= fileChannels) {
                    throw new Error(`Invalid channel ${ch} (file has ${fileChannels} channels)`);
                }
            }

            const numChannels = selectedChannels.length;

            // 5. Interleave selected channels
            const guardSamples = (GUARD_BEFORE + GUARD_AFTER) * numChannels;
            const interleavedData = new Float32Array((framesToRead * numChannels) + guardSamples);
            const dataOffset = GUARD_BEFORE * numChannels;

            for (let frame = 0; frame < framesToRead; frame++) {
                for (let ch = 0; ch < numChannels; ch++) {
                    const fileChannel = selectedChannels[ch];
                    const channelData = audioBuffer.getChannelData(fileChannel);
                    interleavedData[dataOffset + (frame * numChannels) + ch] =
                        channelData[actualStartFrame + frame];
                }
            }

            // 6. Allocate and write
            const bytesNeeded = interleavedData.length * 4;
            allocatedPtr = this.bufferPool.malloc(bytesNeeded);
            if (allocatedPtr === 0) {
                throw new Error('Buffer pool allocation failed (out of memory)');
            }

            const wasmHeap = new Float32Array(this.sharedBuffer, allocatedPtr, interleavedData.length);
            wasmHeap.set(interleavedData);

            // 7. Track and send
            this.allocatedBuffers.set(bufnum, { ptr: allocatedPtr, size: bytesNeeded });
            await this.send('/b_allocPtr', bufnum, allocatedPtr, framesToRead, numChannels, audioBuffer.sampleRate);

            if (completionMsg) {
                // TODO: Handle completion message
            }

        } catch (error) {
            if (allocatedPtr) {
                this.bufferPool.free(allocatedPtr);
                this.allocatedBuffers.delete(bufnum);
            }
            console.error(`[SuperSonic] Buffer ${bufnum} load failed:`, error);
            throw error;
        }
    }

    /**
     * /b_read bufnum path [startFrame numFrames bufStartFrame leaveOpen completion]
     * Read file into existing buffer
     */
    async _readBuffer(bufnum, path, startFrame = 0, numFrames = 0, bufStartFrame = 0, leaveOpen = 0, completionMsg = null) {
        console.warn('[SuperSonic] /b_read requires pre-allocated buffer - not yet implemented');
        throw new Error('/b_read not yet implemented (requires /b_alloc first)');
    }

    /**
     * /b_readChannel bufnum path [startFrame numFrames bufStartFrame leaveOpen channel1 channel2 ... completion]
     * Read specific channels into existing buffer
     */
    async _readChannelBuffer(bufnum, path, startFrame = 0, numFrames = 0, bufStartFrame = 0, leaveOpen = 0, ...channelsAndCompletion) {
        console.warn('[SuperSonic] /b_readChannel requires pre-allocated buffer - not yet implemented');
        throw new Error('/b_readChannel not yet implemented (requires /b_alloc first)');
    }

    /**
     * Send pre-encoded OSC bytes to scsynth
     * @param {ArrayBuffer|Uint8Array} oscData - Pre-encoded OSC data
     * @param {Object} options - Send options
     */
    sendOSC(oscData, options = {}) {
        if (!this.initialized) {
            throw new Error('Not initialized. Call init() first.');
        }

        // Convert ArrayBuffer to Uint8Array if needed
        let uint8Data;
        if (oscData instanceof ArrayBuffer) {
            uint8Data = new Uint8Array(oscData);
        } else if (oscData instanceof Uint8Array) {
            uint8Data = oscData;
        } else {
            throw new Error('oscData must be ArrayBuffer or Uint8Array');
        }

        this.stats.messagesSent++;

        // Notify callback before sending
        if (this.onMessageSent) {
            this.onMessageSent(uint8Data);
        }

        // Calculate wait time for bundles
        let waitTimeMs = null;

        // Check if this is a bundle (starts with "#bundle\0")
        if (uint8Data.length >= 16) {
            const header = String.fromCharCode.apply(null, uint8Data.slice(0, 8));
            if (header === '#bundle\0') {
                // Ensure time offset is calculated (fallback if statechange didn't fire)
                if (this.wasmTimeOffset === null) {
                    console.warn('[SuperSonic] Time offset not yet calculated, calculating now');
                    this.#calculateTimeOffset();
                }

                // Extract NTP timetag (8 bytes at offset 8, big-endian)
                const view = new DataView(uint8Data.buffer, uint8Data.byteOffset);
                const ntpSeconds = view.getUint32(8, false);
                const ntpFraction = view.getUint32(12, false);

                // Check for immediate execution (timetag == 0 or 1)
                if (!(ntpSeconds === 0 && (ntpFraction === 0 || ntpFraction === 1))) {
                    // Convert NTP to seconds
                    const ntpTimeS = ntpSeconds + (ntpFraction / 0x100000000);

                    // Convert NTP to AudioContext time using WASM offset
                    const audioTimeS = ntpTimeS - this.wasmTimeOffset;

                    // Calculate wait time (target - current - 50ms latency compensation)
                    const currentAudioTimeS = this.audioContext.currentTime;
                    const latencyS = 0.050; // 50ms latency compensation
                    waitTimeMs = (audioTimeS - currentAudioTimeS - latencyS) * 1000;

                    // Debug bundle timing (commented out - enable for scheduler debugging)
                    // console.log('[SuperSonic] Bundle timing - NTP:', ntpTimeS.toFixed(3),
                    //            'AudioTime:', audioTimeS.toFixed(3),
                    //            'Current:', currentAudioTimeS.toFixed(3),
                    //            'Wait:', waitTimeMs.toFixed(2), 'ms');
                }
            }
        }

        // Use ScsynthOSC's send method with calculated wait time
        this.osc.send(uint8Data, { ...options, waitTimeMs });
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
     * Destroy the orchestrator and clean up resources
     */
    async destroy() {
        console.log('[SuperSonic] Destroying...');

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

        this.sharedBuffer = null;
        this.initialized = false;

        console.log('[SuperSonic] Destroyed');
    }

    /**
     * Load a sample into a buffer and wait for confirmation
     * @param {number} bufnum - Buffer number
     * @param {string} path - Audio file path
     * @returns {Promise} Resolves when buffer is ready
     */
    async loadSample(bufnum, path, startFrame = 0, numFrames = 0) {
        if (!this.initialized) {
            throw new Error('SuperSonic not initialized. Call init() first.');
        }

        // Use the internal _allocReadBuffer which handles everything including UUID correlation
        await this._allocReadBuffer(bufnum, path, startFrame, numFrames);
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

            // Send via /d_recv OSC message
            this.send('/d_recv', synthdefData);

            console.log(`[SuperSonic] Loaded synthdef from ${path} (${synthdefData.length} bytes)`);
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
                'Example: new SuperSonic({ synthdefBaseURL: "https://unpkg.com/supersonic-scsynth-synthdefs@latest/synthdefs/" })\n' +
                'Or install: npm install supersonic-scsynth-synthdefs'
            );
        }

        const results = {};

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
        console.log(`[SuperSonic] Loaded ${successCount}/${names.length} synthdefs`);

        return results;
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
}
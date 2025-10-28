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

export class SuperSonic {
    // Expose OSC utilities as static methods
    static osc = {
        encode: (message) => oscLib.writePacket(message),
        decode: (data, options = { metadata: false }) => oscLib.readPacket(data, options)
    };
    constructor() {
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

        // Configuration
        this.config = {
            wasmUrl: './dist/wasm/scsynth-nrt.wasm',
            workletUrl: './dist/workers/scsynth_audio_worklet.js',
            audioContextOptions: {
                latencyHint: 'interactive',
                sampleRate: 48000
            }
        };

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
        this.wasmMemory = new WebAssembly.Memory({
            initial: 512,  // 512 pages = 32MB (for scsynth + ring buffers)
            maximum: 512,
            shared: true
        });
        this.sharedBuffer = this.wasmMemory.buffer;
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
     * Load WASM binary from network
     */
    async #loadWasm() {
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
     */
    async init() {
        if (this.initialized) {
            console.warn('[SuperSonic] Already initialized');
            return;
        }

        if (this.initializing) {
            console.warn('[SuperSonic] Initialization already in progress');
            return;
        }

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
    send(address, ...args) {
        if (!this.initialized) {
            throw new Error('SuperSonic not initialized. Call init() first.');
        }

        // Auto-detect types
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
     * Load a binary synthdef file and send it to scsynth
     * @param {string} path - Path or URL to the .scsyndef file
     * @returns {Promise<void>}
     * @example
     * await sonic.loadSynthDef('./etc/synthdefs/sonic-pi-beep.scsyndef');
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
     * @param {string} baseUrl - Base URL for synthdef files (default: './etc/synthdefs/')
     * @returns {Promise<Object>} Map of name -> success/error
     * @example
     * const results = await sonic.loadSynthDefs(['sonic-pi-beep', 'sonic-pi-tb303']);
     */
    async loadSynthDefs(names, baseUrl = './etc/synthdefs/') {
        if (!this.initialized) {
            throw new Error('SuperSonic not initialized. Call init() first.');
        }

        const results = {};

        await Promise.all(
            names.map(async (name) => {
                try {
                    const path = `${baseUrl}${name}.scsyndef`;
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
}
// SPDX-License-Identifier: MIT OR GPL-3.0-or-later
// Copyright (c) 2025 Sam Aaron

/**
 * SuperSonic - WebAssembly SuperCollider synthesis engine
 * Coordinates SharedArrayBuffer, WASM, AudioWorklet, and IO Workers
 */

import { createTransport } from "./lib/transport/index.js";
import { BufferManager } from "./lib/buffer_manager.js";
import { AssetLoader } from "./lib/asset_loader.js";
import { OSCRewriter } from "./lib/osc_rewriter.js";
import { DirectWriter } from "./lib/direct_writer.js";
import { extractSynthDefName } from "./lib/synthdef_parser.js";
import { EventEmitter } from "./lib/event_emitter.js";
import { MetricsReader } from "./lib/metrics_reader.js";
import { NTPTiming } from "./lib/ntp_timing.js";
import { AudioCapture } from "./lib/audio_capture.js";
import { inspect, parseNodeTree } from "./lib/inspector.js";
import oscLib from "./vendor/osc.js/osc.js";
import { SYNC_TIMEOUT_MS, WORKLET_INIT_TIMEOUT_MS } from "./timing_constants.js";
import { MemoryLayout } from "./memory_layout.js";
import { defaultWorldOptions } from "./scsynth_options.js";
import { addWorkletModule } from "./lib/worker_loader.js";

// Derive default base URL from module location
const MODULE_URL = import.meta.url;

/**
 * Parse the module URL to extract base paths for CDN usage
 */
function deriveDefaultURLs() {
  try {
    const baseURL = MODULE_URL.substring(0, MODULE_URL.lastIndexOf('/') + 1);
    const unpkgMatch = MODULE_URL.match(/^(https:\/\/unpkg\.com\/)supersonic-scsynth@([^/]+)\/dist\/supersonic\.js$/);

    if (unpkgMatch) {
      const cdnBase = unpkgMatch[1];
      const version = unpkgMatch[2];
      return {
        baseURL,
        coreBaseURL: `${cdnBase}supersonic-scsynth-core@${version}/`,
        sampleBaseURL: `${cdnBase}supersonic-scsynth-samples@${version}/samples/`,
        synthdefBaseURL: `${cdnBase}supersonic-scsynth-synthdefs@${version}/synthdefs/`,
      };
    }

    return { baseURL, coreBaseURL: null, sampleBaseURL: null, synthdefBaseURL: null };
  } catch {
    return { baseURL: null, coreBaseURL: null, sampleBaseURL: null, synthdefBaseURL: null };
  }
}

const DEFAULT_URLS = deriveDefaultURLs();

/**
 * @typedef {import('./lib/metrics_types.js').SuperSonicMetrics} SuperSonicMetrics
 */

export class SuperSonic {
  // Expose OSC utilities as static methods
  static osc = {
    encode: (message) => oscLib.writePacket(message),
    decode: (data, options = { metadata: false }) => oscLib.readPacket(data, options),
  };

  /**
   * Inspect a SuperSonic instance or raw SharedArrayBuffer
   */
  static inspect(target) {
    return inspect(target);
  }

  /**
   * Get schema describing all available metrics.
   * Useful for generating UIs or understanding metric types.
   */
  static getMetricsSchema() {
    return {
      // Meta
      mode: { type: 'string', values: ['sab', 'postMessage'], description: 'Transport mode' },

      // scsynth metrics
      scsynthProcessCount: { type: 'counter', unit: 'count', description: 'Audio process() calls' },
      scsynthMessagesProcessed: { type: 'counter', unit: 'count', description: 'OSC messages processed by scsynth' },
      scsynthMessagesDropped: { type: 'counter', unit: 'count', description: 'Messages dropped (ring buffer full)' },
      scsynthSchedulerDepth: { type: 'gauge', unit: 'count', description: 'Current scheduler queue depth' },
      scsynthSchedulerPeakDepth: { type: 'gauge', unit: 'count', description: 'Peak scheduler queue depth (high water mark)' },
      scsynthSchedulerCapacity: { type: 'constant', unit: 'count', description: 'Maximum scheduler queue size' },
      scsynthSchedulerDropped: { type: 'counter', unit: 'count', description: 'Scheduled events dropped' },
      scsynthSequenceGaps: { type: 'counter', unit: 'count', description: 'Messages lost in transit from JS to scsynth' },
      scsynthSchedulerLates: { type: 'counter', unit: 'count', description: 'Bundles executed after their scheduled time' },

      // Prescheduler metrics
      preschedulerPending: { type: 'gauge', unit: 'count', description: 'Events waiting to be scheduled' },
      preschedulerPendingPeak: { type: 'gauge', unit: 'count', description: 'Peak pending events' },
      preschedulerDispatched: { type: 'counter', unit: 'count', description: 'Events sent to worklet' },
      preschedulerRetriesSucceeded: { type: 'counter', unit: 'count', description: 'Retries that succeeded' },
      preschedulerRetriesFailed: { type: 'counter', unit: 'count', description: 'Retries that failed' },
      preschedulerBundlesScheduled: { type: 'counter', unit: 'count', description: 'Bundles scheduled' },
      preschedulerEventsCancelled: { type: 'counter', unit: 'count', description: 'Events cancelled' },
      preschedulerTotalDispatches: { type: 'counter', unit: 'count', description: 'Total dispatch attempts' },
      preschedulerMessagesRetried: { type: 'counter', unit: 'count', description: 'Messages that needed retry' },
      preschedulerRetryQueueSize: { type: 'gauge', unit: 'count', description: 'Current retry queue size' },
      preschedulerRetryQueuePeak: { type: 'gauge', unit: 'count', description: 'Peak retry queue size' },
      preschedulerBypassed: { type: 'counter', unit: 'count', description: 'Messages sent directly from JS to scsynth, bypassing prescheduler' },
      preschedulerCapacity: { type: 'constant', unit: 'count', description: 'Maximum pending events in prescheduler' },
      preschedulerMinHeadroomMs: { type: 'gauge', unit: 'ms', description: 'Smallest time gap between JS prescheduler dispatch and scsynth scheduler execution' },
      preschedulerLates: { type: 'counter', unit: 'count', description: 'Bundles dispatched after their scheduled execution time' },

      // OSC In metrics
      oscInMessagesReceived: { type: 'counter', unit: 'count', description: 'OSC replies received from scsynth to JS' },
      oscInMessagesDropped: { type: 'counter', unit: 'count', description: 'Replies lost in transit from scsynth to JS' },
      oscInBytesReceived: { type: 'counter', unit: 'bytes', description: 'Total bytes received from scsynth to JS' },

      // Debug metrics
      debugMessagesReceived: { type: 'counter', unit: 'count', description: 'Debug messages from scsynth' },
      debugBytesReceived: { type: 'counter', unit: 'bytes', description: 'Debug bytes received' },

      // Main thread metrics
      oscOutMessagesSent: { type: 'counter', unit: 'count', description: 'OSC messages sent from JS to scsynth' },
      oscOutBytesSent: { type: 'counter', unit: 'bytes', description: 'Total bytes sent from JS to scsynth' },

      // Buffer usage
      inBufferUsed: { type: 'object', description: 'Input ring buffer usage', properties: {
        bytes: { type: 'gauge', unit: 'bytes', description: 'Bytes used' },
        percentage: { type: 'gauge', unit: 'percentage', description: 'Percentage full' },
        capacity: { type: 'constant', unit: 'bytes', description: 'Total buffer capacity' }
      }},
      outBufferUsed: { type: 'object', description: 'Output ring buffer usage', properties: {
        bytes: { type: 'gauge', unit: 'bytes', description: 'Bytes used' },
        percentage: { type: 'gauge', unit: 'percentage', description: 'Percentage full' },
        capacity: { type: 'constant', unit: 'bytes', description: 'Total buffer capacity' }
      }},
      debugBufferUsed: { type: 'object', description: 'Debug ring buffer usage', properties: {
        bytes: { type: 'gauge', unit: 'bytes', description: 'Bytes used' },
        percentage: { type: 'gauge', unit: 'percentage', description: 'Percentage full' },
        capacity: { type: 'constant', unit: 'bytes', description: 'Total buffer capacity' }
      }},

      // Timing
      driftOffsetMs: { type: 'gauge', unit: 'ms', description: 'Clock drift between AudioContext and wall clock' },

      // Engine state
      audioContextState: { type: 'string', values: ['running', 'suspended', 'closed', 'interrupted'], description: 'AudioContext state' },
      bufferPoolUsedBytes: { type: 'gauge', unit: 'bytes', description: 'Buffer pool bytes used' },
      bufferPoolAvailableBytes: { type: 'gauge', unit: 'bytes', description: 'Buffer pool bytes available' },
      bufferPoolAllocations: { type: 'counter', unit: 'count', description: 'Total buffer allocations' },
      loadedSynthDefs: { type: 'gauge', unit: 'count', description: 'Number of loaded synthdefs' },

      // Error metrics
      scsynthWasmErrors: { type: 'counter', unit: 'count', description: 'WASM execution errors in audio worklet' },
      oscInCorrupted: { type: 'counter', unit: 'count', description: 'Corrupted messages detected from scsynth to JS' },
    };
  }

  /**
   * Get schema describing the node tree structure.
   * Useful for generating UIs or understanding tree data.
   */
  static getTreeSchema() {
    const nodeSchema = {
      id: { type: 'number', description: 'Unique node ID' },
      type: { type: 'string', values: ['group', 'synth'], description: 'Node type' },
      defName: { type: 'string', description: 'Synthdef name (synths only, empty for groups)' },
      children: { type: 'array', description: 'Child nodes (recursive)', itemSchema: '(self)' }
    };
    return {
      nodeCount: { type: 'number', description: 'Total nodes in tree' },
      version: { type: 'number', description: 'Increments on any tree change, useful for detecting updates' },
      droppedCount: { type: 'number', description: 'Nodes that exceeded mirror capacity (tree may be incomplete)' },
      root: {
        type: 'object',
        description: 'Root node of the tree (always a group with id 0)',
        schema: nodeSchema
      }
    };
  }

  static getRawTreeSchema() {
    return {
      nodeCount: { type: 'number', description: 'Total nodes in tree' },
      version: { type: 'number', description: 'Increments on any tree change, useful for detecting updates' },
      droppedCount: { type: 'number', description: 'Nodes that exceeded mirror capacity (tree may be incomplete)' },
      nodes: {
        type: 'array',
        description: 'Flat array of all nodes with internal linkage pointers',
        itemSchema: {
          id: { type: 'number', description: 'Unique node ID' },
          parentId: { type: 'number', description: 'Parent node ID (-1 for root)' },
          isGroup: { type: 'boolean', description: 'True if group, false if synth' },
          prevId: { type: 'number', description: 'Previous sibling node ID (-1 if none)' },
          nextId: { type: 'number', description: 'Next sibling node ID (-1 if none)' },
          headId: { type: 'number', description: 'First child node ID (groups only, -1 if empty)' },
          defName: { type: 'string', description: 'Synthdef name (synths only, empty for groups)' }
        }
      }
    };
  }

  // Private implementation
  #audioContext;
  #workletNode;
  #node = null;
  #osc;
  #wasmMemory;
  #bufferManager;
  #oscRewriter;
  #syncListeners;
  #sampleBaseURL;
  #synthdefBaseURL;
  #fetchRetryConfig;
  #assetLoader;
  #initialized;
  #initializing;
  #initPromise;
  #capabilities;
  #version;
  #config;

  // Extracted modules
  #eventEmitter;
  #metricsReader;
  #ntpTiming;
  #audioCapture;

  // Direct ring buffer write (bypasses worker for low-latency non-bundle messages)
  #directWriter;

  // Track AudioContext state for recovery detection
  #previousAudioContextState = null;

  // Cached WASM bytes for fast recover()
  #cachedWasmBytes = null;

  // Snapshot tracking (postMessage mode)
  #snapshotsSent = 0;

  // Buffer for early debugRawBatch messages
  #earlyDebugMessages = [];
  #debugRawHandler = null;

  constructor(options = {}) {
    this.#initialized = false;
    this.#initializing = false;
    this.#initPromise = null;
    this.#capabilities = {};
    this.#version = null;

    // Initialize extracted modules
    this.#eventEmitter = new EventEmitter();
    this.#metricsReader = new MetricsReader({ mode: options.mode || 'postMessage' });
    this.#audioCapture = new AudioCapture({});

    // Core components
    this.#audioContext = null;
    this.#workletNode = null;
    this.#osc = null;
    this.#bufferManager = null;
    this.loadedSynthDefs = new Map();

    // Configuration
    // coreBaseURL is for WASM and workers (from supersonic-scsynth-core package)
    // baseURL is for backwards compatibility and local development
    const baseURL = options.baseURL || DEFAULT_URLS.baseURL;
    const coreBaseURL = options.coreBaseURL || DEFAULT_URLS.coreBaseURL || baseURL;
    const workerBaseURL = options.workerBaseURL || (coreBaseURL ? `${coreBaseURL}workers/` : null);
    const wasmBaseURL = options.wasmBaseURL || (coreBaseURL ? `${coreBaseURL}wasm/` : null);

    if (!workerBaseURL || !wasmBaseURL) {
      throw new Error(
        "SuperSonic requires coreBaseURL, baseURL, or explicit workerBaseURL and wasmBaseURL options."
      );
    }

    const worldOptions = { ...defaultWorldOptions, ...options.scsynthOptions };
    const mode = options.mode || 'postMessage';

    this.#config = {
      mode: mode,
      snapshotIntervalMs: options.snapshotIntervalMs ?? 50,
      wasmUrl: options.wasmUrl || wasmBaseURL + "scsynth-nrt.wasm",
      wasmBaseURL: wasmBaseURL,
      workletUrl: options.workletUrl || workerBaseURL + "scsynth_audio_worklet.js",
      workerBaseURL: workerBaseURL,
      audioContext: options.audioContext || null,
      autoConnect: options.autoConnect !== false,
      audioContextOptions: {
        latencyHint: "interactive",
        sampleRate: 48000,
      },
      memory: MemoryLayout,
      worldOptions: worldOptions,
      preschedulerCapacity: options.preschedulerCapacity || 65536,
      activityEvent: {
        maxLineLength: options.activityEvent?.maxLineLength ?? 200,
        scsynthMaxLineLength: options.activityEvent?.scsynthMaxLineLength ?? null,
        oscInMaxLineLength: options.activityEvent?.oscInMaxLineLength ?? null,
        oscOutMaxLineLength: options.activityEvent?.oscOutMaxLineLength ?? null,
      },
      debug: options.debug ?? false,
      debugScsynth: options.debugScsynth ?? false,
      debugOscIn: options.debugOscIn ?? false,
      debugOscOut: options.debugOscOut ?? false,
      activityConsoleLog: {
        maxLineLength: options.activityConsoleLog?.maxLineLength ?? 200,
        scsynthMaxLineLength: options.activityConsoleLog?.scsynthMaxLineLength ?? null,
        oscInMaxLineLength: options.activityConsoleLog?.oscInMaxLineLength ?? null,
        oscOutMaxLineLength: options.activityConsoleLog?.oscOutMaxLineLength ?? null,
      },
    };

    this.#sampleBaseURL = options.sampleBaseURL || DEFAULT_URLS.sampleBaseURL || (baseURL ? `${baseURL}samples/` : null);
    this.#synthdefBaseURL = options.synthdefBaseURL || DEFAULT_URLS.synthdefBaseURL || (baseURL ? `${baseURL}synthdefs/` : null);

    this.#fetchRetryConfig = {
      maxRetries: options.fetchMaxRetries ?? 3,
      baseDelay: options.fetchRetryDelay ?? 1000,
    };

    this.#assetLoader = new AssetLoader({
      onLoadingEvent: (event, data) => this.#eventEmitter.emit(event, data),
      maxRetries: this.#fetchRetryConfig.maxRetries,
      baseDelay: this.#fetchRetryConfig.baseDelay,
    });

    this.bootStats = {
      initStartTime: null,
      initDuration: null,
    };
  }

  // ============================================================================
  // PUBLIC GETTERS
  // ============================================================================

  get initialized() { return this.#initialized; }
  get initializing() { return this.#initializing; }
  get mode() { return this.#config.mode; }
  get bufferConstants() { return this.#metricsReader.bufferConstants; }
  get ringBufferBase() { return this.#metricsReader.ringBufferBase; }
  get sharedBuffer() { return this.#metricsReader.sharedBuffer; }
  get node() { return this.#node; }
  get osc() { return this.#osc; }

  // ============================================================================
  // EVENT EMITTER DELEGATION
  // ============================================================================

  on(event, callback) { return this.#eventEmitter.on(event, callback); }
  off(event, callback) { this.#eventEmitter.off(event, callback); return this; }
  once(event, callback) { this.#eventEmitter.once(event, callback); return this; }
  removeAllListeners(event) { this.#eventEmitter.removeAllListeners(event); return this; }

  // ============================================================================
  // INITIALIZATION
  // ============================================================================

  async init() {
    if (this.#initialized) return;
    if (this.#initPromise) return this.#initPromise;

    this.#initPromise = this.#doInit();
    return this.#initPromise;
  }

  async #doInit() {
    this.#initializing = true;
    this.bootStats.initStartTime = performance.now();

    try {
      this.#setAndValidateCapabilities();
      this.#initializeMemory();
      this.#initializeAudioContext();
      this.#initializeBufferManager();
      this.#initializeOSCRewriter();
      const wasmBytes = await this.#loadWasm();
      await this.#initializeAudioWorklet(wasmBytes);
      await this.#initializeOSC();
      await this.#finishInitialization();
    } catch (error) {
      this.#initializing = false;
      this.#initPromise = null;
      console.error("[SuperSonic] Initialization failed:", error);
      this.#eventEmitter.emit('error', error);
      throw error;
    }
  }

  // ============================================================================
  // METRICS API
  // ============================================================================

  getMetrics() {
    return this.#gatherMetrics();
  }

  // ============================================================================
  // RECOVERY API
  // ============================================================================

  /**
   * Smart recovery - tries quick resume first, falls back to full reload.
   * Use this when you don't know if a quick resume will work.
   * @returns {Promise<boolean>} true if audio is running after recovery
   */
  async recover() {
    if (!this.#initialized) return false;

    if (__DEV__) console.log('[Dbg-SuperSonic] Attempting recovery...');

    if (await this.resume()) {
      if (__DEV__) console.log('[Dbg-SuperSonic] Quick resume succeeded');
      return true;
    }

    if (__DEV__) console.log('[Dbg-SuperSonic] Resume failed, doing full reload');
    return await this.reload();
  }

  /**
   * Quick resume - just resumes AudioContext and resyncs timing.
   * Memory and node tree are preserved. Does NOT emit 'setup' event.
   * Use when you know the worklet is still running (e.g., tab was just backgrounded briefly).
   * @returns {Promise<boolean>} true if worklet is running after resume
   */
  async resume() {
    if (!this.#initialized) return false;
    if (!this.#audioContext || !this.#metricsReader.getMetricsView()) {
      return false;
    }

    try {
      await this.#audioContext.resume();
    } catch (e) {
      // Resume may fail
    }

    const metricsView = this.#metricsReader.getMetricsView();
    const count1 = metricsView[0]; // PROCESS_COUNT
    await new Promise(resolve => setTimeout(resolve, 200));
    const count2 = metricsView[0];

    const isRunning = count2 > count1;
    if (isRunning) {
      this.#ntpTiming.resync();
      this.#eventEmitter.emit('resumed');
    }

    return isRunning;
  }

  /**
   * Full reload - destroys and recreates worklet/WASM, restores synthdefs and buffers.
   * Emits 'setup' event so you can rebuild groups, FX chains, bus routing.
   * Use when the worklet was killed (e.g., long background, browser reclaimed memory).
   * @returns {Promise<boolean>} true if reload succeeded
   */
  async reload() {
    if (!this.#initialized) return false;

    this.#eventEmitter.emit('reload:start');

    const cachedSynthDefs = new Map(this.loadedSynthDefs);
    const cachedBuffers = this.#bufferManager?.getAllocatedBuffers() || [];

    await this.#partialShutdown();
    await this.#partialInit();

    // Restore synthdefs
    for (const [name, data] of cachedSynthDefs) {
      try {
        await this.send('/d_recv', data);
      } catch (e) {
        console.error(`[SuperSonic] Failed to restore synthdef ${name}:`, e);
      }
    }

    // Restore buffers
    for (const buf of cachedBuffers) {
      try {
        if (this.#config.mode === 'postMessage' && buf.source) {
          if (buf.source.type === 'file') {
            await this.loadSample(buf.bufnum, buf.source.path, buf.source.startFrame || 0, buf.source.numFrames || 0);
          }
        } else {
          const uuid = crypto.randomUUID();
          await this.send('/b_allocPtr', buf.bufnum, buf.ptr, buf.numFrames, buf.numChannels, buf.sampleRate, uuid);
        }
      } catch (e) {
        console.error(`[SuperSonic] Failed to restore buffer ${buf.bufnum}:`, e);
      }
    }

    if (cachedSynthDefs.size > 0 || cachedBuffers.length > 0) {
      await this.sync();
    }

    this.#eventEmitter.emit('reload:complete', { success: true });
    return true;
  }

  async #partialShutdown() {
    this.#ntpTiming?.stopDriftTimer();
    this.#syncListeners?.clear();
    this.#syncListeners = null;

    if (this.#osc) {
      this.#osc.cancelAll();
      this.#osc.dispose();
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

    this.#initialized = false;
    this.loadedSynthDefs.clear();
    this.#initPromise = null;
    this.#directWriter = null;
    this.#ntpTiming?.reset();
  }

  async #partialInit() {
    this.#initializing = true;
    this.bootStats.initStartTime = performance.now();

    try {
      this.#initializeAudioContext();
      if (this.#bufferManager) {
        this.#bufferManager.updateAudioContext(this.#audioContext);
      }
      this.#initializeOSCRewriter();
      const wasmBytes = await this.#loadWasm();
      await this.#initializeAudioWorklet(wasmBytes);
      await this.#initializeOSC();
      await this.#finishInitialization();
    } catch (error) {
      this.#initializing = false;
      this.#initPromise = null;
      console.error("[SuperSonic] Partial init failed:", error);
      this.#eventEmitter.emit('error', error);
      throw error;
    }
  }

  // ============================================================================
  // NODE TREE API
  // ============================================================================

  getRawTree() {
    if (!this.#initialized) {
      return { nodeCount: 0, version: 0, droppedCount: 0, nodes: [] };
    }

    const bc = this.#metricsReader.bufferConstants;
    if (!bc) {
      return { nodeCount: 0, version: 0, droppedCount: 0, nodes: [] };
    }

    let buffer, treeOffset;
    if (this.#config.mode === 'postMessage') {
      const snapshot = this.#metricsReader.getSnapshotBuffer();
      if (!snapshot) {
        return { nodeCount: 0, version: 0, droppedCount: 0, nodes: [] };
      }
      buffer = snapshot;
      treeOffset = bc.METRICS_SIZE;
    } else {
      const sab = this.#metricsReader.sharedBuffer;
      if (!sab) {
        return { nodeCount: 0, version: 0, droppedCount: 0, nodes: [] };
      }
      buffer = sab;
      treeOffset = this.#metricsReader.ringBufferBase + bc.NODE_TREE_START;
    }

    return parseNodeTree(buffer, treeOffset, bc);
  }

  getTree() {
    const raw = this.getRawTree();

    // Build hierarchical tree from flat node list
    const buildNode = (rawNode) => ({
      id: rawNode.id,
      type: rawNode.isGroup ? 'group' : 'synth',
      defName: rawNode.defName,
      children: []
    });

    // Create node map for efficient lookup
    const nodeMap = new Map();
    for (const rawNode of raw.nodes) {
      nodeMap.set(rawNode.id, buildNode(rawNode));
    }

    // Build parent-child relationships
    let root = null;
    for (const rawNode of raw.nodes) {
      const node = nodeMap.get(rawNode.id);
      if (rawNode.parentId === -1 || rawNode.parentId === 0 && rawNode.id === 0) {
        // Root node (id 0 with parentId -1 or 0)
        root = node;
      } else {
        const parent = nodeMap.get(rawNode.parentId);
        if (parent) {
          parent.children.push(node);
        }
      }
    }

    return {
      nodeCount: raw.nodeCount,
      version: raw.version,
      droppedCount: raw.droppedCount,
      root: root || { id: 0, type: 'group', defName: '', children: [] }
    };
  }

  // ============================================================================
  // AUDIO CAPTURE API
  // ============================================================================

  startCapture() {
    this.#ensureInitialized("start capture");
    this.#audioCapture.start();
  }

  stopCapture() {
    this.#ensureInitialized("stop capture");
    return this.#audioCapture.stop();
  }

  isCaptureEnabled() {
    return this.#audioCapture.isEnabled();
  }

  getCaptureFrames() {
    return this.#audioCapture.getFrameCount();
  }

  getMaxCaptureDuration() {
    return this.#audioCapture.getMaxDuration();
  }

  // ============================================================================
  // OSC MESSAGING API
  // ============================================================================

  async send(address, ...args) {
    this.#ensureInitialized("send OSC messages");

    // Block unsupported commands
    const blocked = {
      "/d_load": "Use loadSynthDef() or send /d_recv with synthdef bytes instead.",
      "/d_loadDir": "Use loadSynthDef() or send /d_recv with synthdef bytes instead.",
      "/b_read": "Use loadSample() to load audio into a buffer.",
      "/b_readChannel": "Use loadSample() to load audio into a buffer.",
      "/b_write": "Writing audio files is not available in the browser.",
      "/b_close": "Writing audio files is not available in the browser.",
      "/clearSched": "Use cancelAllScheduled() or the fine-grained cancelTag(), cancelSession(), cancelSessionTag() methods instead.",
      "/dumpOSC": "Use browser developer tools to inspect OSC messages.",
      "/error": "SuperSonic always enables error notifications so you never miss a /fail message.",
    };

    if (blocked[address]) {
      throw new Error(`${address} is not supported in SuperSonic. ${blocked[address]}`);
    }

    // Cache synthdefs for /d_recv
    if (address === "/d_recv") {
      const synthdefBytes = args[0];
      if (synthdefBytes instanceof Uint8Array || synthdefBytes instanceof ArrayBuffer) {
        const bytes = synthdefBytes instanceof ArrayBuffer ? new Uint8Array(synthdefBytes) : synthdefBytes;
        const name = extractSynthDefName(bytes) || 'unknown';
        this.loadedSynthDefs.set(name, bytes);
      }
    }

    // Track synthdef frees
    if (address === "/d_free") {
      for (const name of args) {
        if (typeof name === "string") {
          this.loadedSynthDefs.delete(name);
        }
      }
    } else if (address === "/d_freeAll") {
      this.loadedSynthDefs.clear();
    }

    const oscArgs = args.map((arg) => {
      if (typeof arg === "string") return { type: "s", value: arg };
      if (typeof arg === "number") return { type: Number.isInteger(arg) ? "i" : "f", value: arg };
      if (arg instanceof Uint8Array || arg instanceof ArrayBuffer) {
        return { type: "b", value: arg instanceof ArrayBuffer ? new Uint8Array(arg) : arg };
      }
      throw new Error(`Unsupported argument type: ${typeof arg}`);
    });

    const message = { address, args: oscArgs };
    const oscData = SuperSonic.osc.encode(message);

    if (this.#config.debug || this.#config.debugOscOut) {
      const maxLen = this.#config.activityConsoleLog.oscOutMaxLineLength ?? this.#config.activityConsoleLog.maxLineLength;
      const argsStr = args.map(a => {
        if (a instanceof Uint8Array || a instanceof ArrayBuffer) return `<${a.byteLength || a.length} bytes>`;
        const str = JSON.stringify(a);
        return str.length > maxLen ? str.slice(0, maxLen) + '...' : str;
      }).join(', ');
      console.log(`[OSC →] ${address}${argsStr ? ' ' + argsStr : ''}`);
    }

    return this.sendOSC(oscData);
  }

  async sendOSC(oscData, options = {}) {
    this.#ensureInitialized("send OSC data");

    const uint8Data = this.#toUint8Array(oscData);
    const preparedData = await this.#prepareOutboundPacket(uint8Data);

    this.#metricsReader.addMetric("oscOutMessagesSent");
    this.#metricsReader.addMetric("oscOutBytesSent", preparedData.length);

    this.#eventEmitter.emit('message:sent', preparedData);

    // Fast path: try direct ring buffer write
    if (this.#directWriter?.tryWrite(preparedData)) {
      this.#metricsReader.addMetric("preschedulerBypassed");
      return;
    }

    const timing = this.#ntpTiming?.calculateBundleWait(preparedData);

    // PostMessage mode: send immediate messages directly to worklet, bypassing prescheduler
    if (this.#config.mode === 'postMessage' && !timing) {
      this.#osc.sendImmediate(preparedData);
      return;
    }

    // Size guard for scheduled bundles
    const slotSize = this.#metricsReader.bufferConstants?.scheduler_slot_size;
    if (timing && slotSize && preparedData.length > slotSize) {
      throw new Error(
        `OSC bundle too large to schedule (${preparedData.length} > ${slotSize} bytes). ` +
        `Use immediate timestamp (0 or 1) for large messages, or reduce bundle size.`
      );
    }

    const sendOptions = { ...options };
    if (timing) {
      sendOptions.audioTimeS = timing.audioTimeS;
      sendOptions.currentTimeS = timing.currentTimeS;
    }

    this.#osc.sendWithOptions(preparedData, sendOptions);
  }

  cancelTag(runTag) {
    this.#ensureInitialized("cancel by tag");
    this.#osc.cancelTag(runTag);
  }

  cancelSession(sessionId) {
    this.#ensureInitialized("cancel by session");
    this.#osc.cancelSession(sessionId);
  }

  cancelSessionTag(sessionId, runTag) {
    this.#ensureInitialized("cancel by session and tag");
    this.#osc.cancelSessionTag(sessionId, runTag);
  }

  cancelAllScheduled() {
    this.#ensureInitialized("cancel all scheduled");
    this.#osc.cancelAll();
  }

  // ============================================================================
  // ASSET LOADING API
  // ============================================================================

  async loadSynthDef(nameOrPath) {
    this.#ensureInitialized("load synthdef");

    let path;
    if (this.#looksLikePathOrURL(nameOrPath)) {
      path = nameOrPath;
    } else {
      if (!this.#synthdefBaseURL) {
        throw new Error("synthdefBaseURL not configured.");
      }
      path = `${this.#synthdefBaseURL}${nameOrPath}.scsyndef`;
    }

    const synthName = extractSynthDefName(path);

    const arrayBuffer = await this.#assetLoader.fetch(path, { type: 'synthdef', name: synthName });
    const synthdefData = new Uint8Array(arrayBuffer);
    await this.send("/d_recv", synthdefData);

    return { name: synthName, size: synthdefData.length };
  }

  async loadSynthDefs(names) {
    this.#ensureInitialized("load synthdefs");

    const results = {};
    await Promise.all(
      names.map(async (name) => {
        try {
          await this.loadSynthDef(name);
          results[name] = { success: true };
        } catch (error) {
          results[name] = { success: false, error: error.message };
        }
      })
    );
    return results;
  }

  async loadSample(bufnum, source, startFrame = 0, numFrames = 0) {
    this.#ensureInitialized("load samples");

    let bufferInfo;

    if (typeof source === 'string') {
      // Path or URL
      bufferInfo = await this.#bufferManager.prepareFromFile({
        bufnum, path: source, startFrame, numFrames,
      });
    } else if (source instanceof ArrayBuffer || ArrayBuffer.isView(source)) {
      // ArrayBuffer or TypedArray
      bufferInfo = await this.#bufferManager.prepareFromBlob({
        bufnum, blob: source, startFrame, numFrames,
      });
    } else if (source instanceof Blob) {
      // File or Blob - read into ArrayBuffer first
      const arrayBuffer = await source.arrayBuffer();
      bufferInfo = await this.#bufferManager.prepareFromBlob({
        bufnum, blob: arrayBuffer, startFrame, numFrames,
      });
    } else {
      throw new Error('loadSample source must be a path/URL string, ArrayBuffer, TypedArray, or File/Blob');
    }

    await this.send(
      "/b_allocPtr", bufnum, bufferInfo.ptr, bufferInfo.numFrames,
      bufferInfo.numChannels, bufferInfo.sampleRate, bufferInfo.uuid
    );

    return bufferInfo.allocationComplete;
  }

  async sync(syncId = Math.floor(Math.random() * 2147483647)) {
    this.#ensureInitialized("sync");

    const syncPromise = new Promise((resolve, reject) => {
      const timeout = setTimeout(() => {
        this.#syncListeners?.delete(syncId);
        reject(new Error("Timeout waiting for /synced response"));
      }, SYNC_TIMEOUT_MS);

      const messageHandler = () => {
        clearTimeout(timeout);
        this.#syncListeners.delete(syncId);
        resolve();
      };

      if (!this.#syncListeners) this.#syncListeners = new Map();
      this.#syncListeners.set(syncId, messageHandler);
    });

    await this.send("/sync", syncId);
    await syncPromise;

    if (this.#config.mode === 'postMessage') {
      await new Promise(r => setTimeout(r, this.#config.snapshotIntervalMs * 2));
    }
  }

  // ============================================================================
  // INFO API
  // ============================================================================

  getInfo() {
    this.#ensureInitialized("get info");

    return {
      sampleRate: this.#audioContext.sampleRate,
      numBuffers: this.#config.worldOptions.numBuffers,
      totalMemory: this.#config.memory.totalMemory,
      wasmHeapSize: this.#config.memory.wasmHeapSize,
      bufferPoolSize: this.#config.memory.bufferPoolSize,
      bootTimeMs: this.bootStats.initDuration,
      capabilities: { ...this.#capabilities },
      version: this.#version,
    };
  }

  // ============================================================================
  // LIFECYCLE API
  // ============================================================================

  async shutdown() {
    if (!this.#initialized && !this.#initializing) return;

    this.#eventEmitter.emit("shutdown");
    this.#ntpTiming?.stopDriftTimer();
    this.#syncListeners?.clear();
    this.#syncListeners = null;

    if (this.#osc) {
      this.#osc.cancelAll();
      this.#osc.dispose();
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

    if (this.#bufferManager) {
      this.#bufferManager.destroy();
      this.#bufferManager = null;
    }

    this.#oscRewriter = null;
    this.#directWriter = null;
    this.#initialized = false;
    this.loadedSynthDefs.clear();
    this.#initPromise = null;
    this.#wasmMemory = null;
    this.#ntpTiming?.reset();
    this.bootStats = { initStartTime: null, initDuration: null };
  }

  async destroy() {
    this.#eventEmitter.emit("destroy");
    await this.shutdown();
    this.#cachedWasmBytes = null;
    this.#eventEmitter.clearAllListeners();
  }

  async reset(config = {}) {
    await this.shutdown();
    await this.init(config);
  }

  // ============================================================================
  // PRIVATE: INITIALIZATION HELPERS
  // ============================================================================

  #setAndValidateCapabilities() {
    this.#capabilities = {
      audioWorklet: typeof AudioWorklet !== "undefined",
      sharedArrayBuffer: typeof SharedArrayBuffer !== "undefined",
      crossOriginIsolated: window.crossOriginIsolated === true,
      atomics: typeof Atomics !== "undefined",
      webWorker: typeof Worker !== "undefined",
    };

    const mode = this.#config.mode;
    const required = ["audioWorklet", "webWorker"];

    if (mode === 'sab') {
      required.push("sharedArrayBuffer", "crossOriginIsolated", "atomics");
    }

    const missing = required.filter((f) => !this.#capabilities[f]);

    if (missing.length > 0) {
      const error = new Error(`Missing required features for ${mode} mode: ${missing.join(", ")}`);
      if (mode === 'sab' && !this.#capabilities.crossOriginIsolated) {
        error.message += "\n\nConsider using mode: 'postMessage' which doesn't require COOP/COEP headers.";
      }
      throw error;
    }

    if (mode !== 'sab' && mode !== 'postMessage') {
      throw new Error(`Invalid mode: '${mode}'. Use 'sab' or 'postMessage'.`);
    }
  }

  #initializeMemory() {
    const memConfig = this.#config.memory;
    const mode = this.#config.mode;

    if (mode === 'sab') {
      this.#wasmMemory = new WebAssembly.Memory({
        initial: memConfig.totalPages,
        maximum: memConfig.totalPages,
        shared: true,
      });
    } else {
      this.#wasmMemory = null;
    }
  }

  #initializeAudioContext() {
    if (this.#config.audioContext) {
      this.#audioContext = this.#config.audioContext;
    } else {
      this.#audioContext = new AudioContext(this.#config.audioContextOptions);
    }

    this.#audioContext.addEventListener('statechange', () => {
      const state = this.#audioContext?.state;
      if (!state) return;

      const previousState = this.#previousAudioContextState;
      this.#previousAudioContextState = state;

      if (state === 'running' && (previousState === 'suspended' || previousState === 'interrupted')) {
        this.#ntpTiming?.resync();
      }

      this.#eventEmitter.emit('audiocontext:statechange', { state });
      if (state === 'suspended') this.#eventEmitter.emit('audiocontext:suspended');
      else if (state === 'running') this.#eventEmitter.emit('audiocontext:resumed');
      else if (state === 'interrupted') this.#eventEmitter.emit('audiocontext:interrupted');
    });
  }

  #initializeBufferManager() {
    const sharedBuffer = this.#config.mode === 'sab' ? this.#wasmMemory.buffer : null;

    this.#bufferManager = new BufferManager({
      mode: this.#config.mode,
      audioContext: this.#audioContext,
      sharedBuffer: sharedBuffer,
      bufferPoolConfig: {
        start: this.#config.memory.bufferPoolOffset,
        size: this.#config.memory.bufferPoolSize,
      },
      sampleBaseURL: this.#sampleBaseURL,
      maxBuffers: this.#config.worldOptions.numBuffers,
      assetLoader: this.#assetLoader,
    });
  }

  #initializeOSCRewriter() {
    this.#oscRewriter = new OSCRewriter({
      bufferManager: this.#bufferManager,
      getDefaultSampleRate: () => this.#audioContext?.sampleRate || 44100,
    });
  }

  async #loadWasm() {
    if (this.#cachedWasmBytes) return this.#cachedWasmBytes;

    const wasmName = this.#config.wasmUrl.split('/').pop();
    this.#eventEmitter.emit('loading:start', { type: 'wasm', name: wasmName });

    const wasmResponse = await fetch(this.#config.wasmUrl);
    if (!wasmResponse.ok) {
      throw new Error(`Failed to load WASM: ${wasmResponse.status} ${wasmResponse.statusText}`);
    }

    const wasmBytes = await wasmResponse.arrayBuffer();
    this.#eventEmitter.emit('loading:complete', { type: 'wasm', name: wasmName, size: wasmBytes.byteLength });
    this.#cachedWasmBytes = wasmBytes;

    return wasmBytes;
  }

  async #initializeAudioWorklet(wasmBytes) {
    await addWorkletModule(this.#audioContext.audioWorklet, this.#config.workletUrl);

    this.#workletNode = new AudioWorkletNode(this.#audioContext, "scsynth-processor", {
      numberOfInputs: 1,
      numberOfOutputs: 1,
      outputChannelCount: [2],
    });

    if (this.#config.autoConnect) {
      this.#workletNode.connect(this.#audioContext.destination);
    }

    this.#node = this.#createNodeWrapper();
    this.#workletNode.port.start();
    this.#setupMessageHandlers();

    const mode = this.#config.mode;
    const sharedBuffer = mode === 'sab' ? this.#wasmMemory.buffer : null;

    this.#workletNode.port.postMessage({
      type: "init",
      mode: mode,
      sharedBuffer: sharedBuffer,
      snapshotIntervalMs: this.#config.snapshotIntervalMs,
    });

    const loadWasmMsg = {
      type: "loadWasm",
      wasmBytes: wasmBytes,
      worldOptions: this.#config.worldOptions,
      sampleRate: this.#audioContext.sampleRate,
    };

    if (mode === 'sab') {
      loadWasmMsg.wasmMemory = this.#wasmMemory;
    } else {
      loadWasmMsg.memoryPages = this.#config.memoryPages || 1280;
    }

    this.#workletNode.port.postMessage(loadWasmMsg);

    await this.#waitForWorkletInit();
    this.#bufferManager.setWorkletPort(this.#workletNode.port);
  }

  #createNodeWrapper() {
    const worklet = this.#workletNode;
    return Object.freeze({
      connect: (...args) => worklet.connect(...args),
      disconnect: (...args) => worklet.disconnect(...args),
      get context() { return worklet.context; },
      get numberOfOutputs() { return worklet.numberOfOutputs; },
      get numberOfInputs() { return worklet.numberOfInputs; },
      get channelCount() { return worklet.channelCount; },
      // Expose AudioWorkletNode as input for connecting external sources
      get input() { return worklet; },
    });
  }

  async #initializeOSC() {
    const mode = this.#config.mode;
    const bc = this.#metricsReader.bufferConstants;
    const ringBufferBase = this.#metricsReader.ringBufferBase;
    const sharedBuffer = this.#metricsReader.sharedBuffer;

    // Create transport based on mode
    const transportConfig = {
      workerBaseURL: this.#config.workerBaseURL,
      preschedulerCapacity: this.#config.preschedulerCapacity,
      getAudioContextTime: () => this.#audioContext?.currentTime ?? 0,
      getNTPStartTime: () => this.#ntpTiming?.getNTPStartTime() ?? 0,
    };

    if (mode === 'sab') {
      transportConfig.sharedBuffer = sharedBuffer;
      transportConfig.ringBufferBase = ringBufferBase;
      transportConfig.bufferConstants = bc;
    }

    this.#osc = createTransport(mode, transportConfig);

    // Handle raw OSC replies - parse and dispatch
    this.#osc.onReply((oscData, sequence) => {
      // Emit raw message event
      this.#eventEmitter.emit('message:raw', { oscData, sequence });

      // Parse OSC and emit parsed message
      try {
        const options = { metadata: false, unpackSingleArgs: false };
        const msg = oscLib.readPacket(oscData, options);

        // Handle special messages
        if (msg.address === "/supersonic/buffer/freed") {
          this.#bufferManager?.handleBufferFreed(msg.args);
        } else if (msg.address === "/supersonic/buffer/allocated") {
          this.#bufferManager?.handleBufferAllocated(msg.args);
        } else if (msg.address === "/synced" && msg.args.length > 0) {
          const syncId = msg.args[0];
          if (this.#syncListeners?.has(syncId)) {
            this.#syncListeners.get(syncId)(msg);
          }
        }

        this.#eventEmitter.emit('message', msg);

        if (this.#config.debug || this.#config.debugOscIn) {
          const maxLen = this.#config.activityConsoleLog.oscInMaxLineLength ?? this.#config.activityConsoleLog.maxLineLength;
          const argsStr = msg.args?.map(a => {
            const str = JSON.stringify(a);
            return str.length > maxLen ? str.slice(0, maxLen) + '...' : str;
          }).join(', ') || '';
          console.log(`[← OSC] ${msg.address}${argsStr ? ' ' + argsStr : ''}`);
        }
      } catch (e) {
        console.error('[SuperSonic] Failed to decode OSC message:', e);
      }
    });

    // Handle debug messages
    this.#osc.onDebug((msg) => {
      const eventMaxLen = this.#config.activityEvent.scsynthMaxLineLength ?? this.#config.activityEvent.maxLineLength;
      if (eventMaxLen > 0 && msg.text?.length > eventMaxLen) {
        msg = { ...msg, text: msg.text.slice(0, eventMaxLen) + '...' };
      }
      this.#eventEmitter.emit('debug', msg);

      if (this.#config.debug || this.#config.debugScsynth) {
        const maxLen = this.#config.activityConsoleLog.scsynthMaxLineLength ?? this.#config.activityConsoleLog.maxLineLength;
        const text = msg.text.length > maxLen ? msg.text.slice(0, maxLen) + '...' : msg.text;
        console.log(`[synth] ${text}`);
      }
    });

    // Handle errors
    this.#osc.onError((error, workerName) => {
      console.error(`[SuperSonic] ${workerName} error:`, error);
      this.#eventEmitter.emit('error', new Error(`${workerName}: ${error}`));
    });

    // Initialize transport
    if (mode === 'sab') {
      await this.#osc.initialize();
    } else {
      await this.#osc.initialize(this.#workletNode.port);

      // Handle early debug messages that arrived before transport was ready
      if (this.#earlyDebugMessages?.length > 0) {
        for (const data of this.#earlyDebugMessages) {
          this.#osc.handleDebugRaw(data);
        }
      }
      this.#debugRawHandler = (data) => this.#osc.handleDebugRaw(data);
      this.#earlyDebugMessages = [];
    }
  }

  async #finishInitialization() {
    this.#initialized = true;
    this.#initializing = false;
    this.bootStats.initDuration = performance.now() - this.bootStats.initStartTime;

    await this.#eventEmitter.emitAsync('setup');
    this.#eventEmitter.emit('ready', { capabilities: this.#capabilities, bootStats: this.bootStats });

    // TODO(v1): Consider whether to keep this dev console helper.
    // It auto-registers instances to window.__supersonic__ for quick debugging (ss.metrics(), ss.tree(), etc.)
    // Unusual pattern - most libs expect devs to do `window.sonic = sonic` themselves.
    // Useful but the `instances` array for multiple engines is over-engineered.
    if (__DEV__ && typeof window !== 'undefined') {
      if (!window.__supersonic__) {
        const ss = window.__supersonic__ = { instances: [] };
        Object.defineProperties(ss, {
          primary: { get: () => ss.instances[0] },
          layout: { get: () => ss.primary?.bufferConstants },
        });
        ss.metrics = () => ss.primary?.getMetrics();
        ss.tree = () => ss.primary?.getTree();
        ss.rawTree = () => ss.primary?.getRawTree();
        ss.inspect = () => ss.primary ? SuperSonic.inspect(ss.primary) : null;
      }
      window.__supersonic__.instances.push(this);
    }
  }

  #waitForWorkletInit() {
    return new Promise((resolve, reject) => {
      const timeout = setTimeout(() => {
        reject(new Error("AudioWorklet initialization timeout"));
      }, WORKLET_INIT_TIMEOUT_MS);

      const messageHandler = async (event) => {
        if (event.data.type === "error") {
          clearTimeout(timeout);
          this.#workletNode.port.removeEventListener("message", messageHandler);
          reject(new Error(event.data.error || "AudioWorklet error"));
          return;
        }

        if (event.data.type === "initialized") {
          clearTimeout(timeout);
          this.#workletNode.port.removeEventListener("message", messageHandler);

          if (event.data.success) {
            const ringBufferBase = event.data.ringBufferBase ?? 0;
            const bufferConstants = event.data.bufferConstants;
            const sharedBuffer = this.#config.mode === 'sab' ? this.#wasmMemory.buffer : null;

            // Initialize metrics reader
            this.#metricsReader.initSharedViews(sharedBuffer, ringBufferBase, bufferConstants);

            // Warn if maxNodes exceeds mirror capacity
            const maxNodes = this.#config.worldOptions?.maxNodes ?? 1024;
            const mirrorMax = bufferConstants?.NODE_TREE_MIRROR_MAX_NODES ?? 1024;
            if (maxNodes > mirrorMax) {
              console.warn(
                `SuperSonic: maxNodes (${maxNodes}) exceeds NODE_TREE_MIRROR_MAX_NODES (${mirrorMax}). ` +
                `The node tree mirror will not show all nodes. Rebuild with NODE_TREE_MIRROR_MAX_NODES=${maxNodes} to fix.`
              );
            }

            // Initialize NTP timing
            this.#ntpTiming = new NTPTiming({
              mode: this.#config.mode,
              audioContext: this.#audioContext,
              workletPort: this.#workletNode.port,
            });
            this.#ntpTiming.initSharedViews(sharedBuffer, ringBufferBase, bufferConstants);
            await this.#ntpTiming.initialize();
            this.#ntpTiming.startDriftTimer();

            // Initialize audio capture (SAB mode only)
            if (this.#config.mode === 'sab') {
              this.#audioCapture.update(sharedBuffer, ringBufferBase, bufferConstants);

              // Initialize direct writer
              this.#directWriter = new DirectWriter({
                sharedBuffer: sharedBuffer,
                ringBufferBase: ringBufferBase,
                bufferConstants: bufferConstants,
                getAudioContextTime: () => this.#audioContext?.currentTime ?? null,
                getNTPStartTime: () => this.#ntpTiming?.getNTPStartTime() ?? 0,
              });
            }

            // PostMessage mode: set initial snapshot
            if (this.#config.mode === 'postMessage' && event.data.initialSnapshot) {
              this.#metricsReader.updateSnapshot(event.data.initialSnapshot);
            }

            resolve();
          } else {
            reject(new Error(event.data.error || "AudioWorklet initialization failed"));
          }
        }
      };

      this.#workletNode.port.addEventListener("message", messageHandler);
      this.#workletNode.port.start();
    });
  }

  #setupMessageHandlers() {
    this.#workletNode.port.addEventListener('message', (event) => {
      const { data } = event;

      switch (data.type) {
        case "error":
          console.error("[Worklet] Error:", data.error);
          this.#eventEmitter.emit('error', new Error(data.error));
          break;

        case "version":
          this.#version = data.version;
          break;

        case "snapshot":
          if (data.buffer) {
            this.#metricsReader.updateSnapshot(data.buffer);
            this.#snapshotsSent = data.snapshotsSent;
          }
          break;

        case "debugRawBatch":
          if (this.#debugRawHandler) {
            this.#debugRawHandler(data);
          } else {
            this.#earlyDebugMessages.push(data);
          }
          break;
      }
    });
  }

  // ============================================================================
  // PRIVATE: METRICS
  // ============================================================================

  #gatherMetrics() {
    const preschedulerMetrics = this.#osc?.getPreschedulerMetrics();

    return this.#metricsReader.gatherMetrics({
      preschedulerMetrics: preschedulerMetrics,
      transportMetrics: this.#osc?.getMetrics(),
      driftOffsetMs: this.#ntpTiming?.getDriftOffset() ?? 0,
      audioContextState: this.#audioContext?.state || "unknown",
      bufferPoolStats: this.#bufferManager?.getStats(),
      loadedSynthDefsCount: this.loadedSynthDefs?.size || 0,
      preschedulerCapacity: this.#config.preschedulerCapacity,
    });
  }

  // ============================================================================
  // PRIVATE: UTILITIES
  // ============================================================================

  #ensureInitialized(actionDescription = "perform this operation") {
    if (!this.#initialized) {
      throw new Error(`SuperSonic not initialized. Call init() before attempting to ${actionDescription}.`);
    }
  }

  #looksLikePathOrURL(str) {
    return str.includes("/") || str.includes("://");
  }

  #toUint8Array(data) {
    if (data instanceof Uint8Array) return data;
    if (data instanceof ArrayBuffer) return new Uint8Array(data);
    throw new Error("oscData must be ArrayBuffer or Uint8Array");
  }

  async #prepareOutboundPacket(uint8Data) {
    const decodeOptions = { metadata: true, unpackSingleArgs: false };
    try {
      const decodedPacket = SuperSonic.osc.decode(uint8Data, decodeOptions);
      const { packet, changed } = await this.#oscRewriter.rewritePacket(decodedPacket);
      if (!changed) return uint8Data;
      return SuperSonic.osc.encode(packet);
    } catch (error) {
      console.error("[SuperSonic] Failed to prepare OSC packet:", error);
      throw error;
    }
  }
}

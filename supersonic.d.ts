// SPDX-License-Identifier: MIT OR GPL-3.0-or-later
// Copyright (c) 2025 Sam Aaron
//
// Type declarations for supersonic-scsynth

// ============================================================================
// OSC Types
// ============================================================================

/** OSC argument: plain values that can be sent in an OSC message */
export type OscArg =
  | number
  | string
  | boolean
  | Uint8Array
  | ArrayBuffer
  | { type: 'int64'; value: number | bigint }
  | { type: 'double'; value: number }
  | { type: 'timetag'; value: number };

/** Decoded OSC message: [address, ...args] */
export type OscMessage = [string, ...OscArg[]];

/** Decoded OSC bundle */
export interface OscBundle {
  timeTag: number;
  packets: (OscMessage | OscBundle)[];
}

/** NTP timetag value: number, [seconds, fraction] pair, 1 for immediate, or null/undefined */
export type NTPTimeTag = number | [number, number] | 1 | null | undefined;

/** OSC packet for encoding in a bundle */
export type OscBundlePacket =
  | OscMessage
  | { address: string; args?: OscArg[] }
  | { timeTag: NTPTimeTag; packets: OscBundlePacket[] };

// ============================================================================
// Configuration Types
// ============================================================================

/** Transport mode */
export type TransportMode = 'sab' | 'postMessage';

/** scsynth engine configuration options */
export interface ScsynthOptions {
  /** Max audio buffers (1-65535, default: 1024) */
  numBuffers?: number;
  /** Max synthesis nodes (default: 1024) */
  maxNodes?: number;
  /** Max synth definitions (default: 1024) */
  maxGraphDefs?: number;
  /** Max wire buffers for internal routing (default: 64) */
  maxWireBufs?: number;
  /** Audio bus channels (default: 128) */
  numAudioBusChannels?: number;
  /** Input bus channels (default: 2) */
  numInputBusChannels?: number;
  /** Output bus channels (1-128, default: 2) */
  numOutputBusChannels?: number;
  /** Control bus channels (default: 4096) */
  numControlBusChannels?: number;
  /** Audio buffer length - must be 128 (WebAudio constraint) */
  bufLength?: 128;
  /** RT memory pool size in KB (default: 8192) */
  realTimeMemorySize?: number;
  /** Number of random number generators (default: 64) */
  numRGens?: number;
  /** Clock source mode (default: false) */
  realTime?: boolean;
  /** Memory locking - not applicable in browser (default: false) */
  memoryLocking?: boolean;
  /** Auto-load synthdefs: 0 or 1 (default: 0) */
  loadGraphDefs?: 0 | 1;
  /** Preferred sample rate, 0 for auto (default: 0) */
  preferredSampleRate?: number;
  /** Debug verbosity 0-4 (default: 0) */
  verbosity?: number;
}

/** Activity event line length configuration */
export interface ActivityLineConfig {
  maxLineLength?: number;
  scsynthMaxLineLength?: number | null;
  oscInMaxLineLength?: number | null;
  oscOutMaxLineLength?: number | null;
}

/** SuperSonic constructor options */
export interface SuperSonicOptions {
  /** Transport mode (default: 'postMessage') */
  mode?: TransportMode;

  // URL configuration (at least baseURL or coreBaseURL+wasmBaseURL+workerBaseURL required)
  /** Convenience shorthand when all assets are co-located */
  baseURL?: string;
  /** Base URL for WASM and workers (supersonic-scsynth-core package) */
  coreBaseURL?: string;
  /** Base URL for worker scripts */
  workerBaseURL?: string;
  /** Base URL for WASM files */
  wasmBaseURL?: string;
  /** Full URL to the WASM file */
  wasmUrl?: string;
  /** Full URL to the worklet script */
  workletUrl?: string;

  // Asset URLs
  /** Base URL for sample files */
  sampleBaseURL?: string;
  /** Base URL for synthdef files */
  synthdefBaseURL?: string;

  // AudioContext
  /** Provide your own AudioContext */
  audioContext?: AudioContext;
  /** Options passed to new AudioContext() */
  audioContextOptions?: AudioContextOptions;
  /** Auto-connect worklet to destination (default: true) */
  autoConnect?: boolean;

  // scsynth configuration
  /** Engine options passed to scsynth World_New() */
  scsynthOptions?: ScsynthOptions;

  // Timing & scheduling
  /** Snapshot interval in ms for postMessage mode */
  snapshotIntervalMs?: number;
  /** Max pending prescheduler events (default: 65536) */
  preschedulerCapacity?: number;
  /** Bypass lookahead threshold in ms (default: 500) */
  bypassLookaheadMs?: number;

  // Debugging
  /** Enable all debug logging (default: false) */
  debug?: boolean;
  /** Log scsynth debug output (default: false) */
  debugScsynth?: boolean;
  /** Log OSC messages received from scsynth (default: false) */
  debugOscIn?: boolean;
  /** Log OSC messages sent to scsynth (default: false) */
  debugOscOut?: boolean;

  // Activity event configuration
  activityEvent?: ActivityLineConfig;
  activityConsoleLog?: ActivityLineConfig;

  // Network
  /** Max fetch retries for assets (default: 3) */
  fetchMaxRetries?: number;
  /** Base delay between retries in ms (default: 1000) */
  fetchRetryDelay?: number;
}

// ============================================================================
// Metrics Types
// ============================================================================

/** Complete metrics snapshot from getMetrics() */
export interface SuperSonicMetrics {
  // scsynth metrics
  scsynthProcessCount: number;
  scsynthMessagesProcessed: number;
  scsynthMessagesDropped: number;
  scsynthSchedulerDepth: number;
  scsynthSchedulerPeakDepth: number;
  scsynthSchedulerDropped: number;
  scsynthSequenceGaps: number;
  scsynthWasmErrors: number;
  scsynthSchedulerLates: number;

  // Prescheduler metrics
  preschedulerPending: number;
  preschedulerPendingPeak: number;
  preschedulerBundlesScheduled: number;
  preschedulerDispatched: number;
  preschedulerEventsCancelled: number;
  preschedulerMinHeadroomMs: number;
  preschedulerLates: number;
  preschedulerRetriesSucceeded: number;
  preschedulerRetriesFailed: number;
  preschedulerRetryQueueSize: number;
  preschedulerRetryQueuePeak: number;
  preschedulerMessagesRetried: number;
  preschedulerTotalDispatches: number;
  preschedulerBypassed: number;
  preschedulerMaxLateMs: number;

  // OSC Out metrics
  oscOutMessagesSent: number;
  oscOutBytesSent: number;

  // OSC In metrics
  oscInMessagesReceived: number;
  oscInBytesReceived: number;
  oscInMessagesDropped: number;
  oscInCorrupted: number;

  // Debug metrics
  debugMessagesReceived: number;
  debugBytesReceived: number;

  // Ring buffer usage
  inBufferUsedBytes: number;
  outBufferUsedBytes: number;
  debugBufferUsedBytes: number;
  inBufferPeakBytes: number;
  outBufferPeakBytes: number;
  debugBufferPeakBytes: number;

  // Bypass categories
  bypassNonBundle: number;
  bypassImmediate: number;
  bypassNearFuture: number;
  bypassLate: number;

  // scsynth late timing
  scsynthSchedulerMaxLateMs: number;
  scsynthSchedulerLastLateMs: number;
  scsynthSchedulerLastLateTick: number;

  // Ring buffer direct write failures
  ringBufferDirectWriteFails: number;

  // Context metrics
  driftOffsetMs: number;
  clockOffsetMs: number;
  audioContextState: number;
  bufferPoolUsedBytes: number;
  bufferPoolAvailableBytes: number;
  bufferPoolAllocations: number;
  loadedSynthDefs: number;
  scsynthSchedulerCapacity: number;
  preschedulerCapacity: number;
  inBufferCapacity: number;
  outBufferCapacity: number;
  debugBufferCapacity: number;
  mode: number;
}

/** Metric schema entry */
export interface MetricDefinition {
  offset: number;
  type: 'counter' | 'gauge' | 'constant' | 'enum';
  unit?: string;
  signed?: boolean;
  values?: string[];
  description: string;
}

/** Metrics schema returned by SuperSonic.getMetricsSchema() */
export interface MetricsSchema {
  metrics: Record<keyof SuperSonicMetrics, MetricDefinition>;
  layout: {
    panels: Array<{
      title: string;
      class?: string;
      rows: Array<{
        type?: string;
        label: string;
        cells?: Array<{
          key?: string;
          kind?: string;
          format?: string;
          text?: string;
          sep?: string;
        }>;
        usedKey?: string;
        peakKey?: string;
        capacityKey?: string;
        color?: string;
      }>;
    }>;
  };
  sentinels: {
    HEADROOM_UNSET: number;
  };
}

// ============================================================================
// Node Tree Types
// ============================================================================

/** Hierarchical tree node */
export interface TreeNode {
  id: number;
  type: 'group' | 'synth';
  defName: string;
  children: TreeNode[];
}

/** Hierarchical tree returned by getTree() */
export interface Tree {
  nodeCount: number;
  version: number;
  droppedCount: number;
  root: TreeNode;
}

/** Raw flat tree node */
export interface RawTreeNode {
  id: number;
  parentId: number;
  isGroup: boolean;
  prevId: number;
  nextId: number;
  headId: number;
  defName: string;
}

/** Raw flat tree returned by getRawTree() */
export interface RawTree {
  nodeCount: number;
  version: number;
  droppedCount: number;
  nodes: RawTreeNode[];
}

// ============================================================================
// Info & Snapshot Types
// ============================================================================

/** Engine info returned by getInfo() */
export interface SuperSonicInfo {
  sampleRate: number;
  numBuffers: number;
  totalMemory: number;
  wasmHeapSize: number;
  bufferPoolSize: number;
  bootTimeMs: number | null;
  capabilities: {
    audioWorklet: boolean;
    sharedArrayBuffer: boolean;
    crossOriginIsolated: boolean;
    atomics: boolean;
    webWorker: boolean;
  };
  version: string | null;
}

/** Diagnostic snapshot returned by getSnapshot() */
export interface Snapshot {
  timestamp: string;
  metrics: Record<string, { value: number; description?: string }>;
  nodeTree: RawTree;
  memory: {
    usedJSHeapSize: number;
    totalJSHeapSize: number;
    jsHeapSizeLimit: number;
  } | null;
}

/** Loaded buffer info from getLoadedBuffers() */
export interface LoadedBufferInfo {
  bufnum: number;
  numFrames: number;
  numChannels: number;
  sampleRate: number;
  source: string | null;
  duration: number;
}

/** Result from loadSynthDef() */
export interface LoadSynthDefResult {
  name: string;
  size: number;
}

/** Result from loadSample() */
export interface LoadSampleResult {
  bufnum: number;
  numFrames: number;
  numChannels: number;
  sampleRate: number;
}

/** Boot statistics */
export interface BootStats {
  initStartTime: number | null;
  initDuration: number | null;
}

// ============================================================================
// Event Types
// ============================================================================

/** Map of event names to their callback signatures */
export interface SuperSonicEventMap {
  /** Fired after init completes, before 'ready'. Use for setting up groups, FX chains, etc. */
  'setup': () => void | Promise<void>;
  /** Fired when engine is fully ready */
  'ready': (data: { capabilities: SuperSonicInfo['capabilities']; bootStats: BootStats }) => void;
  /** Decoded OSC message received from scsynth */
  'message': (msg: OscMessage) => void;
  /** Raw OSC bytes received from scsynth */
  'message:raw': (data: { oscData: Uint8Array; sequence: number }) => void;
  /** OSC message sent to scsynth */
  'message:sent': (oscData: Uint8Array, sourceId: number, sequence: number) => void;
  /** Debug text from scsynth */
  'debug': (msg: { text: string }) => void;
  /** Error event */
  'error': (error: Error) => void;
  /** Engine shutdown */
  'shutdown': () => void;
  /** Engine destroyed */
  'destroy': () => void;
  /** Audio resumed after suspend */
  'resumed': () => void;
  /** Reload started */
  'reload:start': () => void;
  /** Reload completed */
  'reload:complete': (data: { success: boolean }) => void;
  /** AudioContext state changed */
  'audiocontext:statechange': (data: { state: AudioContextState }) => void;
  /** AudioContext suspended */
  'audiocontext:suspended': () => void;
  /** AudioContext resumed */
  'audiocontext:resumed': () => void;
  /** AudioContext interrupted (iOS) */
  'audiocontext:interrupted': () => void;
  /** Asset loading started */
  'loading:start': (data: { type: string; name: string }) => void;
  /** Asset loading completed */
  'loading:complete': (data: { type: string; name: string; size: number }) => void;
}

/** All event names */
export type SuperSonicEvent = keyof SuperSonicEventMap;

// ============================================================================
// OscChannel
// ============================================================================

/** OSC message classification category */
export type OscCategory = 'nonBundle' | 'immediate' | 'nearFuture' | 'late' | 'farFuture';

/** OscChannel local metrics */
export interface OscChannelMetrics {
  messagesSent: number;
  bytesSent: number;
  nonBundle: number;
  immediate: number;
  nearFuture: number;
  late: number;
  bypassed: number;
}

/** Transferable config for SAB mode OscChannel */
export interface OscChannelSABTransferable {
  mode: 'sab';
  sharedBuffer: SharedArrayBuffer;
  ringBufferBase: number;
  bufferConstants: Record<string, number>;
  controlIndices: Record<string, number>;
  preschedulerPort: MessagePort | null;
  bypassLookaheadS: number;
  sourceId: number;
  blocking: boolean;
}

/** Transferable config for postMessage mode OscChannel */
export interface OscChannelPMTransferable {
  mode: 'postMessage';
  port: MessagePort;
  preschedulerPort: MessagePort | null;
  bypassLookaheadS: number;
  sourceId: number;
  blocking: boolean;
}

export type OscChannelTransferable = OscChannelSABTransferable | OscChannelPMTransferable;

/**
 * OscChannel - Unified dispatch for sending OSC to the audio worklet.
 *
 * Can be transferred to Web Workers for direct communication
 * with the AudioWorklet, bypassing the main thread.
 */
export class OscChannel {
  /** Classify OSC data for routing */
  classify(oscData: Uint8Array): OscCategory;

  /** Send OSC message with automatic routing */
  send(oscData: Uint8Array): boolean;

  /** Send directly to worklet without classification (no metrics) */
  sendDirect(oscData: Uint8Array): boolean;

  /** Send to prescheduler without classification */
  sendToPrescheduler(oscData: Uint8Array): boolean;

  /** Get current metrics */
  getMetrics(): OscChannelMetrics;

  /** Get and reset local metrics */
  getAndResetMetrics(): OscChannelMetrics;

  /** Close the channel */
  close(): void;

  /** Set the NTP time source for classification */
  set getCurrentNTP(fn: () => number);

  /** Transport mode */
  get mode(): TransportMode;

  /** Get transferable config for postMessage to a worker */
  get transferable(): OscChannelTransferable;

  /** Get list of transferable objects for postMessage transfer list */
  get transferList(): Transferable[];

  /** Reconstruct an OscChannel from transferred data (use in workers) */
  static fromTransferable(data: OscChannelTransferable): OscChannel;
}

// ============================================================================
// OSC Utilities (exported as `osc`)
// ============================================================================

export declare const osc: {
  /** Encode an OSC message */
  encodeMessage(address: string, args?: OscArg[]): Uint8Array;
  /** Encode an OSC bundle */
  encodeBundle(timeTag: NTPTimeTag, packets: OscBundlePacket[]): Uint8Array;
  /** Decode an OSC packet (message or bundle) */
  decode(data: Uint8Array | ArrayBuffer): OscMessage | OscBundle;
  /** Encode a single-message bundle (common case optimisation) */
  encodeSingleBundle(timeTag: NTPTimeTag, address: string, args?: OscArg[]): Uint8Array;
  /** Read the timetag from a bundle without full decode */
  readTimetag(bundleData: Uint8Array): number | null;
  /** Get current NTP time from performance.now() */
  ntpNow(): number;
  /** Seconds from 1900 to 1970 */
  NTP_EPOCH_OFFSET: number;
};

// ============================================================================
// SuperSonic
// ============================================================================

/** Options for sendOSC */
export interface SendOSCOptions {
  /** Session ID for cancellation */
  sessionId?: string;
  /** Run tag for cancellation */
  runTag?: string;
}

/**
 * SuperSonic - WebAssembly SuperCollider synthesis engine.
 *
 * Coordinates SharedArrayBuffer, WASM, AudioWorklet, and IO Workers
 * to run scsynth in the browser with low latency.
 */
export class SuperSonic {
  constructor(options?: SuperSonicOptions);

  // Static properties
  /** OSC encoding/decoding utilities */
  static osc: typeof osc;
  /** Get metrics schema with offsets, types, and UI layout */
  static getMetricsSchema(): MetricsSchema;
  /** Get schema describing the hierarchical node tree */
  static getTreeSchema(): Record<string, unknown>;
  /** Get schema describing the raw flat node tree */
  static getRawTreeSchema(): Record<string, unknown>;

  // Public getters
  /** Whether the engine has been initialised */
  get initialized(): boolean;
  /** Whether init() is currently in progress */
  get initializing(): boolean;
  /** The underlying AudioContext */
  get audioContext(): AudioContext | null;
  /** Transport mode ('sab' or 'postMessage') */
  get mode(): TransportMode;
  /** Buffer layout constants from the WASM build */
  get bufferConstants(): Record<string, number> | null;
  /** Ring buffer base offset in SharedArrayBuffer */
  get ringBufferBase(): number;
  /** The SharedArrayBuffer (SAB mode) or null (postMessage mode) */
  get sharedBuffer(): SharedArrayBuffer | null;
  /** AudioWorkletNode wrapper with connect/disconnect */
  get node(): {
    connect(...args: Parameters<AudioNode['connect']>): ReturnType<AudioNode['connect']>;
    disconnect(...args: Parameters<AudioNode['disconnect']>): void;
    readonly context: BaseAudioContext;
    readonly numberOfOutputs: number;
    readonly numberOfInputs: number;
    readonly channelCount: number;
    readonly input: AudioWorkletNode;
  } | null;
  /** The active OscChannel (internal, for advanced use) */
  get osc(): OscChannel | null;

  /** Map of loaded synthdef names to their binary data */
  loadedSynthDefs: Map<string, Uint8Array>;
  /** Boot timing statistics */
  bootStats: BootStats;

  // Event emitter
  /** Subscribe to an event. Returns an unsubscribe function. */
  on<E extends SuperSonicEvent>(event: E, callback: SuperSonicEventMap[E]): () => void;
  /** Unsubscribe from an event */
  off<E extends SuperSonicEvent>(event: E, callback: SuperSonicEventMap[E]): this;
  /** Subscribe to an event once (auto-unsubscribes after first call) */
  once<E extends SuperSonicEvent>(event: E, callback: SuperSonicEventMap[E]): this;
  /** Remove all listeners for an event, or all listeners if no event specified */
  removeAllListeners(event?: SuperSonicEvent): this;

  // Initialisation
  /** Initialise the engine (load WASM, create AudioContext, start worklet) */
  init(): Promise<void>;

  // Metrics
  /** Get current metrics as an object */
  getMetrics(): SuperSonicMetrics;
  /** Get metrics as a flat Uint32Array (zero-allocation, same reference each call) */
  getMetricsArray(): Uint32Array;
  /** Get a diagnostic snapshot with metrics, node tree, and memory info */
  getSnapshot(): Snapshot;

  // Timing
  /** Set clock offset for multi-system sync (seconds) */
  setClockOffset(offsetS: number): void;

  // Recovery
  /** Smart recovery - tries resume first, falls back to full reload */
  recover(): Promise<boolean>;
  /** Quick resume - just resumes AudioContext and resyncs timing */
  resume(): Promise<boolean>;
  /** Suspend the AudioContext and stop the drift timer */
  suspend(): Promise<void>;
  /** Full reload - destroys and recreates worklet/WASM, restores state */
  reload(): Promise<boolean>;

  // Node tree
  /** Get the node tree in flat format */
  getRawTree(): RawTree;
  /** Get the node tree in hierarchical format */
  getTree(): Tree;

  // Audio capture (SAB mode only)
  /** Start capturing audio output */
  startCapture(): void;
  /** Stop capturing and return the captured data */
  stopCapture(): Float32Array;
  /** Check if capture is currently enabled */
  isCaptureEnabled(): boolean;
  /** Get number of captured frames so far */
  getCaptureFrames(): number;
  /** Get max capture duration in seconds */
  getMaxCaptureDuration(): number;

  // OSC messaging
  /** Send an OSC message (high-level: address + args) */
  send(address: string, ...args: OscArg[]): Promise<void>;
  /** Send pre-encoded OSC bytes */
  sendOSC(oscData: Uint8Array | ArrayBuffer, options?: SendOSCOptions): Promise<void>;
  /** Cancel scheduled messages by run tag */
  cancelTag(runTag: string): void;
  /** Cancel scheduled messages by session ID */
  cancelSession(sessionId: string): void;
  /** Cancel scheduled messages by session ID and run tag */
  cancelSessionTag(sessionId: string, runTag: string): void;
  /** Cancel all scheduled messages */
  cancelAll(): void;
  /** Flush all pending messages from prescheduler and WASM scheduler */
  purge(): Promise<void>;

  /** Create an OscChannel for direct worker-to-worklet communication */
  createOscChannel(options?: { sourceId?: number }): OscChannel;

  // Asset loading
  /** Load a synthdef by name, path/URL, raw bytes, or File/Blob */
  loadSynthDef(source: string | ArrayBuffer | ArrayBufferView | Blob): Promise<LoadSynthDefResult>;
  /** Load multiple synthdefs by name (parallel) */
  loadSynthDefs(names: string[]): Promise<Record<string, { success: boolean; error?: string }>>;
  /** Load an audio sample into a buffer */
  loadSample(
    bufnum: number,
    source: string | ArrayBuffer | ArrayBufferView | Blob,
    startFrame?: number,
    numFrames?: number,
  ): Promise<LoadSampleResult>;
  /** Get info about all loaded buffers */
  getLoadedBuffers(): LoadedBufferInfo[];
  /** Wait for scsynth to process all pending commands */
  sync(syncId?: number): Promise<void>;

  // Info
  /** Get engine info (sample rate, memory, capabilities, version) */
  getInfo(): SuperSonicInfo;

  // Lifecycle
  /** Shut down the engine (preserves the instance for re-init) */
  shutdown(): Promise<void>;
  /** Destroy the engine completely (cannot be re-used) */
  destroy(): Promise<void>;
  /** Shutdown and re-initialise */
  reset(): Promise<void>;
}

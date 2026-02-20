// SPDX-License-Identifier: MIT OR GPL-3.0-or-later
// Copyright (c) 2025 Sam Aaron
//
// Type declarations for supersonic-scsynth
//
// For narrative documentation, usage examples, and comparison tables
// see docs/API.md

// ============================================================================
// OSC Types
// ============================================================================

/**
 * OSC argument types that can be sent in a message.
 *
 * Plain JS values are mapped to OSC types automatically:
 * - `number` (integer) → `i` (int32)
 * - `number` (float) → `f` (float32)
 * - `string` → `s`
 * - `boolean` → `T` / `F`
 * - `Uint8Array` / `ArrayBuffer` → `b` (blob)
 *
 * For 64-bit or timetag types, use the tagged object form:
 * @example
 * { type: 'int64', value: 9007199254740992n }
 * { type: 'double', value: 3.141592653589793 }
 * { type: 'timetag', value: ntpTimestamp }
 */
export type OscArg =
  | number
  | string
  | boolean
  | Uint8Array
  | ArrayBuffer
  | { type: 'int64'; value: number | bigint }
  | { type: 'double'; value: number }
  | { type: 'timetag'; value: number };

/**
 * Decoded OSC message as a plain array.
 *
 * The first element is always the address string, followed by zero or more arguments.
 *
 * @example
 * // A decoded /s_new message:
 * ["/s_new", "beep", 1001, 0, 1, "freq", 440]
 *
 * // Access parts:
 * const address = msg[0];  // "/s_new"
 * const args = msg.slice(1);  // ["beep", 1001, 0, 1, "freq", 440]
 */
export type OscMessage = [string, ...OscArg[]];

/** Decoded OSC bundle containing a timetag and nested packets. */
export interface OscBundle {
  /** NTP timestamp in seconds since 1900. */
  timeTag: number;
  /** Nested messages or bundles. */
  packets: (OscMessage | OscBundle)[];
}

/**
 * NTP timetag for bundle encoding.
 *
 * - `1` or `null` or `undefined` → immediate execution
 * - `number` → NTP seconds since 1900
 * - `[seconds, fraction]` → raw NTP pair (both uint32)
 */
export type NTPTimeTag = number | [number, number] | 1 | null | undefined;

/**
 * A packet that can be included in an OSC bundle.
 *
 * Accepts three formats:
 * @example
 * // Array format (preferred):
 * ["/s_new", "beep", 1001, 0, 1]
 *
 * // Object format (legacy):
 * { address: "/s_new", args: ["beep", 1001, 0, 1] }
 *
 * // Nested bundle:
 * { timeTag: ntpTime, packets: [ ["/n_set", 1001, "freq", 880] ] }
 */
export type OscBundlePacket =
  | OscMessage
  | { address: string; args?: OscArg[] }
  | { timeTag: NTPTimeTag; packets: OscBundlePacket[] };

// ============================================================================
// Configuration Types
// ============================================================================

/**
 * Transport mode for communication between JS and the AudioWorklet.
 *
 * - `'sab'` — SharedArrayBuffer: lowest latency, requires COOP/COEP headers
 * - `'postMessage'` — postMessage: works everywhere including CDN, slightly higher latency
 */
export type TransportMode = 'sab' | 'postMessage';

/**
 * scsynth engine options passed to World_New().
 *
 * These configure the internal SuperCollider engine. All values have sensible defaults.
 * Override via `new SuperSonic({ scsynthOptions: { ... } })`.
 */
export interface ScsynthOptions {
  /** Max audio buffers (1–65535). Default: 1024. */
  numBuffers?: number;
  /** Max synthesis nodes — synths + groups. Default: 1024. */
  maxNodes?: number;
  /** Max synth definitions. Default: 1024. */
  maxGraphDefs?: number;
  /** Max wire buffers for internal UGen routing. Default: 64. */
  maxWireBufs?: number;
  /** Audio bus channels for routing between synths. Default: 128. */
  numAudioBusChannels?: number;
  /** Hardware input channels. Default: 2 (stereo). */
  numInputBusChannels?: number;
  /** Hardware output channels (1–128). Default: 2 (stereo). */
  numOutputBusChannels?: number;
  /** Control bus channels for control-rate data. Default: 4096. */
  numControlBusChannels?: number;
  /** Audio buffer length — must be 128 (WebAudio API constraint). */
  bufLength?: 128;
  /** Real-time memory pool in KB for synthesis allocations. Default: 8192 (8MB). */
  realTimeMemorySize?: number;
  /** Random number generators per synth. Default: 64. */
  numRGens?: number;
  /** Clock source. Always false in SuperSonic (externally clocked by AudioWorklet). */
  realTime?: boolean;
  /** Memory locking — not applicable in browser. Default: false. */
  memoryLocking?: boolean;
  /** Auto-load synthdefs from disk: 0 or 1. Default: 0. */
  loadGraphDefs?: 0 | 1;
  /** Preferred sample rate. 0 = use AudioContext default (typically 48000). */
  preferredSampleRate?: number;
  /** Debug verbosity: 0 = quiet, 1 = errors, 2 = warnings, 3 = info, 4 = debug. */
  verbosity?: number;
}

/** Configuration for truncating activity log lines. */
export interface ActivityLineConfig {
  /** Default max line length for all activity types. Default: 200. */
  maxLineLength?: number;
  /** Override max line length for scsynth debug output. null = use maxLineLength. */
  scsynthMaxLineLength?: number | null;
  /** Override max line length for OSC in messages. null = use maxLineLength. */
  oscInMaxLineLength?: number | null;
  /** Override max line length for OSC out messages. null = use maxLineLength. */
  oscOutMaxLineLength?: number | null;
}

/**
 * Options for the SuperSonic constructor.
 *
 * Requires `baseURL` or both `coreBaseURL`/`workerBaseURL` and `wasmBaseURL`
 * so SuperSonic can locate its WASM binary and worker scripts.
 *
 * @example
 * // Simplest setup — all assets co-located:
 * const sonic = new SuperSonic({ baseURL: '/supersonic/dist/' });
 *
 * // CDN usage:
 * const sonic = new SuperSonic({
 *   baseURL: 'https://unpkg.com/supersonic-scsynth@0.48.0/dist/',
 *   mode: 'postMessage',  // CDN can't set COOP/COEP headers
 * });
 *
 * // Full control:
 * const sonic = new SuperSonic({
 *   mode: 'sab',
 *   coreBaseURL: '/core/',
 *   sampleBaseURL: '/samples/',
 *   synthdefBaseURL: '/synthdefs/',
 *   scsynthOptions: { numBuffers: 2048 },
 * });
 */
export interface SuperSonicOptions {
  /**
   * Transport mode.
   * - `'postMessage'` (default) — works everywhere, no special headers needed
   * - `'sab'` — lowest latency, requires Cross-Origin-Opener-Policy and Cross-Origin-Embedder-Policy headers
   */
  mode?: TransportMode;

  /** Convenience shorthand when all assets (WASM, workers, synthdefs, samples) are co-located. */
  baseURL?: string;
  /** Base URL for WASM and workers (supersonic-scsynth-core package). */
  coreBaseURL?: string;
  /** Base URL for worker scripts. Defaults to `coreBaseURL + 'workers/'`. */
  workerBaseURL?: string;
  /** Base URL for WASM files. Defaults to `coreBaseURL + 'wasm/'`. */
  wasmBaseURL?: string;
  /** Full URL to the WASM binary. Overrides wasmBaseURL. */
  wasmUrl?: string;
  /** Full URL to the AudioWorklet script. Overrides workerBaseURL. */
  workletUrl?: string;

  /** Base URL for audio sample files (used by {@link SuperSonic.loadSample}). */
  sampleBaseURL?: string;
  /** Base URL for synthdef files (used by {@link SuperSonic.loadSynthDef}). */
  synthdefBaseURL?: string;

  /** Provide your own AudioContext instead of letting SuperSonic create one. */
  audioContext?: AudioContext;
  /** Options passed to `new AudioContext()`. Ignored if `audioContext` is provided. */
  audioContextOptions?: AudioContextOptions;
  /** Auto-connect the AudioWorkletNode to the AudioContext destination. Default: true. */
  autoConnect?: boolean;

  /** Engine options passed to scsynth World_New(). */
  scsynthOptions?: ScsynthOptions;

  /** How often to snapshot metrics/tree in postMessage mode (ms). */
  snapshotIntervalMs?: number;
  /** Max pending events in the JS prescheduler. Default: 65536. */
  preschedulerCapacity?: number;
  /** Bundles within this many ms of now bypass the prescheduler. Default: 500. */
  bypassLookaheadMs?: number;

  /** Enable all debug console logging. Default: false. */
  debug?: boolean;
  /** Log scsynth debug output to console. Default: false. */
  debugScsynth?: boolean;
  /** Log incoming OSC messages to console. Default: false. */
  debugOscIn?: boolean;
  /** Log outgoing OSC messages to console. Default: false. */
  debugOscOut?: boolean;

  /** Line length limits for activity events emitted to listeners. */
  activityEvent?: ActivityLineConfig;
  /** Line length limits for activity console.log output. */
  activityConsoleLog?: ActivityLineConfig;

  /** Max fetch retries when loading assets. Default: 3. */
  fetchMaxRetries?: number;
  /** Base delay between retries in ms (exponential backoff). Default: 1000. */
  fetchRetryDelay?: number;
}

// ============================================================================
// Metrics Types
// ============================================================================

/**
 * Complete metrics snapshot returned by {@link SuperSonic.getMetrics}.
 *
 * All values are numbers. Counter metrics are cumulative; gauge metrics
 * reflect current state. Use {@link SuperSonic.getMetricsSchema} for
 * descriptions, units, and UI layout metadata.
 */
export interface SuperSonicMetrics {
  // scsynth metrics
  /** Audio process() calls (cumulative). */
  scsynthProcessCount: number;
  /** OSC messages processed by scsynth. */
  scsynthMessagesProcessed: number;
  /** Messages dropped by scsynth (scheduler queue full). */
  scsynthMessagesDropped: number;
  /** Current scsynth scheduler queue depth. */
  scsynthSchedulerDepth: number;
  /** Peak scsynth scheduler queue depth (high water mark). */
  scsynthSchedulerPeakDepth: number;
  /** Messages dropped from scsynth scheduler queue. */
  scsynthSchedulerDropped: number;
  /** Messages lost in transit from JS to scsynth. */
  scsynthSequenceGaps: number;
  /** WASM execution errors in audio worklet. */
  scsynthWasmErrors: number;
  /** Bundles executed after their scheduled time. */
  scsynthSchedulerLates: number;

  // Prescheduler metrics
  /** Events waiting in JS prescheduler queue. */
  preschedulerPending: number;
  /** Peak pending events. */
  preschedulerPendingPeak: number;
  /** Bundles added to prescheduler. */
  preschedulerBundlesScheduled: number;
  /** Events sent from prescheduler to worklet. */
  preschedulerDispatched: number;
  /** Bundles cancelled before dispatch. */
  preschedulerEventsCancelled: number;
  /** Smallest time gap between dispatch and execution (ms). 0xFFFFFFFF = no data yet. */
  preschedulerMinHeadroomMs: number;
  /** Bundles dispatched after their scheduled time. */
  preschedulerLates: number;
  /** Ring buffer write retries that succeeded. */
  preschedulerRetriesSucceeded: number;
  /** Ring buffer write retries that failed. */
  preschedulerRetriesFailed: number;
  /** Current retry queue size. */
  preschedulerRetryQueueSize: number;
  /** Peak retry queue size. */
  preschedulerRetryQueuePeak: number;
  /** Total messages that needed retry. */
  preschedulerMessagesRetried: number;
  /** Total dispatch cycles. */
  preschedulerTotalDispatches: number;
  /** Messages sent directly, bypassing prescheduler (aggregate). */
  preschedulerBypassed: number;
  /** Maximum lateness at prescheduler (ms). */
  preschedulerMaxLateMs: number;

  // OSC Out metrics
  /** OSC messages sent from JS to scsynth. */
  oscOutMessagesSent: number;
  /** Total bytes sent from JS to scsynth. */
  oscOutBytesSent: number;

  // OSC In metrics
  /** OSC replies received from scsynth. */
  oscInMessagesReceived: number;
  /** Total bytes received from scsynth. */
  oscInBytesReceived: number;
  /** Replies lost in transit from scsynth to JS. */
  oscInMessagesDropped: number;
  /** Corrupted messages detected from scsynth. */
  oscInCorrupted: number;

  // Debug metrics
  /** Debug messages received from scsynth. */
  debugMessagesReceived: number;
  /** Debug bytes received from scsynth. */
  debugBytesReceived: number;

  // Ring buffer usage
  /** Bytes used in IN ring buffer (JS → scsynth). */
  inBufferUsedBytes: number;
  /** Bytes used in OUT ring buffer (scsynth → JS). */
  outBufferUsedBytes: number;
  /** Bytes used in DEBUG ring buffer. */
  debugBufferUsedBytes: number;
  /** Peak bytes used in IN ring buffer. */
  inBufferPeakBytes: number;
  /** Peak bytes used in OUT ring buffer. */
  outBufferPeakBytes: number;
  /** Peak bytes used in DEBUG ring buffer. */
  debugBufferPeakBytes: number;

  // Bypass categories
  /** Plain OSC messages (not bundles) that bypassed prescheduler. */
  bypassNonBundle: number;
  /** Bundles with timetag 0 or 1 that bypassed prescheduler. */
  bypassImmediate: number;
  /** Bundles within lookahead threshold that bypassed prescheduler. */
  bypassNearFuture: number;
  /** Late bundles that bypassed prescheduler. */
  bypassLate: number;

  // scsynth late timing diagnostics
  /** Maximum lateness observed in scsynth scheduler (ms). */
  scsynthSchedulerMaxLateMs: number;
  /** Most recent late magnitude in scsynth scheduler (ms). */
  scsynthSchedulerLastLateMs: number;
  /** Process count when last scsynth late occurred. */
  scsynthSchedulerLastLateTick: number;

  /** SAB mode only: optimistic direct writes that fell back to prescheduler. */
  ringBufferDirectWriteFails: number;

  // Context metrics (main thread only)
  /** Clock drift between AudioContext and wall clock (ms, signed). */
  driftOffsetMs: number;
  /** Clock offset for multi-system sync (ms, signed). */
  clockOffsetMs: number;
  /** AudioContext state as enum index: 0=unknown, 1=running, 2=suspended, 3=closed, 4=interrupted. */
  audioContextState: number;
  /** Buffer pool bytes currently in use. */
  bufferPoolUsedBytes: number;
  /** Buffer pool bytes available. */
  bufferPoolAvailableBytes: number;
  /** Total buffer pool allocations. */
  bufferPoolAllocations: number;
  /** Number of loaded synthdefs. */
  loadedSynthDefs: number;
  /** Maximum scsynth scheduler queue size (compile-time constant). */
  scsynthSchedulerCapacity: number;
  /** Maximum pending events in JS prescheduler. */
  preschedulerCapacity: number;
  /** IN ring buffer capacity (bytes). */
  inBufferCapacity: number;
  /** OUT ring buffer capacity (bytes). */
  outBufferCapacity: number;
  /** DEBUG ring buffer capacity (bytes). */
  debugBufferCapacity: number;
  /** Transport mode as enum index: 0=sab, 1=postMessage. */
  mode: number;
}

/** Schema entry describing a single metric field. */
export interface MetricDefinition {
  /** Offset into the flat metrics Uint32Array. */
  offset: number;
  /** Metric type: counter (cumulative), gauge (current), constant, or enum. */
  type: 'counter' | 'gauge' | 'constant' | 'enum';
  /** Unit of measurement. */
  unit?: string;
  /** Whether the value should be read as signed int32. */
  signed?: boolean;
  /** Enum value names (for type 'enum'). */
  values?: string[];
  /** Human-readable description. */
  description: string;
}

/**
 * Metrics schema returned by {@link SuperSonic.getMetricsSchema}.
 *
 * Contains metric definitions with array offsets (for zero-allocation reading),
 * a declarative UI layout for rendering metrics panels, and sentinel values.
 */
export interface MetricsSchema {
  /** Each key maps to offset, type, unit, and description for the merged Uint32Array. */
  metrics: Record<keyof SuperSonicMetrics, MetricDefinition>;
  /** Panel structure for rendering a metrics UI. Used by `<supersonic-metrics>`. */
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
  /** Magic values used in the metrics array. */
  sentinels: {
    /** Value of preschedulerMinHeadroomMs before any data arrives. */
    HEADROOM_UNSET: number;
  };
}

// ============================================================================
// Node Tree Types
// ============================================================================

/**
 * A node in the hierarchical synth tree.
 *
 * Groups contain children; synths are leaves.
 */
export interface TreeNode {
  /** Unique node ID. */
  id: number;
  /** `'group'` for groups, `'synth'` for synth nodes. */
  type: 'group' | 'synth';
  /** SynthDef name (synths only, empty string for groups). */
  defName: string;
  /** Child nodes (groups only, empty array for synths). */
  children: TreeNode[];
}

/**
 * Hierarchical node tree returned by {@link SuperSonic.getTree}.
 *
 * @example
 * const tree = sonic.getTree();
 * console.log(tree.root.children); // top-level groups and synths
 * console.log(tree.nodeCount);     // total nodes in the tree
 */
export interface Tree {
  /** Total number of nodes. */
  nodeCount: number;
  /** Increments on any tree change — useful for detecting updates. */
  version: number;
  /** Nodes that exceeded mirror capacity (tree may be incomplete if > 0). */
  droppedCount: number;
  /** Root group (always id 0). */
  root: TreeNode;
}

/** A node in the flat (raw) tree representation with linkage pointers. */
export interface RawTreeNode {
  /** Unique node ID. */
  id: number;
  /** Parent node ID (-1 for root). */
  parentId: number;
  /** true if group, false if synth. */
  isGroup: boolean;
  /** Previous sibling node ID (-1 if none). */
  prevId: number;
  /** Next sibling node ID (-1 if none). */
  nextId: number;
  /** First child node ID (groups only, -1 if empty). */
  headId: number;
  /** SynthDef name (synths only, empty string for groups). */
  defName: string;
}

/**
 * Flat node tree returned by {@link SuperSonic.getRawTree}.
 *
 * Contains all nodes as a flat array with parent/sibling linkage pointers.
 * More efficient than the hierarchical tree for serialization or custom rendering.
 */
export interface RawTree {
  /** Total number of nodes. */
  nodeCount: number;
  /** Increments on any tree change. */
  version: number;
  /** Nodes that exceeded mirror capacity. */
  droppedCount: number;
  /** Flat array of all nodes. */
  nodes: RawTreeNode[];
}

// ============================================================================
// Info & Snapshot Types
// ============================================================================

/** Engine info returned by {@link SuperSonic.getInfo}. */
export interface SuperSonicInfo {
  /** AudioContext sample rate (e.g. 48000). */
  sampleRate: number;
  /** Max audio buffers configured. */
  numBuffers: number;
  /** Total WebAssembly memory in bytes. */
  totalMemory: number;
  /** WASM heap size available for scsynth allocations. */
  wasmHeapSize: number;
  /** Audio sample buffer pool size in bytes. */
  bufferPoolSize: number;
  /** Time taken to boot in ms, or null if not yet booted. */
  bootTimeMs: number | null;
  /** Browser capability detection results. */
  capabilities: {
    audioWorklet: boolean;
    sharedArrayBuffer: boolean;
    crossOriginIsolated: boolean;
    atomics: boolean;
    webWorker: boolean;
  };
  /** scsynth WASM version string, or null if not yet initialised. */
  version: string | null;
}

/**
 * Diagnostic snapshot returned by {@link SuperSonic.getSnapshot}.
 *
 * Captures metrics with descriptions, the current node tree, and JS heap
 * memory info. Useful for bug reports and debugging timing issues.
 */
export interface Snapshot {
  /** ISO 8601 timestamp when the snapshot was taken. */
  timestamp: string;
  /** All metrics with their current values and descriptions. */
  metrics: Record<string, { value: number; description?: string }>;
  /** Current node tree in flat format. */
  nodeTree: RawTree;
  /** JS heap memory info (Chrome only, null in other browsers). */
  memory: {
    usedJSHeapSize: number;
    totalJSHeapSize: number;
    jsHeapSizeLimit: number;
  } | null;
}

/** Info about a loaded audio buffer, returned by {@link SuperSonic.getLoadedBuffers}. */
export interface LoadedBufferInfo {
  /** Buffer slot number. */
  bufnum: number;
  /** Number of sample frames. */
  numFrames: number;
  /** Number of channels. */
  numChannels: number;
  /** Sample rate in Hz. */
  sampleRate: number;
  /** Original source path/URL, or null. */
  source: string | null;
  /** Duration in seconds. */
  duration: number;
}

/** Result from {@link SuperSonic.loadSynthDef}. */
export interface LoadSynthDefResult {
  /** Extracted SynthDef name. */
  name: string;
  /** Size of the synthdef binary in bytes. */
  size: number;
}

/** Result from {@link SuperSonic.loadSample}. */
export interface LoadSampleResult {
  /** Buffer slot the sample was loaded into. */
  bufnum: number;
  /** Number of sample frames loaded. */
  numFrames: number;
  /** Number of channels. */
  numChannels: number;
  /** Sample rate in Hz. */
  sampleRate: number;
}

/** Boot timing statistics. */
export interface BootStats {
  /** Timestamp when init() started (performance.now()), or null. */
  initStartTime: number | null;
  /** Total boot duration in ms, or null if not yet booted. */
  initDuration: number | null;
}

// ============================================================================
// Event Types
// ============================================================================

/**
 * Map of event names to their callback signatures.
 *
 * Used with {@link SuperSonic.on}, {@link SuperSonic.off}, and {@link SuperSonic.once}
 * for type-safe event subscriptions.
 *
 * @example
 * sonic.on('message', (msg) => {
 *   // msg is typed as OscMessage — [address, ...args]
 *   if (msg[0] === '/n_go') {
 *     console.log('Node started:', msg[1]);
 *   }
 * });
 *
 * sonic.on('setup', async () => {
 *   // Runs after init(), before 'ready'. Set up groups and FX chains here.
 *   await sonic.send('/g_new', 1, 0, 0);
 * });
 */
export interface SuperSonicEventMap {
  /**
   * Fired after init completes, before `'ready'`.
   * Use for setting up groups, FX chains, and bus routing.
   * Can be async — init waits for all setup handlers to resolve.
   */
  'setup': () => void | Promise<void>;

  /** Fired when the engine is fully booted and ready to receive messages. */
  'ready': (data: { capabilities: SuperSonicInfo['capabilities']; bootStats: BootStats }) => void;

  /**
   * Decoded OSC message received from scsynth.
   * Messages are plain arrays: `[address, ...args]`.
   */
  'message': (msg: OscMessage) => void;

  /** Raw OSC bytes received from scsynth (before decoding). Includes NTP timestamps for timing analysis. */
  'message:raw': (data: { oscData: Uint8Array; sequence: number; timestamp: number; scheduledTime: number | null }) => void;

  /** Fired when an OSC message is sent to scsynth. Includes source worker ID, sequence number, and NTP timestamps. */
  'message:sent': (data: { oscData: Uint8Array; sourceId: number; sequence: number; timestamp: number; scheduledTime: number | null }) => void;

  /** Debug text output from scsynth (e.g. synthdef compilation messages). */
  'debug': (msg: { text: string }) => void;

  /** Error from any component (worklet, transport, workers). */
  'error': (error: Error) => void;

  /** Engine is shutting down. */
  'shutdown': () => void;

  /** Engine has been destroyed. */
  'destroy': () => void;

  /** Audio resumed after a suspend (AudioContext was re-started). */
  'resumed': () => void;

  /** Full reload started (worklet and WASM will be recreated). */
  'reload:start': () => void;

  /** Full reload completed. */
  'reload:complete': (data: { success: boolean }) => void;

  /** AudioContext state changed. */
  'audiocontext:statechange': (data: { state: AudioContextState }) => void;

  /** AudioContext was suspended (e.g. tab backgrounded, autoplay policy). */
  'audiocontext:suspended': () => void;

  /** AudioContext resumed to 'running' state. */
  'audiocontext:resumed': () => void;

  /** AudioContext was interrupted (iOS-specific). */
  'audiocontext:interrupted': () => void;

  /** An asset (WASM, synthdef, sample) started loading. */
  'loading:start': (data: { type: string; name: string }) => void;

  /** An asset finished loading. */
  'loading:complete': (data: { type: string; name: string; size: number }) => void;
}

/** Union of all event names. */
export type SuperSonicEvent = keyof SuperSonicEventMap;

// ============================================================================
// OscChannel
// ============================================================================

/**
 * Classification category for OSC message routing.
 *
 * - `'nonBundle'` — plain message (not a bundle), sent directly
 * - `'immediate'` — bundle with timetag 0 or 1, sent directly
 * - `'nearFuture'` — bundle within the bypass lookahead threshold, sent directly
 * - `'late'` — bundle past its scheduled time, sent directly
 * - `'farFuture'` — bundle beyond the lookahead threshold, routed to the prescheduler
 */
export type OscCategory = 'nonBundle' | 'immediate' | 'nearFuture' | 'late' | 'farFuture';

/** OscChannel metrics counters. */
export interface OscChannelMetrics {
  messagesSent: number;
  bytesSent: number;
  nonBundle: number;
  immediate: number;
  nearFuture: number;
  late: number;
  bypassed: number;
}

/** Transferable config for SAB mode OscChannel. */
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

/** Transferable config for postMessage mode OscChannel. */
export interface OscChannelPMTransferable {
  mode: 'postMessage';
  port: MessagePort;
  preschedulerPort: MessagePort | null;
  bypassLookaheadS: number;
  sourceId: number;
  blocking: boolean;
}

/** Opaque config produced by `channel.transferable` and consumed by `OscChannel.fromTransferable()`. */
export type OscChannelTransferable = OscChannelSABTransferable | OscChannelPMTransferable;

/**
 * OscChannel — unified dispatch for sending OSC to the AudioWorklet.
 *
 * Obtain a channel via {@link SuperSonic.createOscChannel} on the main thread,
 * then transfer it to a Web Worker for direct communication with the AudioWorklet.
 *
 * @example
 * // Main thread: create and transfer to worker
 * const channel = sonic.createOscChannel();
 * myWorker.postMessage(
 *   { channel: channel.transferable },
 *   channel.transferList,
 * );
 *
 * // Inside worker: reconstruct and send
 * import { OscChannel } from 'supersonic-scsynth/osc-channel';
 * const channel = OscChannel.fromTransferable(event.data.channel);
 * channel.send(oscBytes);
 */
export class OscChannel {
  /**
   * Classify an OSC message to determine its routing.
   * @param oscData - Encoded OSC bytes
   */
  classify(oscData: Uint8Array): OscCategory;

  /**
   * Send an OSC message with automatic routing.
   *
   * Classifies the message and routes it:
   * - bypass categories → sent directly to the AudioWorklet
   * - far-future bundles → routed to the prescheduler for timed dispatch
   *
   * @param oscData - Encoded OSC bytes
   * @returns true if sent successfully
   */
  send(oscData: Uint8Array): boolean;

  /**
   * Send directly to worklet without classification or metrics tracking.
   * @param oscData - Encoded OSC bytes
   * @returns true if sent successfully
   */
  sendDirect(oscData: Uint8Array): boolean;

  /**
   * Send to prescheduler without classification.
   * @param oscData - Encoded OSC bytes
   * @returns true if sent successfully
   */
  sendToPrescheduler(oscData: Uint8Array): boolean;

  /** Get current metrics snapshot. */
  getMetrics(): OscChannelMetrics;

  /** Get and reset local metrics (for periodic reporting). */
  getAndResetMetrics(): OscChannelMetrics;

  /** Close the channel and release its ports. */
  close(): void;

  /** Set the NTP time source for classification (used in AudioWorklet context). */
  set getCurrentNTP(fn: () => number);

  /** Transport mode this channel is using. */
  get mode(): TransportMode;

  /**
   * Serializable config for transferring this channel to a worker via postMessage.
   *
   * @example
   * worker.postMessage({ ch: channel.transferable }, channel.transferList);
   */
  get transferable(): OscChannelTransferable;

  /**
   * Array of transferable objects (MessagePorts) for the postMessage transfer list.
   *
   * @example
   * worker.postMessage({ ch: channel.transferable }, channel.transferList);
   */
  get transferList(): Transferable[];

  /**
   * Reconstruct an OscChannel from data received via postMessage in a worker.
   *
   * @param data - The transferable config from `channel.transferable`
   * @example
   * // In a Web Worker:
   * self.onmessage = (e) => {
   *   const channel = OscChannel.fromTransferable(e.data.ch);
   *   channel.send(oscBytes);
   * };
   */
  static fromTransferable(data: OscChannelTransferable): OscChannel;
}

// ============================================================================
// OSC Utilities (exported as `osc`)
// ============================================================================

/**
 * Static OSC encoding/decoding utilities.
 *
 * Available as `SuperSonic.osc` or via the named `osc` export.
 * All encode methods return independent copies safe to store or transfer.
 *
 * @example
 * import { SuperSonic } from 'supersonic-scsynth';
 *
 * // Encode a message
 * const msg = SuperSonic.osc.encodeMessage('/s_new', ['beep', 1001, 0, 1]);
 *
 * // Encode a timed bundle
 * const time = SuperSonic.osc.ntpNow() + 0.5; // 500ms from now
 * const bundle = SuperSonic.osc.encodeBundle(time, [
 *   ['/s_new', 'beep', 1001, 0, 1, 'freq', 440],
 *   ['/s_new', 'beep', 1002, 0, 1, 'freq', 660],
 * ]);
 *
 * // Decode incoming data
 * const decoded = SuperSonic.osc.decode(rawBytes);
 */
export declare const osc: {
  /**
   * Encode an OSC message.
   * @param address - OSC address pattern (e.g. `'/s_new'`)
   * @param args - Arguments to encode
   * @returns Encoded OSC bytes (independent copy)
   *
   * @example
   * osc.encodeMessage('/s_new', ['beep', 1001, 0, 1, 'freq', 440])
   */
  encodeMessage(address: string, args?: OscArg[]): Uint8Array;

  /**
   * Encode an OSC bundle with multiple packets.
   * @param timeTag - NTP timestamp, `1` for immediate, or `[seconds, fraction]` pair
   * @param packets - Array of messages or nested bundles
   * @returns Encoded bundle bytes (independent copy)
   *
   * @example
   * const time = osc.ntpNow() + 1.0; // 1 second from now
   * osc.encodeBundle(time, [
   *   ['/n_set', 1001, 'freq', 880],
   *   ['/n_set', 1001, 'amp', 0.5],
   * ])
   */
  encodeBundle(timeTag: NTPTimeTag, packets: OscBundlePacket[]): Uint8Array;

  /**
   * Decode an OSC packet (message or bundle).
   * @param data - Raw OSC bytes
   * @returns Decoded message `[address, ...args]` or bundle `{ timeTag, packets }`
   */
  decode(data: Uint8Array | ArrayBuffer): OscMessage | OscBundle;

  /**
   * Encode a single-message bundle (common case optimisation).
   *
   * Equivalent to `encodeBundle(timeTag, [[address, ...args]])` but faster.
   *
   * @param timeTag - NTP timestamp
   * @param address - OSC address pattern
   * @param args - Arguments to encode
   * @returns Encoded bundle bytes (independent copy)
   */
  encodeSingleBundle(timeTag: NTPTimeTag, address: string, args?: OscArg[]): Uint8Array;

  /**
   * Read the timetag from a bundle without fully decoding it.
   * @param bundleData - Raw bundle bytes (must be at least 16 bytes)
   * @returns NTP timetag as `{ ntpSeconds, ntpFraction }` (both uint32), or null if data is too short
   */
  readTimetag(bundleData: Uint8Array): { ntpSeconds: number; ntpFraction: number } | null;

  /**
   * Get the current time as an NTP timestamp (seconds since 1900).
   *
   * Use this to schedule bundles relative to now:
   * @example
   * const halfSecondFromNow = osc.ntpNow() + 0.5;
   */
  ntpNow(): number;

  /** Seconds between NTP epoch (1900) and Unix epoch (1970): `2208988800`. */
  NTP_EPOCH_OFFSET: number;
};

// ============================================================================
// SuperSonic
// ============================================================================

/** Options for {@link SuperSonic.sendOSC}. */
export interface SendOSCOptions {
  /** Session ID for cancellation via {@link SuperSonic.cancelSession}. */
  sessionId?: string;
  /** Run tag for cancellation via {@link SuperSonic.cancelTag}. */
  runTag?: string;
}

/**
 * SuperSonic — WebAssembly SuperCollider synthesis engine for the browser.
 *
 * Coordinates WASM, AudioWorklet, SharedArrayBuffer, and IO Workers to run
 * scsynth with low latency inside a web page.
 *
 * @example
 * import { SuperSonic } from 'supersonic-scsynth';
 *
 * const sonic = new SuperSonic({ baseURL: '/dist/' });
 *
 * sonic.on('setup', async () => {
 *   await sonic.loadSynthDef('beep');
 * });
 *
 * sonic.on('message', (msg) => {
 *   console.log('OSC from scsynth:', msg[0], msg.slice(1));
 * });
 *
 * await sonic.init();
 * sonic.send('/s_new', 'beep', 1001, 0, 1, 'freq', 440);
 */
export class SuperSonic {
  /**
   * Create a new SuperSonic instance.
   *
   * Does not start the engine — call {@link init} to boot.
   *
   * @param options - Configuration options. Requires `baseURL` or both `coreBaseURL`/`workerBaseURL` and `wasmBaseURL`.
   * @throws If URL configuration is missing or scsynthOptions are invalid.
   *
   * @example
   * const sonic = new SuperSonic({
   *   baseURL: '/supersonic/dist/',
   *   mode: 'postMessage',
   *   scsynthOptions: { numBuffers: 2048 },
   * });
   */
  constructor(options?: SuperSonicOptions);

  // ──────────────────────────────────────────────────────────────────────────
  // Static
  // ──────────────────────────────────────────────────────────────────────────

  /**
   * OSC encoding/decoding utilities.
   *
   * @example
   * const msg = SuperSonic.osc.encodeMessage('/s_new', ['beep', 1001]);
   * const decoded = SuperSonic.osc.decode(msg);
   */
  static osc: typeof osc;

  /**
   * Get the metrics schema describing all available metrics.
   *
   * Includes array offsets for zero-allocation reading via {@link getMetricsArray},
   * metric types/units/descriptions, and a declarative UI layout used by the
   * `<supersonic-metrics>` web component.
   */
  static getMetricsSchema(): MetricsSchema;

  /** Get schema describing the hierarchical node tree structure. */
  static getTreeSchema(): Record<string, unknown>;

  /** Get schema describing the raw flat node tree structure. */
  static getRawTreeSchema(): Record<string, unknown>;

  // ──────────────────────────────────────────────────────────────────────────
  // State
  // ──────────────────────────────────────────────────────────────────────────

  /** Whether the engine has completed initialisation. */
  get initialized(): boolean;

  /** Whether {@link init} is currently in progress. */
  get initializing(): boolean;

  /**
   * The underlying AudioContext.
   *
   * Available after {@link init}. Use this to read `sampleRate`, `currentTime`,
   * or to connect additional audio nodes.
   */
  get audioContext(): AudioContext | null;

  /** Active transport mode (`'sab'` or `'postMessage'`). */
  get mode(): TransportMode;

  /** Buffer layout constants from the WASM build. Mostly internal. */
  get bufferConstants(): Record<string, number> | null;

  /** Ring buffer base offset in SharedArrayBuffer. Internal. */
  get ringBufferBase(): number;

  /** The SharedArrayBuffer (SAB mode) or null (postMessage mode). Internal. */
  get sharedBuffer(): SharedArrayBuffer | null;

  /** NTP time (seconds since 1900) when the AudioContext started. Use to compute relative times: `event.timestamp - sonic.initTime`. */
  get initTime(): number;

  /**
   * AudioWorkletNode wrapper for custom audio routing.
   *
   * Use `node.connect()` / `node.disconnect()` to route audio.
   * Use `node.input` to connect external audio sources into scsynth.
   *
   * @example
   * // Route scsynth output through an AnalyserNode:
   * sonic.node.disconnect();
   * sonic.node.connect(analyser);
   * analyser.connect(sonic.audioContext.destination);
   */
  get node(): {
    connect(...args: Parameters<AudioNode['connect']>): ReturnType<AudioNode['connect']>;
    disconnect(...args: Parameters<AudioNode['disconnect']>): void;
    readonly context: BaseAudioContext;
    readonly numberOfOutputs: number;
    readonly numberOfInputs: number;
    readonly channelCount: number;
    /** The underlying AudioWorkletNode — connect external sources here. */
    readonly input: AudioWorkletNode;
  } | null;

  /** The internal OscChannel used by the main thread. Advanced use only. */
  get osc(): OscChannel | null;

  /** Map of loaded SynthDef names to their binary data. */
  loadedSynthDefs: Map<string, Uint8Array>;

  /** Boot timing statistics. */
  bootStats: BootStats;

  // ──────────────────────────────────────────────────────────────────────────
  // Events
  // ──────────────────────────────────────────────────────────────────────────

  /**
   * Subscribe to an event.
   *
   * @param event - Event name
   * @param callback - Handler function (type-checked per event)
   * @returns Unsubscribe function — call it to remove the listener
   *
   * @example
   * const unsub = sonic.on('message', (msg) => {
   *   console.log(msg[0], msg.slice(1));
   * });
   *
   * // Later:
   * unsub();
   */
  on<E extends SuperSonicEvent>(event: E, callback: SuperSonicEventMap[E]): () => void;

  /**
   * Unsubscribe from an event.
   * @param event - Event name
   * @param callback - The same function reference passed to {@link on}
   */
  off<E extends SuperSonicEvent>(event: E, callback: SuperSonicEventMap[E]): this;

  /**
   * Subscribe to an event once. The handler is automatically removed after the first call.
   * @param event - Event name
   * @param callback - Handler function
   */
  once<E extends SuperSonicEvent>(event: E, callback: SuperSonicEventMap[E]): this;

  /**
   * Remove all listeners for an event, or all listeners entirely.
   * @param event - Event name, or omit to remove everything
   */
  removeAllListeners(event?: SuperSonicEvent): this;

  // ──────────────────────────────────────────────────────────────────────────
  // Lifecycle
  // ──────────────────────────────────────────────────────────────────────────

  /**
   * Initialise the engine.
   *
   * Loads the WASM binary, creates the AudioContext and AudioWorklet,
   * starts IO workers, and syncs timing. Emits `'setup'` then `'ready'`
   * when complete.
   *
   * Safe to call multiple times — subsequent calls are no-ops.
   *
   * @throws If required browser features are missing or WASM fails to load.
   *
   * @example
   * await sonic.init();
   * // Engine is now ready to send/receive OSC
   */
  init(): Promise<void>;

  /**
   * Shut down the engine. The instance can be re-initialised with {@link init}.
   *
   * Closes the AudioContext, terminates workers, and releases memory.
   * Emits `'shutdown'`.
   */
  shutdown(): Promise<void>;

  /**
   * Destroy the engine completely. The instance cannot be re-used.
   *
   * Calls {@link shutdown} then clears the WASM cache and all event listeners.
   * Emits `'destroy'`.
   */
  destroy(): Promise<void>;

  /**
   * Shutdown and immediately re-initialise.
   *
   * Equivalent to `await sonic.shutdown(); await sonic.init();`
   */
  reset(): Promise<void>;

  // ──────────────────────────────────────────────────────────────────────────
  // Recovery
  // ──────────────────────────────────────────────────────────────────────────

  /**
   * Smart recovery — tries a quick resume first, falls back to full reload.
   *
   * Use when you're not sure if the worklet is still alive (e.g. returning
   * from a long background period).
   *
   * @returns true if audio is running after recovery
   */
  recover(): Promise<boolean>;

  /**
   * Quick resume — calls {@link purge} to flush stale messages, resumes
   * AudioContext, and resyncs timing.
   *
   * Memory, node tree, and loaded synthdefs are preserved. Does not emit `'setup'`.
   * Use when you know the worklet is still running (e.g. tab was briefly backgrounded).
   *
   * @returns true if the worklet is running after resume
   */
  resume(): Promise<boolean>;

  /**
   * Suspend the AudioContext and stop the drift timer.
   *
   * The worklet remains loaded but audio processing stops.
   * Use {@link resume} or {@link recover} to restart.
   */
  suspend(): Promise<void>;

  /**
   * Full reload — destroys and recreates the worklet and WASM, then restores
   * all previously loaded synthdefs and audio buffers.
   *
   * Emits `'setup'` so you can rebuild groups, FX chains, and bus routing.
   * Use when the worklet was killed (e.g. long background, browser reclaimed memory).
   *
   * @returns true if reload succeeded
   */
  reload(): Promise<boolean>;

  // ──────────────────────────────────────────────────────────────────────────
  // OSC Messaging
  // ──────────────────────────────────────────────────────────────────────────

  /**
   * Send an OSC message to scsynth.
   *
   * This is the primary way to communicate with the engine. Arguments are
   * automatically encoded to OSC format. Synchronous for all commands except
   * buffer allocation (`/b_alloc`, `/b_allocRead`, `/b_allocReadChannel`,
   * `/b_allocFile`) which are queued and processed in the background.
   * Use {@link sync} after buffer commands to ensure they complete.
   *
   * @param address - OSC address pattern (e.g. `'/s_new'`, `'/n_set'`)
   * @param args - Message arguments
   * @throws If the engine is not initialised
   * @throws If the address is a blocked command (e.g. `/d_load`, `/b_read`)
   *
   * @example
   * // Create a synth
   * sonic.send('/s_new', 'beep', 1001, 0, 1, 'freq', 440);
   *
   * // Set a control
   * sonic.send('/n_set', 1001, 'freq', 880);
   *
   * // Free a synth
   * sonic.send('/n_free', 1001);
   *
   * // Send a synthdef as raw bytes
   * sonic.send('/d_recv', synthdefBytes);
   *
   * // Buffer commands are processed in the background; use sync() after them:
   * sonic.send('/b_alloc', 0, 44100, 1);
   * await sonic.sync(); // waits for buffer allocation to complete
   *
   * // Blocked commands throw with a helpful message:
   * sonic.send('/d_load', 'beep');
   * // Error: /d_load is not supported. Use loadSynthDef() or send /d_recv instead.
   */
  send(address: string, ...args: OscArg[]): void;

  /**
   * Send pre-encoded OSC bytes to scsynth.
   *
   * Use this when you've already encoded the message (e.g. via `SuperSonic.osc.encodeMessage`)
   * or when sending from a worker that produces raw OSC. Sends bytes as-is without
   * rewriting — buffer allocation commands (`/b_alloc*`) are not transformed.
   * Use {@link send} for buffer commands so they get rewritten to `/b_allocPtr`.
   *
   * @param oscData - Encoded OSC message or bundle bytes
   * @param options - Optional session/tag for cancellation
   * @throws If the bundle is too large for the WASM scheduler slot size
   *
   * @example
   * const msg = SuperSonic.osc.encodeMessage('/n_set', [1001, 'freq', 880]);
   * sonic.sendOSC(msg);
   *
   * // With cancellation tags:
   * const bundle = SuperSonic.osc.encodeBundle(futureTime, packets);
   * sonic.sendOSC(bundle, { sessionId: 'song1', runTag: 'verse' });
   */
  sendOSC(oscData: Uint8Array | ArrayBuffer, options?: SendOSCOptions): void;

  /**
   * Cancel all scheduled messages with the given run tag.
   * Only affects messages in the JS prescheduler (not yet dispatched to WASM).
   * @param runTag - Tag to cancel
   */
  cancelTag(runTag: string): void;

  /**
   * Cancel all scheduled messages for a session.
   * @param sessionId - Session to cancel
   */
  cancelSession(sessionId: string): void;

  /**
   * Cancel scheduled messages matching both a session and run tag.
   * @param sessionId - Session to match
   * @param runTag - Tag to match within that session
   */
  cancelSessionTag(sessionId: string, runTag: string): void;

  /** Cancel all scheduled messages in the JS prescheduler. */
  cancelAll(): void;

  /**
   * Flush all pending OSC messages from both the JS prescheduler and the
   * WASM BundleScheduler.
   *
   * Unlike {@link cancelAll} which only clears the JS prescheduler, this also
   * clears bundles already consumed from the ring buffer and sitting in the
   * WASM scheduler's priority queue. Resolves when both are confirmed empty.
   */
  purge(): Promise<void>;

  /**
   * Create an OscChannel for direct worker-to-worklet communication.
   *
   * The returned channel can be transferred to a Web Worker, allowing that
   * worker to send OSC directly to the AudioWorklet without going through
   * the main thread. Works in both SAB and postMessage modes.
   *
   * @param options - Channel options
   * @param options.sourceId - Numeric source ID (0 = main thread, 1+ = workers)
   *
   * @example
   * const channel = sonic.createOscChannel();
   * myWorker.postMessage(
   *   { channel: channel.transferable },
   *   channel.transferList,
   * );
   */
  createOscChannel(options?: { sourceId?: number; blocking?: boolean }): OscChannel;

  // ──────────────────────────────────────────────────────────────────────────
  // Asset Loading
  // ──────────────────────────────────────────────────────────────────────────

  /**
   * Load a SynthDef into scsynth.
   *
   * Accepts multiple source types:
   * - **Name string** — fetched from `synthdefBaseURL` (e.g. `'beep'` → `synthdefBaseURL/beep.scsyndef`)
   * - **Path/URL string** — fetched directly (must contain `/` or `://`)
   * - **ArrayBuffer / Uint8Array** — raw synthdef bytes
   * - **File / Blob** — e.g. from a file input
   *
   * @param source - SynthDef name, path/URL, raw bytes, or File/Blob
   * @returns The extracted name and byte size
   * @throws If the source type is invalid or the synthdef can't be parsed
   *
   * @example
   * // By name (uses synthdefBaseURL):
   * await sonic.loadSynthDef('beep');
   *
   * // By URL:
   * await sonic.loadSynthDef('/assets/synthdefs/pad.scsyndef');
   *
   * // From raw bytes:
   * const bytes = await fetch('/my-synth.scsyndef').then(r => r.arrayBuffer());
   * await sonic.loadSynthDef(bytes);
   *
   * // From file input:
   * fileInput.onchange = async (e) => {
   *   await sonic.loadSynthDef(e.target.files[0]);
   * };
   */
  loadSynthDef(source: string | ArrayBuffer | ArrayBufferView | Blob): Promise<LoadSynthDefResult>;

  /**
   * Load multiple SynthDefs by name in parallel.
   *
   * @param names - Array of synthdef names
   * @returns Object mapping each name to `{ success: true }` or `{ success: false, error: string }`
   *
   * @example
   * const results = await sonic.loadSynthDefs(['beep', 'pad', 'kick']);
   * if (!results.kick.success) console.error(results.kick.error);
   */
  loadSynthDefs(names: string[]): Promise<Record<string, { success: boolean; error?: string }>>;

  /**
   * Load an audio sample into a scsynth buffer slot.
   *
   * Decodes the audio file (WAV, AIFF, etc.) and copies the samples into
   * the WASM buffer pool. The buffer is then available for use with `PlayBuf`,
   * `BufRd`, etc.
   *
   * @param bufnum - Buffer slot number (0 to numBuffers-1)
   * @param source - Sample path/URL, raw bytes, or File/Blob
   * @param startFrame - First frame to read (default: 0)
   * @param numFrames - Number of frames to read (default: 0 = all)
   * @returns Buffer info including frame count, channels, and sample rate
   *
   * @example
   * // Load from URL:
   * await sonic.loadSample(0, '/samples/kick.wav');
   *
   * // Use in a synth:
   * await sonic.send('/s_new', 'sampler', 1001, 0, 1, 'bufnum', 0);
   */
  loadSample(
    bufnum: number,
    source: string | ArrayBuffer | ArrayBufferView | Blob,
    startFrame?: number,
    numFrames?: number,
  ): Promise<LoadSampleResult>;

  /**
   * Get info about all loaded audio buffers.
   *
   * @example
   * const buffers = sonic.getLoadedBuffers();
   * for (const buf of buffers) {
   *   console.log(`Buffer ${buf.bufnum}: ${buf.duration.toFixed(1)}s, ${buf.source}`);
   * }
   */
  getLoadedBuffers(): LoadedBufferInfo[];

  /**
   * Wait for scsynth to process all pending commands.
   *
   * Sends a `/sync` message and waits for the `/synced` reply. Use after
   * loading synthdefs or buffers to ensure they're ready before creating synths.
   *
   * @param syncId - Optional custom sync ID (random if omitted)
   * @throws After timeout if scsynth doesn't respond
   *
   * @example
   * await sonic.loadSynthDef('beep');
   * await sonic.sync();
   * // SynthDef is now guaranteed to be loaded
   * await sonic.send('/s_new', 'beep', 1001, 0, 1);
   */
  sync(syncId?: number): Promise<void>;

  // ──────────────────────────────────────────────────────────────────────────
  // Metrics & Monitoring
  // ──────────────────────────────────────────────────────────────────────────

  /**
   * Get current metrics as a named object.
   *
   * @example
   * const m = sonic.getMetrics();
   * console.log(`Messages sent: ${m.oscOutMessagesSent}`);
   * console.log(`Scheduler depth: ${m.scsynthSchedulerDepth}`);
   */
  getMetrics(): SuperSonicMetrics;

  /**
   * Get metrics as a flat Uint32Array for zero-allocation reading.
   *
   * Returns the same array reference every call — values are updated in-place.
   * Use {@link SuperSonic.getMetricsSchema} for offset mappings.
   *
   * @example
   * const schema = SuperSonic.getMetricsSchema();
   * const arr = sonic.getMetricsArray();
   * const sent = arr[schema.metrics.oscOutMessagesSent.offset];
   */
  getMetricsArray(): Uint32Array;

  /**
   * Get a diagnostic snapshot with metrics, node tree, and memory info.
   *
   * Useful for capturing state for bug reports or debugging timing issues.
   */
  getSnapshot(): Snapshot;

  // ──────────────────────────────────────────────────────────────────────────
  // Node Tree
  // ──────────────────────────────────────────────────────────────────────────

  /**
   * Get the node tree in flat format with linkage pointers.
   *
   * More efficient than {@link getTree} for serialization or custom rendering.
   */
  getRawTree(): RawTree;

  /**
   * Get the node tree in hierarchical format.
   *
   * @example
   * const tree = sonic.getTree();
   * function printTree(node, indent = 0) {
   *   const prefix = '  '.repeat(indent);
   *   const label = node.type === 'synth' ? node.defName : 'group';
   *   console.log(`${prefix}[${node.id}] ${label}`);
   *   for (const child of node.children) printTree(child, indent + 1);
   * }
   * printTree(tree.root);
   */
  getTree(): Tree;

  // ──────────────────────────────────────────────────────────────────────────
  // Timing
  // ──────────────────────────────────────────────────────────────────────────

  /**
   * Set clock offset for multi-system sync (e.g. Ableton Link, NTP server).
   *
   * Shifts all scheduled bundle execution times by the specified offset.
   * Positive values mean the shared/server clock is ahead of local time.
   *
   * @param offsetS - Offset in seconds
   */
  setClockOffset(offsetS: number): void;

  // ──────────────────────────────────────────────────────────────────────────
  // Audio Capture (SAB mode only)
  // ──────────────────────────────────────────────────────────────────────────

  /** Start capturing audio output to a buffer. SAB mode only. */
  startCapture(): void;

  /** Stop capturing and return the captured audio data. */
  stopCapture(): Float32Array;

  /** Check if audio capture is currently enabled. */
  isCaptureEnabled(): boolean;

  /** Get number of audio frames captured so far. */
  getCaptureFrames(): number;

  /** Get maximum capture duration in seconds. */
  getMaxCaptureDuration(): number;

  // ──────────────────────────────────────────────────────────────────────────
  // Info
  // ──────────────────────────────────────────────────────────────────────────

  /**
   * Get engine info: sample rate, memory layout, capabilities, and version.
   *
   * @example
   * const info = sonic.getInfo();
   * console.log(`Sample rate: ${info.sampleRate}Hz`);
   * console.log(`Boot time: ${info.bootTimeMs}ms`);
   * console.log(`Version: ${info.version}`);
   */
  getInfo(): SuperSonicInfo;
}

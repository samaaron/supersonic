// SPDX-License-Identifier: MIT OR GPL-3.0-or-later
// Copyright (c) 2025 Sam Aaron
//
// Type declarations for supersonic-scsynth
//
// For narrative documentation, usage examples, and comparison tables
// see docs/API.md

// ============================================================================
// Core Types
// ============================================================================

/** A v7 UUID as 16 raw bytes. */
export type UUID = Uint8Array;

/**
 * A node identifier (int32).
 */
export type NodeID = number;

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
 * - an array → an OSC array (`[` … `]`)
 *
 * For 64-bit, timetag or UUID types, use the tagged object form:
 * @example
 * { type: 'int', value: 42 }
 * { type: 'float', value: 440 }     // force float32 for whole numbers
 * { type: 'string', value: 'hello' }
 * { type: 'blob', value: new Uint8Array([1,2,3]) }
 * { type: 'bool', value: true }
 * { type: 'int64', value: 9007199254740992n }
 * { type: 'double', value: 3.141592653589793 }
 * { type: 'timetag', value: ntpTimestamp }
 * { type: 'uuid', value: uuidBytes } // 16 bytes, OSC tag `u`
 */
export type OscArg =
  | number
  | string
  | boolean
  | Uint8Array
  | ArrayBuffer
  | OscArg[]
  | { type: 'int'; value: number }
  | { type: 'float'; value: number }
  | { type: 'string'; value: string }
  | { type: 'blob'; value: Uint8Array | ArrayBuffer }
  | { type: 'bool'; value: boolean }
  | { type: 'int64'; value: number | bigint }
  | { type: 'double'; value: number }
  | { type: 'timetag'; value: number }
  | { type: 'uuid'; value: UUID };

/**
 * Decoded OSC message as a plain array.
 *
 * The first element is always the address string, followed by zero or more arguments.
 *
 * @example
 * // A decoded /n_go message received from the engine:
 * ["/n_go", 1001, 0, -1, -1, 0]
 *
 * // Access parts:
 * const address = msg[0];  // "/n_go"
 * const args = msg.slice(1);  // [1001, 0, -1, -1, 0]
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
 * A message as an array, or a nested bundle:
 * @example
 * // Message:
 * ["/s_new", "beep", 1001, 0, 0]
 *
 * // Nested bundle:
 * { timeTag: ntpTime, packets: [ ["/n_set", 1001, "freq", 880] ] }
 */
export type OscBundlePacket =
  | OscMessage
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
 * Engine configuration options controlling resource limits and audio behaviour.
 *
 * The engine's own options (their names, defaults and ranges) are listed once
 * in `dsp/scsynth/scsynth_options.h`; the values documented here are that
 * list's. The remaining fields are the web host's own and never reach the
 * engine. Override via `new SuperSonic({ scsynthOptions: { ... } })`.
 *
 * Until 0.87 the web client kept defaults of its own, and three differ from
 * the engine's list: `maxNodes` was 8192 (now 1024), `numAudioBusChannels`
 * 128 (now 1024) and `numControlBusChannels` 4096 (now 16384). A host that
 * needs more than 1024 nodes at once sets `maxNodes` itself.
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
  /** Audio bus channels for routing between synths. Default: 1024. */
  numAudioBusChannels?: number;
  /** Input channels read from the device (0 disables input). Default: 2. */
  numInputBusChannels?: number;
  /** Output channels opened on the audio graph (1–128). Default: 2. */
  numOutputBusChannels?: number;
  /** Control bus channels for control-rate data. Default: 16384. */
  numControlBusChannels?: number;
  /** Audio buffer length — must be 128 (WebAudio API constraint). */
  bufLength?: 128;
  /** Real-time memory pool in KB for synthesis allocations. Default: 8192 (8MB). */
  realTimeMemorySize?: number;
  /** Random number generators. Default: 64. */
  numRGens?: number;
  /** scsynth's real-time flag. Always false here: the AudioWorklet drives the engine. */
  realTime?: boolean;
  /** Memory locking — not applicable in browser. Default: false. */
  memoryLocking?: boolean;
  /** Load synth definitions from the synthdef directory at boot: 0 or 1. Default: 0. */
  loadGraphDefs?: 0 | 1;
  /**
   * Preferred sample rate: 0, or 8000–384000. The rate the engine runs at is
   * the AudioContext's: set it with `audioContextOptions.sampleRate`
   * (default 48000), or by passing your own `audioContext`.
   */
  preferredSampleRate?: number;
  /** How much the engine prints, 0–4; 0 is quiet. Default: 0. */
  verbosity?: number;
}

/**
 * Length limits for the text the activity events carry: `debug`, `in:text`
 * and `out:text`. A limit of 0 means no limit.
 */
export interface ActivityLineConfig {
  /** Longest text for every activity type: an engine debug line, or one argument of an OSC message. Default: 200. */
  maxLineLength?: number;
  /** Override for the engine's debug lines (the `debug` event). null = use maxLineLength. */
  engineMaxLineLength?: number | null;
  /** Override for incoming OSC (`in:text`), per argument. null = use maxLineLength. */
  oscInMaxLineLength?: number | null;
  /** Override for outgoing OSC (`out:text`), per argument. null = use maxLineLength. */
  oscOutMaxLineLength?: number | null;
}

/** The engine's lifecycle state: the same words as native's `engineState()`. */
export type EngineState = 'stopped' | 'booting' | 'running' | 'restarting' | 'error';

/**
 * Options for the SuperSonic constructor.
 *
 * SuperSonic needs to know where its files are: `baseURL`, or else
 * `workerBaseURL` together with `coreBaseURL` or `wasmBaseURL`. The
 * constructor throws without them.
 *
 * @example
 * // Simplest setup — all assets co-located:
 * const sonic = new SuperSonic({ baseURL: '/supersonic/dist/' });
 *
 * // CDN usage — the engine lives in the supersonic-scsynth-core package:
 * const CDN = 'https://unpkg.com/';
 * const sonic = new SuperSonic({
 *   mode: 'postMessage',  // a CDN can't send the COOP/COEP headers SAB needs
 *   baseURL:     CDN + 'supersonic-scsynth@latest/dist/',
 *   coreBaseURL: CDN + 'supersonic-scsynth-core@latest/',
 * });
 *
 * // Full control:
 * const sonic = new SuperSonic({
 *   mode: 'sab',
 *   workerBaseURL: '/workers/',
 *   coreBaseURL: '/core/',
 *   sampleBaseURL: '/samples/',
 *   synthdefBaseURL: '/synthdefs/',
 *   scsynthOptions: { numBuffers: 2048 },
 * });
 */
export interface SuperSonicOptions {
  /**
   * Transport mode.
   * - `'postMessage'` — works everywhere, no special headers needed (the default off an isolated page)
   * - `'sab'` — lowest latency, requires the Cross-Origin-Opener-Policy and Cross-Origin-Embedder-Policy headers (the default on a cross-origin isolated page)
   *
   * See docs/MODES.md for a full comparison of communication modes.
   */
  mode?: TransportMode;

  /** Convenience shorthand when all assets (WASM, worklet, workers, synthdefs, samples) are co-located. */
  baseURL?: string;
  /** Base URL for the engine: the WASM and the AudioWorklet (the supersonic-scsynth-core package). Defaults to `baseURL`. */
  coreBaseURL?: string;
  /** Base URL for the worker scripts. Defaults to `baseURL + 'workers/'`. */
  workerBaseURL?: string;
  /** Base URL for WASM files. Defaults to `coreBaseURL + 'wasm/'`. */
  wasmBaseURL?: string;
  /** Full URL to the WASM binary. Overrides `wasmBaseURL`. */
  wasmUrl?: string;
  /** Full URL to the AudioWorklet script. Overrides `coreBaseURL`. */
  workletUrl?: string;

  /** Base URL for audio sample files (used by {@link SuperSonic.loadSample}). Defaults to `baseURL + 'samples/'`. */
  sampleBaseURL?: string;
  /** Base URL for synthdef files (used by {@link SuperSonic.loadSynthDef}). Defaults to `baseURL + 'synthdefs/'`. */
  synthdefBaseURL?: string;

  /** Provide your own AudioContext instead of letting SuperSonic create one. It stays yours: kept across `reload()`, left open by `shutdown()`. */
  audioContext?: AudioContext;
  /** Options passed to `new AudioContext()`, over the defaults `{ latencyHint: 'interactive', sampleRate: 48000 }`. Ignored if `audioContext` is provided. */
  audioContextOptions?: AudioContextOptions;
  /** Auto-connect the AudioWorkletNode to the AudioContext destination. Default: true. */
  autoConnect?: boolean;

  /** The engine's options (see {@link ScsynthOptions}). Validated by the constructor, which throws on a bad one. */
  scsynthOptions?: ScsynthOptions;

  /** How often to snapshot metrics and the node tree in postMessage mode (ms). Default: 150. */
  snapshotIntervalMs?: number;

  /**
   * What the page going away, or out of sight, does to the engine.
   * `pagehide`: `'shutdown'` (default — an engine left running after its page is audio with no page to stop it) or
   * `'none'`. `hidden`: `'keep'` (default — a tab behind another plays on) or `'suspend'` (and resume when shown).
   * After a `'shutdown'`, a page restored from the back/forward cache calls `init()` again.
   */
  pageLifecycle?: { pagehide?: 'shutdown' | 'none'; hidden?: 'keep' | 'suspend' };

  /**
   * Web MIDI. Off by default: some browsers ask the user's permission, and a page that never uses MIDI
   * should not ask. `true`, or an object of options for the MIDI manager. Brought up during `init()`; to ask
   * later, from the gesture that wants it, use {@link SuperSonic.enableMidi}. If it cannot come up the
   * engine boots without it, emits `'error'`, and {@link SuperSonic.midiError} says why. Default: false.
   */
  midi?: boolean | Record<string, unknown>;
  /** The Gamepad API: `true`, or an object of options for the gamepad manager. Default: false. */
  gamepad?: boolean | Record<string, unknown>;

  /** Enable all debug console logging. Default: false. */
  debug?: boolean;
  /** Log the engine's debug output to the console. Default: false. */
  debugEngine?: boolean;
  /** Log incoming OSC messages to console. Default: false. */
  debugOscIn?: boolean;
  /** Log outgoing OSC messages to console. Default: false. */
  debugOscOut?: boolean;

  /** Length limits for the text the activity events carry. */
  activityEvent?: ActivityLineConfig;

  /** Initial size of the sample buffer pool in bytes. Default: 4 MB. */
  bufferPoolSize?: number;
  /** Most the sample buffer pool may grow to, in bytes. The pool grows on demand up to this limit. Default: 768 MB. */
  maxBufferMemory?: number;
  /** Bytes the sample buffer pool grows by each time it grows. Default: 32 MB. */
  bufferGrowIncrement?: number;

  /** Max fetch retries when loading assets. Default: 3. */
  fetchMaxRetries?: number;
  /** Base delay between retries in ms (exponential backoff). Default: 1000. */
  fetchRetryDelay?: number;
}

// ============================================================================
// Metrics Types
// ============================================================================

/** How full a ring buffer is, as {@link SuperSonic.getMetrics} reports it. */
export interface RingBufferUsage {
  /** Bytes in use now. */
  bytes: number;
  /** Bytes in use, as a percentage of `capacity`. */
  percentage: number;
  /** Most bytes ever in use (high water mark). */
  peakBytes: number;
  /** `peakBytes` as a percentage of `capacity`. */
  peakPercentage: number;
  /** The ring's size in bytes. */
  capacity: number;
}

/**
 * Metrics returned by {@link SuperSonic.getMetrics}, by name.
 *
 * Counters are cumulative; gauges reflect current state. Most keys are also
 * in {@link SuperSonic.getMetricsSchema}, which gives their descriptions,
 * units and offsets into {@link SuperSonic.getMetricsArray}. Where the two
 * differ: the schema's flat ring-buffer numbers (`inBufferUsedBytes`,
 * `inBufferPeakBytes`, `inBufferCapacity` and the same for `out` and
 * `nrtOut`) are grouped here as `inBufferUsed`, `outBufferUsed` and
 * `nrtOutBufferUsed`; `mode` and `audioContextState` are strings here and
 * enum indexes in the array; `hasPlaybackStats` is a boolean here and 0 or 1
 * in the array; `ntpStartTime` is only here.
 */
export interface SuperSonicMetrics {
  // Engine
  /** Audio process() calls (cumulative). */
  engineProcessCount: number;
  /** Messages drained from the IN ring and dispatched. */
  engineMessagesProcessed: number;
  /** Messages dropped (ring buffer full). */
  engineMessagesDropped: number;
  /** Current scheduler queue depth. */
  engineSchedulerDepth: number;
  /** Peak scheduler queue depth (high water mark). */
  engineSchedulerPeakDepth: number;
  /** Events dropped because the scheduler queue overflowed. */
  engineSchedulerDropped: number;
  /** Messages lost in transit from JS to the engine. */
  engineSequenceGaps: number;
  /** WASM execution errors in the audio worklet. */
  engineWasmErrors: number;
  /** Bundles executed after their scheduled time. */
  engineSchedulerLates: number;
  /** Maximum lateness observed in the scheduler (ms). */
  engineSchedulerMaxLateMs: number;
  /** Most recent late magnitude in the scheduler (ms). */
  engineSchedulerLastLateMs: number;
  /** Process count when the last scheduler late occurred. */
  engineSchedulerLastLateTick: number;
  /** Maximum scheduler queue size. */
  engineSchedulerCapacity?: number;

  // OSC out
  /** OSC messages sent from JS to the engine. */
  oscOutMessagesSent: number;
  /** Total bytes sent from JS to the engine. */
  oscOutBytesSent: number;

  // OSC in
  /** OSC replies received from the engine. */
  oscInMessagesReceived: number;
  /** Total bytes received from the engine. */
  oscInBytesReceived: number;
  /** Replies lost in transit from the engine to JS. */
  oscInMessagesDropped: number;
  /** Corrupted messages detected in the ring buffer. */
  oscInCorrupted: number;

  // Debug
  /** Debug messages received from the engine. */
  debugMessagesReceived: number;
  /** Debug bytes received from the engine. */
  debugBytesReceived: number;

  /** SAB mode only: IN-ring writes dropped because the ring had no room. */
  ringBufferDirectWriteFails: number;

  // Ring buffers (present once the engine has booted)
  /** The IN ring (JS → engine). */
  inBufferUsed?: RingBufferUsage;
  /** The OUT ring (engine replies → JS). */
  outBufferUsed?: RingBufferUsage;
  /** The NRT-out ring (replies, notifications, debug). */
  nrtOutBufferUsed?: RingBufferUsage;

  // System info (written by the engine at boot)
  /** The Clockwork commit the engine was built from: its first 8 hex digits, read as a number; 0 when unknown. */
  clockworkCommit: number;
  /** Output sample rate in Hz. */
  audioSampleRate: number;
  /** Audio block size in frames (128 on the web). */
  audioBlockSize: number;
  /** Number of output bus channels. */
  audioOutputChannels: number;
  /** Number of input bus channels. */
  audioInputChannels: number;

  // ClockworkClock readouts (written every audio block)
  /** Tempo in milli-BPM (bpm * 1000). Divide by 1000 for BPM. */
  clockTempoMbpm: number;
  /** Beat position * 100. Divide by 100 for the beat. */
  clockBeatCenti: number;
  /** Phase within the quantum * 100. Divide by 100 for the phase. */
  clockPhaseCenti: number;
  /** Transport playing (0 = stopped, 1 = playing). */
  clockPlaying: number;

  // Link (native only; 0 on the web, where there is no Link session)
  /** Connected Ableton Link peers on the network. */
  linkPeers: number;
  /** Shared Link session tempo in milli-BPM (bpm * 1000). */
  linkTempoMbpm: number;
  /** Current Link beat position * 100. */
  linkBeatCenti: number;
  /** Phase within the Link quantum * 100. */
  linkPhaseCenti: number;
  /** Link transport playing (0 = stopped, 1 = playing). */
  linkPlaying: number;

  // Link Audio stream health (native only; 0 on the web)
  /** Active received Link Audio channels. */
  linkAudioInChannels: number;
  /** Received Link Audio stream sample rate in Hz. */
  linkAudioStreamRate: number;
  /** Receiver queue underruns — stream audio arrived too late to play. */
  linkAudioUnderruns: number;
  /** Received Link Audio queued in the receiver (ms). */
  linkAudioBufferedMs: number;
  /** Read-rate deviation from the sender's clock (parts per million, signed). */
  linkAudioDriftPpm: number;
  /** Link Audio publishing enabled (0 = off, 1 = on). */
  linkAudioPublish: number;
  /** Active Link Audio output sinks. */
  linkAudioSinks: number;

  // Context (main thread)
  /** Clock drift between AudioContext and wall clock (ms, signed). */
  driftOffsetMs: number;
  /** Clock offset for multi-system sync (ms, signed). */
  clockOffsetMs: number;
  /** NTP time (seconds since 1900) when the AudioContext started; 0 before boot. */
  ntpStartTime: number;
  /** AudioContext state; `'unknown'` when there is no context. */
  audioContextState: 'running' | 'suspended' | 'closed' | 'interrupted' | 'unknown';
  /** Transport mode. */
  mode: TransportMode;

  // Sample buffer pool and synthdefs (declared by SuperSonic; 0 until the first buffer is used)
  /** Sample buffer pool bytes in use. */
  bufferPoolUsedBytes: number;
  /** Sample buffer pool bytes free. */
  bufferPoolAvailableBytes: number;
  /** Buffers currently allocated. */
  bufferPoolAllocations: number;
  /** Committed capacity across all pool segments, in bytes. */
  bufferPoolTotalCapacity: number;
  /** Most the pool may grow to, in bytes. */
  bufferPoolMaxCapacity: number;
  /** Times the pool has grown. */
  bufferPoolGrowthCount: number;
  /** Pool segments; 1 means it has never grown. */
  bufferPoolPoolCount: number;
  /** Synthdefs this client holds, and will restore after a reload. */
  loadedSynthDefs: number;

  // Audio diagnostics (main thread)
  /** Audio health: fraction of expected audio frames delivered, 0–100 (every browser). */
  audioHealthPct: number;
  /** Whether the Chrome playbackStats API is available. */
  hasPlaybackStats: boolean;
  /** Audio underrun/glitch events (Chrome playbackStats; 0 elsewhere). */
  glitchCount: number;
  /** Total silence from audio underruns in ms (Chrome playbackStats; 0 elsewhere). */
  glitchDurationMs: number;
  /** Average audio output latency in microseconds (Chrome playbackStats; 0 elsewhere). */
  averageLatencyUs: number;
  /** Maximum audio output latency in microseconds (Chrome playbackStats; 0 elsewhere). */
  maxLatencyUs: number;
  /** Total audio rendered duration in ms (Chrome playbackStats; 0 elsewhere). */
  totalFramesDurationMs: number;
}

/** Schema entry describing a single metric field. */
export interface MetricDefinition {
  /** Offset into the flat metrics Uint32Array. */
  offset: number;
  /** Metric type: counter (cumulative), gauge (current), constant, or enum; `'u32'` for the metrics SuperSonic declares (the sample buffer pool and `loadedSynthDefs`). */
  type: 'counter' | 'gauge' | 'constant' | 'enum' | 'u32';
  /** For a metric SuperSonic declares: its index within the range clockwork leaves for them. */
  slot?: number;
  /** Unit of measurement. */
  unit?: string;
  /** Whether the value should be read as signed int32. */
  signed?: boolean;
  /** Native only — nothing writes it on the web, where it reads 0. */
  nativeOnly?: boolean;
  /** Enum value names (for type 'enum'). */
  values?: string[];
  /** Human-readable description. */
  description: string;
}

/**
 * A NATIVE_STATS segment entry (native only).
 */
export interface NativeStatDefinition {
  /** u32 slot within the NATIVE_STATS segment. */
  index: number;
  /** Metric type: counter (cumulative) or gauge (current). */
  type: 'counter' | 'gauge';
  /** Unit of measurement. */
  unit?: string;
  /** Human-readable description. */
  description: string;
}

/**
 * Metrics schema returned by {@link SuperSonic.getMetricsSchema}.
 *
 * Contains metric definitions with array offsets (for zero-allocation reading)
 * and a declarative UI layout for rendering metrics panels.
 */
export interface MetricsSchema {
  /** Every metric in the flat array {@link SuperSonic.getMetricsArray} returns, by name, with its offset, type,
   * unit and description: clockwork's own and the ones SuperSonic declares. Native-only ones are marked
   * `nativeOnly` and read 0 on the web. See {@link SuperSonicMetrics} for how these names relate to
   * {@link SuperSonic.getMetrics}' keys. */
  metrics: Record<string, MetricDefinition>;
  /** NATIVE_STATS shm segment descriptions (native only). `index` is
   * the u32 slot within that segment, not an offset into the metrics array. */
  nativeStats: Record<string, NativeStatDefinition>;
  /** Descriptions for rows combining several metrics in one reading
   * ("current | peak", ...), shared by web and native layouts. */
  composites: Record<string, { description: string }>;
  /** Panel structure for rendering a metrics UI. Used by the `<clockwork-metrics>` web component. */
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
  /** The node's UUID when it has one, otherwise its numeric ID. */
  id: UUID | NodeID;
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
 * console.log(tree.root?.children); // top-level groups and synths
 * console.log(tree.nodeCount);      // total nodes in the tree
 */
export interface Tree {
  /** Total number of nodes. */
  nodeCount: number;
  /** Increments on any tree change — useful for detecting updates. */
  version: number;
  /** Nodes that exceeded mirror capacity (tree may be incomplete if > 0). */
  droppedCount: number;
  /** The root group, or null before the engine has published a tree. */
  root: TreeNode | null;
}

/** A node in the flat (raw) tree representation with linkage pointers. */
export interface RawTreeNode {
  /** Numeric node ID; null for a node that has only a UUID. */
  id: NodeID | null;
  /** Parent node ID (-1 for root). */
  parentId: NodeID;
  /** true if group, false if synth. */
  isGroup: boolean;
  /** Previous sibling node ID (-1 if none). */
  prevId: NodeID;
  /** Next sibling node ID (-1 if none). */
  nextId: NodeID;
  /** First child node ID (groups only, -1 if empty). */
  headId: NodeID;
  /** SynthDef name (synths only, empty string for groups). */
  defName: string;
  /** UUID if the node was created with a UUID node ID, null otherwise. */
  uuid: UUID | null;
  /** The parent's UUID, or null. */
  parentUuid: UUID | null;
  /** Peak of the node's output over the last block. */
  outPeak: number;
  /** Synths under this node, itself included. */
  synthCount: number;
  /** Whether the node subscribes to input. */
  listens: boolean;
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
  /** WebAssembly memory committed at boot, in bytes. */
  totalMemory: number;
  /** Size of the WASM heap in the memory layout, in bytes. */
  wasmHeapSize: number;
  /** Size of the engine's fixed memory region, in bytes. */
  guestMemorySize: number;
  /** Time taken to boot in ms, or null if not yet booted. */
  bootTimeMs: number | null;
  /** Browser capability detection results. */
  capabilities: {
    audioWorklet: boolean;
    sharedArrayBuffer: boolean;
    crossOriginIsolated: boolean;
    atomics: boolean;
    webWorker: boolean;
    playbackStats: boolean;
  };
  /** The engine's version string as the worklet reports it, or null. */
  version: string | null;
}

/**
 * Diagnostic snapshot returned by {@link SuperSonic.getSnapshot}.
 *
 * Captures metrics with descriptions, and JS heap memory info. Useful for
 * bug reports and debugging timing issues. For the node tree, see
 * {@link SuperSonic.getRawTree}.
 */
export interface Snapshot {
  /** ISO 8601 timestamp when the snapshot was taken. */
  timestamp: string;
  /** Every {@link SuperSonicMetrics} value by name, with clockwork's description of it where it has one. */
  metrics: Record<string, { value: SuperSonicMetrics[keyof SuperSonicMetrics]; description?: string }>;
  /** JS heap memory info (Chrome only, null in other browsers). */
  memory: {
    usedJSHeapSize: number;
    totalJSHeapSize: number;
    jsHeapSizeLimit: number;
  } | null;
}

/**
 * System performance report returned by {@link SuperSonic.getSystemReport}.
 *
 * Includes hardware info, audio configuration, Chrome playbackStats (if available),
 * a cross-browser audio health percentage, and a human-readable health assessment.
 * Useful for diagnosing audio crackling on constrained hardware.
 */
export interface SystemReport {
  /** ISO 8601 timestamp when the report was generated. */
  timestamp: string;
  /** Hardware and browser info. */
  system: {
    userAgent: string;
    hardwareConcurrency: number | null;
    deviceMemory: number | null;
    platform: string;
  };
  /** AudioContext configuration and state. */
  audio: {
    sampleRate: number;
    baseLatency: number | null;
    outputLatency: number | null;
    state: string;
    channelCount: number;
  };
  /** Chrome playbackStats (null on browsers without support). */
  playbackStats: {
    glitchCount: number;
    glitchDurationS: number;
    totalDurationS: number;
    averageLatencyS: number;
    maximumLatencyS: number;
  } | null;
  /** Engine configuration. */
  engine: {
    mode: TransportMode;
    version: string | null;
    bootTimeMs: number | null;
  };
  /** Health assessment with issues and human-readable summary. */
  health: {
    audioHealthPct: number;
    issues: Array<{ severity: 'warning' | 'error' | 'critical'; message: string }>;
    summary: string;
  };
  /** Full metrics snapshot at time of report. */
  metrics: SuperSonicMetrics;
}

/**
 * Metadata about decoded audio content.
 *
 * Returned by {@link SuperSonic.sampleInfo}. Also the shape of each entry
 * in {@link SuperSonic.getLoadedBuffers} (with `bufnum`) and the return
 * value of {@link SuperSonic.loadSample} (with `bufnum`).
 */
export interface SampleInfo {
  /** SHA-256 hex hash of the decoded interleaved audio content. */
  hash: string;
  /** Original source path/URL, or null for inline data. */
  source: string | null;
  /** Number of sample frames. */
  numFrames: number;
  /** Number of channels. */
  numChannels: number;
  /** Sample rate in Hz. */
  sampleRate: number;
  /** Duration in seconds. */
  duration: number;
}

/** Info about a loaded audio buffer, returned by {@link SuperSonic.getLoadedBuffers}. */
export interface LoadedBufferInfo extends SampleInfo {
  /** Buffer slot number. */
  bufnum: number;
}

/** Result from {@link SuperSonic.loadSynthDef}. */
export interface LoadSynthDefResult {
  /** Extracted SynthDef name. */
  name: string;
  /** Size of the synthdef binary in bytes. */
  size: number;
}

/** Result from {@link SuperSonic.loadSample}. */
export interface LoadSampleResult extends SampleInfo {
  /** Buffer slot the sample was loaded into. */
  bufnum: number;
}

/** Boot timing statistics. */
export interface BootStats {
  /** Timestamp when init() started (performance.now()), or null. */
  initStartTime: number | null;
  /** Total boot duration in ms, or null if not yet booted. */
  initDuration: number | null;
}

// ============================================================================
// ClockworkClock — session-timeline service (clockwork's clock)
// ============================================================================

/**
 * Engine session-timeline service. Tempo, beat origin, transport, meter and
 * NTP-derived "now." Accessed via {@link SuperSonic.clock}, after `init()`.
 *
 * Each field is read/written independently — no multi-field coherence
 * guarantee. There is no Ableton Link on the web: `setLinkEnabled(true)` is
 * ignored with a console warning, `isLinkEnabled()` is always false and
 * `numPeers()` always 0.
 */
export interface ClockworkClock {
  // ── Time / drift ─────────────────────────────────────────────────────

  /** Measure the NTP start time and the first drift. `init()` does this; a client does not need to. */
  initialize(): Promise<void>;
  /** Re-measure the NTP start time and drift, as after a suspend. */
  resync(): void;
  /** Start re-measuring the drift periodically. */
  startDriftTimer(): void;
  /** Stop re-measuring the drift. */
  stopDriftTimer(): void;
  /** Re-measure the drift now. */
  updateDriftOffset(): void;
  /** Drift between the AudioContext and the wall clock, in milliseconds (signed). */
  getDriftOffset(): number;
  /** NTP time (seconds since 1900) when the AudioContext started. */
  getNTPStartTime(): number;
  /** The clock offset set by {@link setClockOffset}, in milliseconds. */
  getClockOffset(): number;
  /** Set the clock offset for multi-system sync, in seconds (stored rounded to the millisecond). */
  setClockOffset(offsetS: number): void;
  /** Stop the drift timer and forget the timing state. */
  reset(): void;

  // ── Session mutators ─────────────────────────────────────────────────

  /**
   * Change the tempo without moving the beat playing at the instant it changes.
   * @param atNtpSeconds the instant the tempo changes (omitted or 0: now). A
   *   scheduler working ahead gives the time its change will be heard.
   */
  setBpm(bpm: number, atNtpSeconds?: number): void;
  setIsPlaying(playing: boolean, atNtpSeconds?: number): void;
  /**
   * Set the meter: how quarter-note beats group into bars. 4/4 until set.
   * The beat grid is not moved.
   * @param num beats per bar: a whole number, 1 or more
   * @param den 1, 2, 4, 8, 16 or 32
   * @returns false, and nothing changed, for any other meter
   */
  setMeter(num: number, den: number): boolean;
  /** Ignored on the web (with a console warning when `true`): Link is native only. */
  setLinkEnabled(enabled: boolean): void;
  requestBeatAtTime(beat: number, atNtpSeconds: number, quantum: number): void;
  /** Identical to {@link requestBeatAtTime} on the web. */
  forceBeatAtTime(beat: number, atNtpSeconds: number, quantum: number): void;

  // ── Session getters ──────────────────────────────────────────────────

  getBpm(): number;
  isPlaying(): boolean;
  getBeatOriginNtp(): number;
  /** The meter set by {@link setMeter}. */
  getMeter(): { num: number; den: number };
  /**
   * One more each time the beat grid moves (a tempo change, a new origin), by
   * any writer. A follower keeping its own copy of the grid reads this, then
   * the grid, and reads the grid again when it has changed. In postMessage
   * mode only this clock's own changes count.
   */
  getGeneration(): number;
  getIsPlayingAtNtp(): number;
  /** Always `false` on the web. */
  isLinkEnabled(): boolean;
  /** Always `0` on the web. */
  numPeers(): number;

  /**
   * Current NTP time as seen by the audio thread. Use this for scheduling:
   * `sonic.clock.now() + 0.05` gives a timestamp 50ms in audio-clock
   * future, which the audio thread reaches in 50ms of audio time —
   * independent of any wall-clock-vs-audio-clock skew.
   */
  now(): number;

  /**
   * Compute audio-thread NTP for a specific `AudioContext.currentTime`.
   * Lower-level than {@link now} — pass a value obtained from
   * `audioContext.getOutputTimestamp()` for sample-aligned scheduling.
   */
  nowAt(audioCurrentTime: number): number;

  /**
   * Current NTP time from the system wall clock. Use only when matching
   * against external wall-clock events; prefer {@link now} for scheduling
   * engine events.
   */
  wallNow(): number;

  // ── Beat math ────────────────────────────────────────────────────────

  beatAtTime(ntpSeconds: number, quantum: number): number;
  phaseAtTime(ntpSeconds: number, quantum: number): number;
  timeAtBeat(beat: number, quantum: number): number;
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
 * sonic.on('in', (msg) => {
 *   // msg is typed as OscMessage — [address, ...args]
 *   if (msg[0] === '/n_go') {
 *     console.log('Node started:', msg[1]);
 *   }
 * });
 *
 * sonic.on('setup', async () => {
 *   // Runs after init(), before 'ready'. Set up groups and FX chains here.
 *   sonic.send('/g_new', 1, 0, 0);
 * });
 */
export interface SuperSonicEventMap {
  /**
   * Fired after init completes, before `'ready'`.
   * Use for setting up groups, FX chains, and bus routing.
   * Can be async — init waits for all setup handlers to resolve; one that throws is reported on `'error'`.
   * Also fires after a `reload()`, including one `recover()` falls back to.
   */
  'setup': () => void | Promise<void>;

  /** Fired when the engine is fully booted and ready to receive messages, after `'setup'` (also after a reload). Payload includes browser capabilities and boot timing. */
  'ready': (data: { capabilities: SuperSonicInfo['capabilities']; bootStats: BootStats }) => void;

  /**
   * Decoded OSC message received from the engine.
   * Messages are plain arrays: `[address, ...args]`.
   */
  'in': (msg: OscMessage) => void;

  /** Raw OSC bytes received (before decoding), with when they arrived and the bundle's time tag (`scheduledTime`, null for a message). `sequence` is -1 for a reply the page made itself (MIDI, gamepad). */
  'in:osc': (data: { oscData: Uint8Array; sequence: number; timestamp: number; scheduledTime: number | null }) => void;

  /** Pre-formatted text representation of an incoming OSC message. Only emitted when listeners are attached or debug logging is enabled. */
  'in:text': (data: { text: string; sequence: number; timestamp: number }) => void;

  /** Pre-formatted HTML representation of an incoming OSC message with CSS classes for colourisation. Only emitted when listeners are attached. */
  'in:html': (data: { html: string; sequence: number; timestamp: number }) => void;

  /**
   * Decoded OSC message sent to the engine.
   * Messages are plain arrays: `[address, ...args]`. Mirrors the `'in'` event for outgoing messages.
   */
  'out': (msg: OscMessage) => void;

  /** Raw OSC bytes sent to the engine. Includes the sending channel's source ID (0 for the main thread), sequence number, NTP timestamp and the bundle's time tag (null for a message). */
  'out:osc': (data: { oscData: Uint8Array; sourceId: number; sequence: number; timestamp: number; scheduledTime: number | null }) => void;

  /** Pre-formatted text representation of an outgoing OSC message (a bundle as its messages, one per line). Only emitted when listeners are attached or debug logging is enabled. */
  'out:text': (data: { text: string; sequence: number; timestamp: number; scheduledTime: number | null }) => void;

  /** Pre-formatted HTML representation of an outgoing OSC message with CSS classes for colourisation. Only emitted when listeners are attached. */
  'out:html': (data: { html: string; sequence: number; timestamp: number }) => void;

  /** A line of the engine's debug output. Not also emitted as `'in'`. */
  'debug': (msg: { text: string; timestamp: number; sequence: number }) => void;

  /** Error from any component (worklet, transport, workers), a failed boot, a queued buffer command that failed, a `'setup'` listener that threw, or a MIDI or gamepad subsystem that could not come up. */
  'error': (error: Error) => void;

  /** Something a host should hear of in every build, for its own log (`{ message }`) — for instance `purge()` getting no answer from the worklet. */
  'warning': (data: { message: string }) => void;

  /** Engine is shutting down. Fired by `shutdown()`, `reset()`, and `destroy()`, when the engine was running or booting. */
  'shutdown': () => void;

  /** `destroy()` has been called. Fired first — before the shutdown, and before every listener is removed: the last chance to clean up. Not fired by `shutdown()` or `reset()`. */
  'destroy': () => void;

  /** Audio resumed after a suspend: `resume()` restarted the AudioContext and the audio thread is running. Not fired when the context was already running. */
  'resumed': () => void;

  /** Full reload started (worklet and WASM will be recreated). */
  'reload:start': () => void;

  /** Full reload completed, or failed (`success: false`, with the error). */
  'reload:complete': (data: { success: boolean; error?: Error }) => void;

  /** A reload failed. What it built has been taken down; `reload()` and `recover()` answer false. */
  'reload:failed': (data: { error: Error }) => void;

  /** The engine's state changed (see `getEngineState()`), as native sends `/clockwork/statechange`. `error` is set on `'error'`. */
  'statechange': (data: { state: EngineState; previous: EngineState; reason: string; error?: Error }) => void;

  /** AudioContext state changed. State is one of: `'running'`, `'suspended'`, `'closed'`, or `'interrupted'`. */
  'audiocontext:statechange': (data: { state: AudioContextState }) => void;

  /** AudioContext was suspended (e.g. tab backgrounded, autoplay policy, iOS audio interruption). Show a restart UI and call `recover()` when the user interacts. */
  'audiocontext:suspended': () => void;

  /** AudioContext changed to the 'running' state. */
  'audiocontext:resumed': () => void;

  /** AudioContext was interrupted (iOS-specific). Another app or system event took audio focus. Similar to suspended but triggered externally. */
  'audiocontext:interrupted': () => void;

  /** An asset started loading. Type is `'wasm'`, `'synthdef'`, or `'sample'`. `size` (bytes) is given when known in advance. */
  'loading:start': (data: { type: string; name: string; size?: number }) => void;

  /** An asset finished loading. Size is in bytes. */
  'loading:complete': (data: { type: string; name: string; size: number }) => void;

  /** The sample buffer pool grew on demand: a new segment was added. */
  'buffer:pool:grown': (data: { poolIndex: number; newBytes: number; totalCapacity: number }) => void;
}

/** Union of all event names. */
export type SuperSonicEvent = keyof SuperSonicEventMap;

// ============================================================================
// OscChannel
// ============================================================================

/** OscChannel metrics counters. */
export interface OscChannelMetrics {
  messagesSent: number;
  bytesSent: number;
}

/** Transferable config for SAB mode OscChannel. */
export interface OscChannelSABTransferable {
  mode: 'sab';
  sharedBuffer: SharedArrayBuffer;
  ringBufferBase: number;
  bufferConstants: Record<string, number>;
  controlIndices: Record<string, number>;
  sourceId: number;
  /** The engine's memory: the receiving worker opens its own client over it. */
  wasmMemory: WebAssembly.Memory;
  /** The compiled engine module, shared rather than compiled again. */
  wasmModule: WebAssembly.Module;
}

/** Transferable config for postMessage mode OscChannel. */
export interface OscChannelPMTransferable {
  mode: 'postMessage';
  port: MessagePort;
  sourceId: number;
  /** The node IDs handed to the worker up front. */
  nodeIdRange?: { from: number; to: number };
  /** The port the worker asks for more node IDs on. */
  nodeIdPort?: MessagePort;
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
 * const channel = await OscChannel.fromTransferable(event.data.channel);
 * channel.send(oscBytes);
 */
export class OscChannel {
  /**
   * Send an OSC message: frames it onto the IN ring (SAB) or postMessages it to
   * the worklet (PM). Classification and scheduling happen on the audio thread
   * (the engine's OscIngress + BundleScheduler) — the producer never classifies.
   *
   * @param oscData - Encoded OSC bytes
   * @returns true if sent; false if the IN ring had no room (SAB, counted as
   *   `ringBufferDirectWriteFails`) or the channel is closed (PM)
   */
  send(oscData: Uint8Array): boolean;

  /** Get current metrics. In SAB mode these are the shared totals for every sender; in postMessage mode, this channel's own. */
  getMetrics(): OscChannelMetrics;

  /** Get and reset this channel's local counters (for periodic reporting). */
  getAndResetMetrics(): OscChannelMetrics;

  /**
   * Get the next unique node ID.
   *
   * Thread-safe — can be called concurrently from multiple workers and no
   * two callers will ever receive the same ID. IDs start at 1000: 0 is the
   * root group and 1–999 are left for the client to assign by hand.
   *
   * In postMessage mode a worker's channel takes IDs in ranges from the main
   * thread, asking for the next range before it needs it; it throws if a
   * tight loop uses a range up before the next has arrived.
   *
   * @returns A unique node ID (>= 1000)
   */
  nextNodeId(): number;

  /**
   * The engine's clock, in NTP seconds: the time its audio thread has reached, readable on any thread the channel is
   * on — a worker cannot see the AudioContext. The clock bundles are stamped on ({@link SuperSonic.clock}'s now()),
   * taken from the audio thread itself once a block: it stands still while the audio does (suspended, interrupted),
   * and after a reload it is the new engine's from its first block. 0 until the engine has rendered one.
   *
   * SAB mode reads the sample clock the audio thread publishes into shared memory; postMessage mode hears it from the
   * audio thread every few blocks and counts on from the last word by the wall clock, a tenth of a second at most.
   *
   * @example
   * // Inside a worker: schedule half a second ahead on the engine's own clock
   * channel.send(osc.encodeBundle(channel.now() + 0.5, [["/s_new", "beep", -1, 0, 0]]));
   *
   * @returns NTP seconds, or 0
   */
  now(): number;

  /** Close the channel. In postMessage mode this closes its port; in SAB mode it does nothing. */
  close(): void;

  /** Transport mode this channel is using. */
  get mode(): TransportMode;

  /**
   * Serializable config for transferring this channel to a worker via postMessage.
   * In postMessage mode each read hands out a fresh range of node IDs and a port
   * for more, so read it once per transfer.
   *
   * @example
   * worker.postMessage({ ch: channel.transferable }, channel.transferList);
   */
  get transferable(): OscChannelTransferable;

  /**
   * Array of transferable objects (MessagePorts) for the postMessage transfer list.
   * Read it after {@link transferable}.
   *
   * @example
   * worker.postMessage({ ch: channel.transferable }, channel.transferList);
   */
  get transferList(): Transferable[];

  /**
   * Reconstruct an OscChannel from data received via postMessage in a worker.
   * Asynchronous: in SAB mode the worker opens its own instance of the engine
   * module over the shared memory.
   *
   * @param data - The transferable config from `channel.transferable`
   * @example
   * // In a Web Worker:
   * self.onmessage = async (e) => {
   *   const channel = await OscChannel.fromTransferable(e.data.ch);
   *   channel.send(oscBytes);
   * };
   */
  static fromTransferable(data: OscChannelTransferable): Promise<OscChannel>;
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
 * const msg = SuperSonic.osc.encodeMessage('/s_new', ['beep', 1001, 0, 0]);
 *
 * // Encode a timed bundle
 * const time = SuperSonic.osc.ntpNow() + 0.5; // 500ms from now
 * const bundle = SuperSonic.osc.encodeBundle(time, [
 *   ['/s_new', 'beep', 1001, 0, 0, 'freq', 440],
 *   ['/s_new', 'beep', 1002, 0, 0, 'freq', 660],
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
   * osc.encodeMessage('/s_new', ['beep', 1001, 0, 0, 'freq', 440])
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
   * Get the current wall-clock time as an NTP timestamp (seconds since 1900).
   * To schedule against the engine's own clock, prefer `sonic.clock.now()`
   * (or `channel.now()` in a worker).
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
// OSC Command Types
// ============================================================================

/** Node add action: 0=head, 1=tail, 2=before, 3=after, 4=replace */
export type AddAction = 0 | 1 | 2 | 3 | 4;

// ============================================================================
// SuperSonic
// ============================================================================

/**
 * SuperSonic — WebAssembly SuperCollider synthesis engine for the browser.
 *
 * Coordinates WASM, AudioWorklet, SharedArrayBuffer, and IO Workers to run
 * scsynth with low latency inside a web page.
 *
 * @example
 * // CDN Quick Start
 * import { SuperSonic } from 'https://unpkg.com/supersonic-scsynth@latest/dist/supersonic.js';
 *
 * const CDN = 'https://unpkg.com/';
 * const sonic = new SuperSonic({
 *   baseURL: CDN + 'supersonic-scsynth@latest/dist/',
 *   coreBaseURL: CDN + 'supersonic-scsynth-core@latest/',
 *   synthdefBaseURL: CDN + 'supersonic-scsynth-synthdefs@latest/synthdefs/',
 * });
 *
 * // Call init after a user gesture (click/tap) due to browser autoplay policies
 * myButton.onclick = async () => {
 *   await sonic.init();
 *   await sonic.loadSynthDef('sonic-pi-beep');
 *   sonic.send('/s_new', 'sonic-pi-beep', -1, 0, 0, 'note', 60);
 * };
 *
 * @example
 * // Setup + message listeners
 * import { SuperSonic } from 'supersonic-scsynth';
 *
 * const sonic = new SuperSonic({ baseURL: '/dist/' });
 *
 * sonic.on('setup', async () => {
 *   await sonic.loadSynthDef('beep');
 * });
 *
 * sonic.on('in', (msg) => {
 *   console.log('OSC from the engine:', msg[0], msg.slice(1));
 * });
 *
 * await sonic.init();
 * sonic.send('/s_new', 'beep', 1001, 0, 0, 'freq', 440);
 */
export class SuperSonic {
  /**
   * Create a new SuperSonic instance.
   *
   * Does not start the engine — call {@link init} to boot.
   *
   * @param options - Configuration options. Needs `baseURL`, or else `workerBaseURL` together with `coreBaseURL` or `wasmBaseURL`.
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
   * const msg = SuperSonic.osc.encodeMessage('/s_new', ['beep', 1001, 0, 0]);
   * const decoded = SuperSonic.osc.decode(msg);
   */
  static osc: typeof osc;

  /**
   * Get the metrics schema describing all available metrics.
   *
   * Includes array offsets for zero-allocation reading via {@link getMetricsArray},
   * metric types/units/descriptions, and a declarative UI layout used by the
   * `<clockwork-metrics>` web component.
   *
   * See docs/METRICS_COMPONENT.md for the metrics component guide.
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

  /**
   * Session-timeline service: tempo, beat origin, transport, NTP "now."
   * See {@link ClockworkClock} for the full API surface. Undefined until the first {@link init}.
   */
  get clock(): ClockworkClock | undefined;

  /**
   * The Web MIDI manager, when MIDI is enabled (the `midi` option, or {@link enableMidi}) and came up.
   * Null when not enabled, before {@link init}, or when it could not come up ({@link midiError} says why).
   */
  get midi(): object | null;

  /** The gamepad manager, when the `gamepad` option is on and it came up; otherwise null ({@link gamepadError} says why). */
  get gamepad(): object | null;

  /** Why {@link midi} is null although MIDI was asked for: the error its start-up threw. Null otherwise. */
  get midiError(): unknown;

  /** Why {@link gamepad} is null although it was asked for: the error its start-up threw. Null otherwise. */
  get gamepadError(): unknown;

  /**
   * AudioWorkletNode wrapper for custom audio routing.
   *
   * Use `node.connect()` / `node.disconnect()` to route audio.
   * Use `node.input` to connect external audio sources into the engine.
   * Null before {@link init}.
   *
   * @example
   * // Route the engine's output through an AnalyserNode:
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


  /**
   * Map of loaded SynthDef names to their binary data — live, not a copy. SynthDefs appear after a `/d_recv`
   * through `send()` or `loadSynthDef()`, and are removed on `/d_free` or `/d_freeAll`. Kept for restoring after
   * `reload()`; cleared by `shutdown()`.
   */
  get loadedSynthDefs(): Map<string, Uint8Array>;

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
   * const unsub = sonic.on('in', (msg) => {
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
   * Returns an unsubscribe function (matching {@link on}).
   * @param event - Event name
   * @param callback - Handler function
   * @returns Unsubscribe function — call it to remove the listener before it fires
   */
  once<E extends SuperSonicEvent>(event: E, callback: SuperSonicEventMap[E]): () => void;

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
   * Safe to call multiple times: a call while booting gets the same boot,
   * and a call once booted does nothing.
   * Call it from a user gesture (click/tap): browsers let audio start only inside one.
   *
   * @throws If required browser features are missing or WASM fails to load.
   *   What was built is taken down again, the engine state becomes `'error'`,
   *   `'error'` is emitted, and `init()` can be called again.
   *
   * @example
   * await sonic.init();
   * // Engine is now ready to send/receive OSC
   */
  init(): Promise<void>;

  /**
   * Shut down the engine. The instance can be re-initialised with {@link init}.
   *
   * Terminates workers and releases memory, and closes the AudioContext if
   * SuperSonic made it (one passed as the `audioContext` option is left open).
   * Forgets the loaded synthdefs and buffers. Emits `'shutdown'` when the
   * engine was running or booting.
   */
  shutdown(): Promise<void>;

  /**
   * Destroy the engine completely. The instance cannot be re-used.
   *
   * Emits `'destroy'`, calls {@link shutdown}, then clears the WASM cache and
   * all event listeners.
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
   * Call it from a user gesture (click/tap). Before anything else it makes a
   * spare AudioContext — browsers let audio start only inside a gesture, and a
   * context that iOS hands back after an interruption may never render again.
   * If the quick resume fails, the reload moves onto the spare; otherwise the
   * spare is closed.
   *
   * @returns true if audio is running after recovery; false if the engine was
   *   not initialised or the reload failed
   *
   * @example
   * sonic.on('audiocontext:suspended', () => showResumeButton());
   * resumeButton.onclick = async () => {
   *   if (await sonic.recover()) hideResumeButton();
   * };
   */
  recover(): Promise<boolean>;

  /**
   * Quick resume. If the AudioContext is already running, only checks that the
   * audio thread is alive: nothing is purged, resynced or emitted. Otherwise it
   * starts the context, calls {@link purge} to drop what queued while it slept,
   * restarts the drift timer and, if the audio thread is running, resyncs timing
   * and emits `'resumed'`.
   *
   * Memory, node tree, and loaded synthdefs are preserved. Does not emit `'setup'`.
   * Use when you know the worklet is still running (e.g. tab was briefly backgrounded).
   * Call it from a user gesture: browsers let audio start only inside one.
   *
   * @returns true if the audio thread is running after resume; false if not, or if the engine is not initialised
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
   * Emits `'reload:start'`, then `'setup'` (so you can rebuild groups, FX
   * chains, and bus routing), `'ready'` and `'reload:complete'`. On failure
   * it takes down what it built, emits `'reload:failed'` and
   * `'reload:complete'` with `success: false`, and resolves to false.
   * A call while a reload is under way gets that reload.
   * Use when the worklet was killed (e.g. long background, browser reclaimed memory).
   *
   * @param options - Reload options
   * @param options.audioContext - A context to reload onto instead of the
   *   current one (what {@link recover} passes). It becomes SuperSonic's own,
   *   closed with the engine.
   * @returns true if reload succeeded; false if it failed or the engine was not initialised
   */
  reload(options?: { audioContext?: AudioContext | null }): Promise<boolean>;

  // ──────────────────────────────────────────────────────────────────────────
  // State observation
  // ──────────────────────────────────────────────────────────────────────────

  /**
   * Returns true if the engine has finished booting and is ready to send
   * and receive messages.
   *
   * Mirrors the C++ `ClockworkEngine::isRunning()` accessor; returns the
   * same value as the `initialized` getter, exposed as a method to match
   * the C++ API shape.
   */
  isRunning(): boolean;

  /**
   * Returns the current engine lifecycle state.
   *
   * One of:
   *   - `'stopped'` — before `init()` or after `shutdown()`/`destroy()`.
   *   - `'booting'` — while `init()` is in progress.
   *   - `'running'` — up. Whether the audio itself is running is the AudioContext's (see the `audiocontext:*` events).
   *   - `'restarting'` — while `reload()` rebuilds the worklet and engine.
   *   - `'error'` — the last `init()` or `reload()` failed; `init()` or `reset()` tries again.
   *
   * The same states, in the same words, as the C++ `ClockworkEngine::engineState()`. Every change is emitted as
   * `statechange`.
   */
  getEngineState(): EngineState;

  // ──────────────────────────────────────────────────────────────────────────
  // OSC Messaging
  // ──────────────────────────────────────────────────────────────────────────

  // ── Top-level commands ─────────────────────────────────────────────

  /** Query server status. Replies with `/status.reply`: unused, numUGens, numSynths, numGroups, numSynthDefs, avgCPU%, peakCPU%, nominalSampleRate, actualSampleRate. */
  send(address: '/status'): void;
  /** Query server version. Replies with `/version.reply`: programName, majorVersion, minorVersion, patchVersion, gitBranch, commitHash. */
  send(address: '/version'): void;
  /** Register (1) or unregister (0) for server notifications (`/n_go`, `/n_end`, `/n_on`, `/n_off`, `/n_move`). Replies with `/done /notify clientID [maxLogins]`. */
  send(address: '/notify', flag: 0 | 1, clientID?: number): void;
  /** Enable/disable OSC message dumping to debug output. 0=off, 1=parsed, 2=hex, 3=both. */
  send(address: '/dumpOSC', flag: 0 | 1 | 2 | 3): void;
  /** Async. Wait for all prior async commands to complete. Replies with `/synced syncID`. */
  send(address: '/sync', syncID: number): void;
  /** Query realtime memory usage. Replies with `/rtMemoryStatus.reply`: freeBytes, largestFreeBlockBytes. */
  send(address: '/rtMemoryStatus'): void;

  // ── SynthDef commands ──────────────────────────────────────────────

  /** Async. Load a compiled synthdef from bytes. Optional completionMessage is an encoded OSC message executed after loading. Replies with `/done /d_recv`. */
  send(address: '/d_recv', bytes: Uint8Array | ArrayBuffer, completionMessage?: Uint8Array | ArrayBuffer): void;
  /** Free one or more loaded synthdefs by name. */
  send(address: '/d_free', ...names: [string, ...string[]]): void;
  /** Free all loaded synthdefs. Not in the official SC reference but supported by scsynth. */
  send(address: '/d_freeAll'): void;

  // ── Synth commands ─────────────────────────────────────────────────

  /** Create a new synth from a loaded synthdef. addAction: 0=head, 1=tail, 2=before, 3=after, 4=replace. Controls are alternating name/index and value pairs. Values can be numbers or bus mapping strings like `"c0"` (control bus 0) or `"a0"` (audio bus 0). Use nodeID=-1 for auto-assign. */
  send(address: '/s_new', defName: string, nodeID: NodeID, addAction: AddAction, targetID: NodeID, ...controls: (string | number)[]): void;
  /** Get synth control values. Controls can be indices or names. Replies with `/n_set nodeID control value ...`. */
  send(address: '/s_get', nodeID: NodeID, ...controls: (string | number)[]): void;
  /** Get sequential synth control values. Control can be an index or name. Replies with `/n_setn nodeID control count values...`. For multiple ranges, use the catch-all overload. */
  send(address: '/s_getn', nodeID: NodeID, control: number | string, count: number): void;
  /** Release client-side synth ID tracking. Synths continue running but are reassigned to reserved negative IDs. Use when you no longer need to communicate with the synth and want to reuse the ID. */
  send(address: '/s_noid', ...nodeIDs: [NodeID, ...NodeID[]]): void;

  // ── Node commands ──────────────────────────────────────────────────

  /** Free (delete) one or more nodes. */
  send(address: '/n_free', ...nodeIDs: [NodeID, ...NodeID[]]): void;
  /** Set node control values. Controls are alternating name/index and value pairs. If the node is a group, sets the control on all nodes in the group. */
  send(address: '/n_set', nodeID: NodeID, ...controls: (string | number)[]): void;
  /** Set sequential control values starting at the given control index/name. For multiple ranges, use the catch-all overload. */
  send(address: '/n_setn', nodeID: NodeID, control: number | string, count: number, ...values: number[]): void;
  /** Fill sequential controls with a single value. For multiple ranges, use the catch-all overload. */
  send(address: '/n_fill', nodeID: NodeID, control: number | string, count: number, value: number): void;
  /** Turn nodes on (1) or off (0). Args are repeating [nodeID, flag] pairs. */
  send(address: '/n_run', ...pairs: [NodeID, 0 | 1, ...(NodeID | 0 | 1)[]]): void;
  /** Move nodeA to execute immediately before nodeB. Args are repeating [nodeA, nodeB] pairs. */
  send(address: '/n_before', ...pairs: [NodeID, NodeID, ...NodeID[]]): void;
  /** Move nodeA to execute immediately after nodeB. Args are repeating [nodeA, nodeB] pairs. */
  send(address: '/n_after', ...pairs: [NodeID, NodeID, ...NodeID[]]): void;
  /** Reorder nodes within a group. addAction: 0=head, 1=tail, 2=before target, 3=after target. Does not support 4 (replace). */
  send(address: '/n_order', addAction: 0 | 1 | 2 | 3, targetID: NodeID, ...nodeIDs: [NodeID, ...NodeID[]]): void;
  /** Query node info. Replies with `/n_info` for each node: nodeID, parentGroupID, prevNodeID, nextNodeID, isGroup, [headNodeID, tailNodeID]. */
  send(address: '/n_query', ...nodeIDs: [NodeID, ...NodeID[]]): void;
  /** Print control values and calculation rates for each node to debug output. No reply message. */
  send(address: '/n_trace', ...nodeIDs: [NodeID, ...NodeID[]]): void;
  /** Map controls to read from control buses. Mappings are repeating [control, busIndex] pairs. Set busIndex to -1 to unmap. */
  send(address: '/n_map', nodeID: NodeID, ...mappings: (string | number)[]): void;
  /** Map a range of sequential controls to sequential control buses. Mappings are repeating [control, busIndex, count] triplets. */
  send(address: '/n_mapn', nodeID: NodeID, ...mappings: (string | number)[]): void;
  /** Map controls to read from audio buses. Mappings are repeating [control, busIndex] pairs. Set busIndex to -1 to unmap. */
  send(address: '/n_mapa', nodeID: NodeID, ...mappings: (string | number)[]): void;
  /** Map a range of sequential controls to sequential audio buses. Mappings are repeating [control, busIndex, count] triplets. */
  send(address: '/n_mapan', nodeID: NodeID, ...mappings: (string | number)[]): void;

  // ── Group commands ─────────────────────────────────────────────────

  /** Create new groups. Args are repeating [groupID, addAction, targetID] triplets. addAction: 0=head, 1=tail, 2=before, 3=after, 4=replace. */
  send(address: '/g_new', ...args: [NodeID, AddAction, NodeID, ...(NodeID | AddAction)[]]): void;
  /** Create new parallel groups (children evaluated in unspecified order). Same signature as /g_new. */
  send(address: '/p_new', ...args: [NodeID, AddAction, NodeID, ...(NodeID | AddAction)[]]): void;
  /** Free all immediate children of one or more groups (groups themselves remain). */
  send(address: '/g_freeAll', ...groupIDs: [NodeID, ...NodeID[]]): void;
  /** Recursively free all synths inside one or more groups and their nested sub-groups. */
  send(address: '/g_deepFree', ...groupIDs: [NodeID, ...NodeID[]]): void;
  /** Move node to head of group. Args are repeating [groupID, nodeID] pairs. */
  send(address: '/g_head', ...pairs: [NodeID, NodeID, ...NodeID[]]): void;
  /** Move node to tail of group. Args are repeating [groupID, nodeID] pairs. */
  send(address: '/g_tail', ...pairs: [NodeID, NodeID, ...NodeID[]]): void;
  /** Print group's node tree to debug output. Args are repeating [groupID, flag] pairs. flag: 0=structure only, non-zero=include control values. No reply message. */
  send(address: '/g_dumpTree', ...groupFlagPairs: [NodeID, number, ...(NodeID | number)[]]): void;
  /** Query group tree structure. Args are repeating [groupID, flag] pairs. flag: 0=structure only, non-zero=include control values. Replies with `/g_queryTree.reply`. */
  send(address: '/g_queryTree', ...groupFlagPairs: [NodeID, number, ...(NodeID | number)[]]): void;

  // ── UGen commands ──────────────────────────────────────────────────

  /** Send a command to a specific UGen instance within a synth. The command name and args are UGen-specific. */
  send(address: '/u_cmd', nodeID: NodeID, ugenIndex: number, command: string, ...args: OscArg[]): void;

  // ── Buffer commands ────────────────────────────────────────────────

  /** Async. Allocate an empty buffer. Queued and rewritten to /b_allocPtr internally. Use sync() after to ensure completion. Replies with `/done /b_allocPtr bufnum`. Note: completion messages are not supported (dropped during rewrite). */
  send(address: '/b_alloc', bufnum: number, numFrames: number, numChannels?: number, sampleRate?: number): void;
  /** Async. Allocate a buffer and read an audio file into it. The path is fetched via the configured sampleBaseURL. Queued and rewritten internally. Replies with `/done /b_allocPtr bufnum`. */
  send(address: '/b_allocRead', bufnum: number, path: string, startFrame?: number, numFrames?: number): void;
  /** Async. Allocate a buffer and read specific channels from an audio file. Queued and rewritten internally. Replies with `/done /b_allocPtr bufnum`. */
  send(address: '/b_allocReadChannel', bufnum: number, path: string, startFrame: number, numFrames: number, ...channels: number[]): void;
  /** Async. SuperSonic extension: allocate a buffer from inline audio file bytes (WAV, FLAC, OGG, etc.) without URL fetch. Queued and rewritten internally. Replies with `/done /b_allocPtr bufnum`. */
  send(address: '/b_allocFile', bufnum: number, data: Uint8Array | ArrayBuffer): void;
  /** Async. Free a buffer. Optional completionMessage is an encoded OSC message executed after freeing. Replies with `/done /b_free bufnum`. */
  send(address: '/b_free', bufnum: number, completionMessage?: Uint8Array | ArrayBuffer): void;
  /** Async. Zero a buffer's sample data. Optional completionMessage is an encoded OSC message executed after zeroing. Replies with `/done /b_zero bufnum`. */
  send(address: '/b_zero', bufnum: number, completionMessage?: Uint8Array | ArrayBuffer): void;
  /** Query buffer info. Replies with `/b_info` for each buffer: bufnum, numFrames, numChannels, sampleRate. */
  send(address: '/b_query', ...bufnums: [number, ...number[]]): void;
  /** Get individual sample values. Replies with `/b_set bufnum index value ...`. */
  send(address: '/b_get', bufnum: number, ...sampleIndices: [number, ...number[]]): void;
  /** Set individual buffer samples. Args are repeating [index, value] pairs after bufnum. */
  send(address: '/b_set', bufnum: number, ...indexValuePairs: number[]): void;
  /** Set sequential buffer samples starting at startIndex. For multiple ranges, use the catch-all overload. */
  send(address: '/b_setn', bufnum: number, startIndex: number, count: number, ...values: number[]): void;
  /** Get sequential sample values. Replies with `/b_setn bufnum startIndex count values...`. For multiple ranges, use the catch-all overload. */
  send(address: '/b_getn', bufnum: number, startIndex: number, count: number): void;
  /** Fill sequential buffer samples with a single value. For multiple ranges, use the catch-all overload. */
  send(address: '/b_fill', bufnum: number, startIndex: number, count: number, value: number): void;
  /** Async. Generate buffer contents. Commands: "sine1", "sine2", "sine3", "cheby", "copy". Flags (for sine/cheby): 1=normalize, 2=wavetable, 4=clear (OR together, e.g. 7=all). Replies with `/done /b_gen bufnum`. */
  send(address: '/b_gen', bufnum: number, command: string, ...args: OscArg[]): void;

  // ── Control bus commands ───────────────────────────────────────────

  /** Set control bus values. Args are repeating [busIndex, value] pairs. */
  send(address: '/c_set', ...busIndexValuePairs: number[]): void;
  /** Get control bus values. Replies with `/c_set index value ...`. */
  send(address: '/c_get', ...busIndices: [number, ...number[]]): void;
  /** Set sequential control bus values starting at startIndex. For multiple ranges, use the catch-all overload. */
  send(address: '/c_setn', startIndex: number, count: number, ...values: number[]): void;
  /** Get sequential control bus values. Replies with `/c_setn startIndex count values...`. For multiple ranges, use the catch-all overload. */
  send(address: '/c_getn', startIndex: number, count: number): void;
  /** Fill sequential control buses with a single value. For multiple ranges, use the catch-all overload. */
  send(address: '/c_fill', startIndex: number, count: number, value: number): void;

  // ── Catch-all ──────────────────────────────────────────────────────

  /**
   * Send an OSC message to the engine.
   *
   * This is the primary way to communicate with the engine. Arguments are
   * automatically encoded to OSC format. The typed overloads cover scsynth's
   * commands; this one takes any other address, and the multi-range forms of
   * commands like `/n_setn`, `/b_fill` and `/c_getn`.
   *
   * Sent at once, except buffer allocation (`/b_alloc`, `/b_allocRead`,
   * `/b_allocReadChannel`, `/b_allocFile`): those are queued, in the order
   * written, while their material is fetched and decoded, then reach the
   * engine as `/b_allocPtr`. A queued command that fails is reported on
   * `'error'`. {@link sync} waits for the queue before its barrier.
   *
   * Nothing is refused here: a command the browser cannot serve (a file-path
   * load, a file write) gets the engine's own `/fail`. A `/d_recv`, `/d_free`
   * or `/d_freeAll` sent this way also updates {@link loadedSynthDefs}.
   *
   * @param address - OSC address pattern (e.g. `'/s_new'`, `'/n_set'`)
   * @param args - Message arguments
   * @throws If the engine is not initialised
   * @throws If a buffer allocation command is malformed (checked before it is queued)
   * @throws If the message is larger than the IN ring
   *
   * @example
   * // Create a synth
   * sonic.send('/s_new', 'beep', 1001, 0, 0, 'freq', 440);
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
   * // Buffer commands are queued; sync() waits for them, then for the engine:
   * sonic.send('/b_alloc', 0, 44100, 1);
   * await sonic.sync();
   */
  send(address: string, ...args: OscArg[]): void;

  /**
   * Send pre-encoded OSC bytes to the engine.
   *
   * Use this when you've already encoded the message (e.g. via `SuperSonic.osc.encodeMessage`)
   * or when sending from a worker that produces raw OSC. Sends bytes as-is without
   * rewriting — buffer allocation commands (`/b_alloc*`) are not transformed.
   * Use {@link send} for buffer commands so they are handled correctly. A
   * `/d_recv` sent this way is not recorded in {@link loadedSynthDefs}, so it
   * is not restored after a reload.
   *
   * @param oscData - Encoded OSC message or bundle bytes
   * @throws If the engine is not initialised
   * @throws If the message exceeds the IN ring size
   *
   * @example
   * const msg = SuperSonic.osc.encodeMessage('/n_set', [1001, 'freq', 880]);
   * sonic.sendOSC(msg);
   */
  sendOSC(oscData: Uint8Array | ArrayBuffer): void;

  /**
   * Send a message and wait for its reply.
   *
   * Resolves with the decoded reply: the first incoming message at `reply`
   * (for which `match` is true, when given). Rejects when a `/fail` arrives
   * first — scsynth's word for a refusal; name another address with `error`,
   * or pass `error: null` to wait only for the reply — and on the timeout.
   * The rejection's Error carries the refusal as `reply`.
   *
   * @param address - The command to send
   * @param args - Its arguments
   * @param options - What to wait for
   * @param options.reply - The address the answer comes on
   * @param options.error - The address a refusal comes on (default `'/fail'`; null for none)
   * @param options.match - Narrows the reply and the refusal, e.g. by an ID
   * @param options.timeoutMs - How long to wait (default 10000)
   * @throws If the engine is not initialised, or `reply` is missing (thrown, not a rejection)
   *
   * @example
   * const [, , numUGens, numSynths] = await sonic.request('/status', [], { reply: '/status.reply' });
   */
  request(
    address: string,
    args: OscArg[],
    options: {
      reply: string;
      error?: string | null;
      match?: (msg: OscMessage) => boolean;
      timeoutMs?: number;
    },
  ): Promise<OscMessage>;

  /**
   * Flush all pending scheduled OSC: clears the engine's scheduler and the IN
   * ring so nothing already in flight will fire. Resolves when the worklet
   * confirms — or after a second without an answer, when it emits `'warning'`
   * (the worklet may be gone).
   */
  purge(): Promise<void>;

  /**
   * Create an OscChannel for direct worker-to-worklet communication.
   *
   * The returned channel can be transferred to a Web Worker, allowing that
   * worker to send OSC directly to the AudioWorklet without going through
   * the main thread. Works in both SAB and postMessage modes.
   *
   * For AudioWorkletProcessor use, import from `'supersonic-scsynth/osc-channel'`
   * which avoids DOM APIs unavailable in the worklet scope.
   *
   * See docs/WORKERS.md for the full workers guide.
   *
   * @param options - Channel options
   * @param options.sourceId - The channel's numeric source ID, shown on
   *   `'out:osc'` (0 is the main thread's). Default: the next unused, from 1.
   * @throws If the engine is not initialised
   *
   * @example
   * const channel = sonic.createOscChannel();
   * myWorker.postMessage(
   *   { channel: channel.transferable },
   *   channel.transferList,
   * );
   */
  createOscChannel(options?: { sourceId?: number }): OscChannel;

  /**
   * Get the next unique node ID.
   *
   * Thread-safe — can be called concurrently from multiple workers and no
   * two callers will ever receive the same ID. IDs start at 1000: 0 is the
   * root group and 1–999 are left for the client to assign by hand.
   *
   * Also available on {@link OscChannel} for use in Web Workers.
   *
   * @throws If the engine is not initialised
   *
   * @returns A unique node ID (>= 1000)
   *
   * @example
   * const id = sonic.nextNodeId();
   * sonic.send('/s_new', 'beep', id, 0, 0, 'freq', 440);
   */
  nextNodeId(): number;

  // ──────────────────────────────────────────────────────────────────────────
  // Asset Loading
  // ──────────────────────────────────────────────────────────────────────────

  /**
   * Load a SynthDef into the engine.
   *
   * Accepts multiple source types:
   * - **Name string** — fetched from `synthdefBaseURL` (e.g. `'beep'` → `synthdefBaseURL/beep.scsyndef`)
   * - **Path/URL string** — fetched directly (one containing `/` or `\`, starting with `http`, or ending in `.scsyndef`)
   * - **ArrayBuffer / Uint8Array** — raw synthdef bytes
   * - **File / Blob** — e.g. from a file input
   *
   * The definition is sent as `/d_recv` through {@link send}, so it is recorded
   * in {@link loadedSynthDefs} and restored after a reload. Resolves once it
   * has been sent; {@link sync} after it waits for the engine to reach it.
   *
   * @param source - SynthDef name, path/URL, raw bytes, or File/Blob
   * @returns The extracted name and byte size
   * @throws If the source type is invalid, the synthdef can't be parsed, a
   *   name is given with no `synthdefBaseURL`, or the fetch fails
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
   * Load several SynthDefs in parallel, each as {@link loadSynthDef} would.
   *
   * @param names - SynthDef names (or paths/URLs)
   * @returns Each one's name and byte size, in the order given. Rejects with
   *   the first failure; the others may still have loaded.
   *
   * @example
   * const loaded = await sonic.loadSynthDefs(['beep', 'pad', 'kick']);
   * console.log(loaded.map((d) => d.name));
   */
  loadSynthDefs(names: string[]): Promise<LoadSynthDefResult[]>;

  /**
   * Load an audio sample into a buffer slot.
   *
   * Decodes the audio file (WAV, AIFF, etc.) and copies the samples into
   * the sample buffer pool. Resolves once the engine has the buffer, which is
   * then available for use with `PlayBuf`, `BufRd`, etc. A bare filename is
   * resolved against `sampleBaseURL`.
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
   * sonic.send('/s_new', 'sampler', 1001, 0, 0, 'bufnum', 0);
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
   * Get sample metadata (including content hash) without allocating a buffer.
   *
   * Fetches, decodes, and hashes the audio, returning the same info that
   * would appear in the {@link loadSample} result if the content were loaded.
   * No buffer slot is consumed and no OSC is sent to the engine.
   *
   * Use this to inspect content or check for duplicates before loading.
   *
   * @param source - Sample path/URL, raw bytes, or File/Blob
   * @param startFrame - First frame to read (default: 0)
   * @param numFrames - Number of frames to read (default: 0 = all)
   * @returns Sample metadata including hash, frame count, channels, sample rate, and duration
   *
   * @example
   * const info = await sonic.sampleInfo('kick.wav');
   * console.log(info.hash, info.duration, info.numChannels);
   *
   * const loaded = sonic.getLoadedBuffers();
   * if (loaded.some(b => b.hash === info.hash)) {
   *   console.log('Already loaded');
   * }
   */
  sampleInfo(
    source: string | ArrayBuffer | ArrayBufferView | Blob,
    startFrame?: number,
    numFrames?: number,
  ): Promise<SampleInfo>;

  /**
   * A barrier: resolves once everything sent before it has reached the engine, in order.
   *
   * Waits for the buffer commands {@link send} has queued, then sends
   * `/clockwork/sync` and waits for the matching `/clockwork/synced`, which the
   * audio thread answers when it reaches the message. (Not scsynth's `/sync`:
   * send that yourself, or use {@link request}, for scsynth's own barrier.)
   * In postMessage mode it then waits two snapshot intervals, so metrics and
   * the node tree have caught up. Use after loading synthdefs or buffers,
   * before creating synths that use them.
   *
   * @param syncId - Optional custom sync ID (random if omitted)
   * @param timeoutMs - How long to wait for the answer (default 10000)
   * @throws Rejects on the timeout, or if the engine shuts down first.
   *
   * @example
   * await sonic.loadSynthDef('beep');
   * await sonic.sync();
   * sonic.send('/s_new', 'beep', 1001, 0, 0);
   */
  sync(syncId?: number, timeoutMs?: number): Promise<void>;

  /**
   * Allocate an empty buffer, as {@link loadSample} does for a file: resolves
   * once the engine has it.
   *
   * @param bufnum - Buffer slot number (0 to numBuffers-1)
   * @param numFrames - Frames to allocate
   * @param numChannels - Channels (default 1)
   * @param sampleRate - Sample rate in Hz (default: the AudioContext's)
   * @returns The buffer's number and shape
   *
   * @example
   * const buf = await sonic.allocSample(1, 48000, 2);
   */
  allocSample(
    bufnum: number,
    numFrames: number,
    numChannels?: number,
    sampleRate?: number | null,
  ): Promise<{ bufnum: number; numFrames: number; numChannels: number; sampleRate: number }>;

  /**
   * Bring Web MIDI up after {@link init}.
   *
   * For a page that asks for MIDI when the player does: some browsers prompt
   * for it, so ask from the gesture that wants it. Rebuilds the MIDI and
   * gamepad managers, so calling it again re-acquires MIDI.
   *
   * @param options - `true`, or the MIDI manager's options (as the `midi` option)
   * @returns The MIDI manager ({@link midi}), or null if it could not come up
   *   ({@link midiError} says why)
   */
  enableMidi(options?: boolean | Record<string, unknown>): Promise<object | null>;

  // ──────────────────────────────────────────────────────────────────────────
  // Metrics & Monitoring
  // ──────────────────────────────────────────────────────────────────────────

  /**
   * Get current metrics as a named object.
   *
   * A local memory read in both SAB and postMessage modes — no IPC. Safe to
   * call from `requestAnimationFrame`. For reading without allocating, see
   * {@link getMetricsArray}.
   *
   * See docs/METRICS.md for the full metrics guide.
   *
   * @example
   * const m = sonic.getMetrics();
   * console.log(`Messages sent: ${m.oscOutMessagesSent}`);
   * console.log(`Scheduler depth: ${m.engineSchedulerDepth}`);
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
   * Get a diagnostic snapshot with metrics (and their descriptions) and JS heap memory info.
   *
   * Useful for capturing state for bug reports or debugging timing issues.
   */
  getSnapshot(): Snapshot;

  /**
   * Copy the newest `frames` frames of a ScopeOut2 scope stream.
   *
   * Scope slots are lossless interleaved rings with a monotonic write
   * cursor (see docs/scope-streams-sample-clock.md); this returns the
   * window ending at the current cursor, zero-filled at the start when
   * fewer frames are available. Allocates the output per call.
   * SAB mode only; returns null when uninitialised, out of range, the
   * slot is inactive, or nothing has been written to it yet.
   *
   * @param scopeNum - Scope slot index, from 0
   * @param frames - Window length in frames (default 1024; at most the ring's length)
   */
  getScope(scopeNum: number, frames?: number): {
    frames: number;
    channels: number;
    writePosition: bigint;
    interleaved: Float32Array;
  } | null;

  /**
   * List the scope slots in use. SAB mode only; empty otherwise.
   */
  getScopes(): Array<{ index: number; channels: number }>;

  /**
   * Scope geometry: slot count, per-slot ring frames, channels. These are the
   * web build's compiled-in defaults, not read from the running engine.
   */
  static getScopeSchema(): {
    maxScopes: number;
    ringFrames: number;
    channels: number;
  };

  /**
   * Get a comprehensive system performance report.
   *
   * Includes hardware info, audio configuration, Chrome playbackStats (if available),
   * a cross-browser audio health percentage, and a human-readable health assessment.
   * Useful for diagnosing audio crackling on constrained hardware.
   *
   * @throws If the engine is not initialised
   *
   * @example
   * const report = sonic.getSystemReport();
   * console.log(report.health.summary);
   * if (report.health.audioHealthPct < 95) {
   *   console.warn('Audio thread struggling:', report.health.issues);
   * }
   */
  getSystemReport(): SystemReport;

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
   * The mirror has a default capacity of 1024 nodes. If exceeded,
   * `droppedCount` will be non-zero and the tree may be incomplete,
   * but audio continues normally.
   *
   * @example
   * const tree = sonic.getTree();
   * function printTree(node, indent = 0) {
   *   const prefix = '  '.repeat(indent);
   *   const label = node.type === 'synth' ? node.defName : 'group';
   *   console.log(`${prefix}[${node.id}] ${label}`);
   *   for (const child of node.children) printTree(child, indent + 1);
   * }
   * if (tree.root) printTree(tree.root);
   */
  getTree(): Tree;

  // ──────────────────────────────────────────────────────────────────────────
  // Timing
  // ──────────────────────────────────────────────────────────────────────────

  /**
   * Set clock offset for multi-system sync (e.g. against an NTP server).
   *
   * Shifts all scheduled bundle execution times by the specified offset.
   * Positive values mean the shared/server clock is ahead of local time.
   *
   * @param offsetS - Offset in seconds (stored rounded to the millisecond)
   * @throws If the engine is not initialised
   */
  setClockOffset(offsetS: number): void;

  // ──────────────────────────────────────────────────────────────────────────
  // Audio Capture (SAB mode only)
  // ──────────────────────────────────────────────────────────────────────────

  /**
   * Start capturing what the engine sends to the audio device. SAB mode only.
   * @throws If the engine is not initialised, or not in SAB mode
   */
  startCapture(): void;

  /**
   * Stop capturing and return what was captured. A capture longer than the
   * ring ({@link getMaxCaptureDuration}) keeps the newest ring's worth.
   * @throws If the engine is not initialised, or not in SAB mode
   */
  stopCapture(): {
    sampleRate: number;
    channels: number;
    frames: number;
    /** Frames the ring overwrote before they were read. */
    lost: number;
    /** One array per channel. */
    channelData: Float32Array[];
    /** The first channel. */
    left: Float32Array;
    /** The second channel, or null. */
    right: Float32Array | null;
  };

  /** Check if audio capture is currently enabled. */
  isCaptureEnabled(): boolean;

  /** Get number of audio frames captured so far — or, with no capture running, written in all. */
  getCaptureFrames(): number;

  /** Get maximum capture duration in seconds: the length of the capture ring. */
  getMaxCaptureDuration(): number;

  // ──────────────────────────────────────────────────────────────────────────
  // Info
  // ──────────────────────────────────────────────────────────────────────────

  /**
   * Get engine info: sample rate, memory layout, capabilities, and version.
   *
   * @throws If the engine is not initialised
   *
   * @example
   * const info = sonic.getInfo();
   * console.log(`Sample rate: ${info.sampleRate}Hz`);
   * console.log(`Boot time: ${info.bootTimeMs}ms`);
   * console.log(`Version: ${info.version}`);
   */
  getInfo(): SuperSonicInfo;
}

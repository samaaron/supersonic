// Type tests for supersonic.d.ts — run with `npx tsd`
import { expectType, expectAssignable, expectNotAssignable, expectError } from 'tsd';
import type {
  OscArg,
  OscMessage,
  OscBundle,
  NTPTimeTag,
  OscBundlePacket,
  TransportMode,
  ScsynthOptions,
  SuperSonicOptions,
  ActivityLineConfig,
  SuperSonicMetrics,
  MetricDefinition,
  MetricsSchema,
  TreeNode,
  Tree,
  RawTreeNode,
  RawTree,
  SuperSonicInfo,
  Snapshot,
  SampleInfo,
  LoadedBufferInfo,
  LoadSynthDefResult,
  LoadSampleResult,
  BootStats,
  SuperSonicEventMap,
  SuperSonicEvent,
  OscCategory,
  OscChannelMetrics,
  OscChannelSABTransferable,
  OscChannelPMTransferable,
  OscChannelTransferable,
  SendOSCOptions,
  AddAction,
  BlockedCommand,
} from './supersonic';
import { SuperSonic, OscChannel, osc } from './supersonic';

// ============================================================================
// Section 1: OSC Types
// ============================================================================

// OscArg — all 5 primitive types
expectAssignable<OscArg>(42);
expectAssignable<OscArg>('hello');
expectAssignable<OscArg>(true);
expectAssignable<OscArg>(new Uint8Array([1, 2, 3]));
expectAssignable<OscArg>(new ArrayBuffer(8));

// OscArg — all 8 tagged object forms
expectAssignable<OscArg>({ type: 'int' as const, value: 42 });
expectAssignable<OscArg>({ type: 'float' as const, value: 440.0 });
expectAssignable<OscArg>({ type: 'string' as const, value: 'hello' });
expectAssignable<OscArg>({ type: 'blob' as const, value: new Uint8Array([1]) });
expectAssignable<OscArg>({ type: 'blob' as const, value: new ArrayBuffer(4) });
expectAssignable<OscArg>({ type: 'bool' as const, value: true });
expectAssignable<OscArg>({ type: 'int64' as const, value: 9007199254740992n });
expectAssignable<OscArg>({ type: 'int64' as const, value: 42 });
expectAssignable<OscArg>({ type: 'double' as const, value: 3.14 });
expectAssignable<OscArg>({ type: 'timetag' as const, value: 123456 });

// OscArg — negative cases
expectNotAssignable<OscArg>(null);
expectNotAssignable<OscArg>(undefined);
expectNotAssignable<OscArg>(Symbol('x'));
expectNotAssignable<OscArg>({ type: 'invalid' as const, value: 42 });

// OscMessage — tuple shape
expectAssignable<OscMessage>(['/s_new', 'beep', 1001, 0, 1]);
expectAssignable<OscMessage>(['/n_free']);
expectType<string>(([] as unknown as OscMessage)[0]);

// OscBundle
declare const bundle: OscBundle;
expectType<number>(bundle.timeTag);
expectType<(OscMessage | OscBundle)[]>(bundle.packets);

// NTPTimeTag — valid forms
expectAssignable<NTPTimeTag>(123456.789);
expectAssignable<NTPTimeTag>(1);
expectAssignable<NTPTimeTag>(null);
expectAssignable<NTPTimeTag>(undefined);
expectAssignable<NTPTimeTag>([100, 200] as [number, number]);
expectNotAssignable<NTPTimeTag>('immediate');

// OscBundlePacket — all three forms
expectAssignable<OscBundlePacket>(['/s_new', 'beep', 1001] as OscMessage);
expectAssignable<OscBundlePacket>({ address: '/s_new', args: ['beep', 1001] });
expectAssignable<OscBundlePacket>({ address: '/s_new' });
expectAssignable<OscBundlePacket>({ timeTag: 1, packets: [['/n_set', 1001, 'freq', 880] as OscMessage] });

// ============================================================================
// Section 2: Configuration Types
// ============================================================================

// TransportMode
expectAssignable<TransportMode>('sab');
expectAssignable<TransportMode>('postMessage');
expectNotAssignable<TransportMode>('websocket');

// ScsynthOptions — all optional, special constraints
const scsynthOpts: ScsynthOptions = {};
expectAssignable<ScsynthOptions>({ bufLength: 128 });
expectNotAssignable<ScsynthOptions>({ bufLength: 256 });
expectAssignable<ScsynthOptions>({ loadGraphDefs: 0 });
expectAssignable<ScsynthOptions>({ loadGraphDefs: 1 });
expectNotAssignable<ScsynthOptions>({ loadGraphDefs: 2 });
expectAssignable<ScsynthOptions>({ numBuffers: 2048, maxNodes: 4096, verbosity: 3 });

// SuperSonicOptions — all optional
const ssOpts: SuperSonicOptions = {};
expectAssignable<SuperSonicOptions>({ mode: 'sab' });
expectAssignable<SuperSonicOptions>({ mode: 'postMessage' });
expectAssignable<SuperSonicOptions>({ scsynthOptions: { numBuffers: 2048 } });
expectAssignable<SuperSonicOptions>({ debug: true, debugScsynth: true, debugOscIn: false, debugOscOut: false });
expectAssignable<SuperSonicOptions>({ baseURL: '/dist/', coreBaseURL: '/core/', workerBaseURL: '/workers/' });
expectAssignable<SuperSonicOptions>({ wasmBaseURL: '/wasm/', wasmUrl: '/wasm/scsynth.wasm', workletUrl: '/worklet.js' });
expectAssignable<SuperSonicOptions>({ sampleBaseURL: '/samples/', synthdefBaseURL: '/synthdefs/' });
expectAssignable<SuperSonicOptions>({ snapshotIntervalMs: 100, preschedulerCapacity: 65536, bypassLookaheadMs: 500 });
expectAssignable<SuperSonicOptions>({ fetchMaxRetries: 3, fetchRetryDelay: 1000 });
expectAssignable<SuperSonicOptions>({ autoConnect: false });

// ActivityLineConfig
expectAssignable<ActivityLineConfig>({ maxLineLength: 200 });
expectAssignable<ActivityLineConfig>({ scsynthMaxLineLength: null, oscInMaxLineLength: 100 });
expectAssignable<SuperSonicOptions>({ activityEvent: { maxLineLength: 200 }, activityConsoleLog: {} });

// ============================================================================
// Section 3: Metrics Types
// ============================================================================

declare const metrics: SuperSonicMetrics;
expectType<number>(metrics.scsynthProcessCount);
expectType<number>(metrics.oscOutMessagesSent);
expectType<number>(metrics.oscOutBytesSent);
expectType<number>(metrics.oscInMessagesReceived);
expectType<number>(metrics.preschedulerPending);
expectType<number>(metrics.inBufferUsedBytes);
expectType<number>(metrics.driftOffsetMs);
expectType<number>(metrics.clockOffsetMs);
expectType<number>(metrics.audioContextState);
expectType<number>(metrics.mode);
expectType<number>(metrics.bypassNonBundle);
expectType<number>(metrics.ringBufferDirectWriteFails);
expectType<number>(metrics.bufferPoolUsedBytes);
expectType<number>(metrics.scsynthSchedulerMaxLateMs);

// MetricDefinition
declare const metricDef: MetricDefinition;
expectType<number>(metricDef.offset);
expectType<'counter' | 'gauge' | 'constant' | 'enum'>(metricDef.type);
expectType<string>(metricDef.description);

// MetricsSchema
declare const schema: MetricsSchema;
expectType<Record<keyof SuperSonicMetrics, MetricDefinition>>(schema.metrics);
expectType<number>(schema.sentinels.HEADROOM_UNSET);
expectType<string>(schema.layout.panels[0].title);

// ============================================================================
// Section 4: Node Tree Types
// ============================================================================

declare const treeNode: TreeNode;
expectType<number>(treeNode.id);
expectType<'group' | 'synth'>(treeNode.type);
expectType<string>(treeNode.defName);
expectType<TreeNode[]>(treeNode.children);

declare const tree: Tree;
expectType<number>(tree.nodeCount);
expectType<number>(tree.version);
expectType<number>(tree.droppedCount);
expectType<TreeNode>(tree.root);

declare const rawNode: RawTreeNode;
expectType<number>(rawNode.id);
expectType<number>(rawNode.parentId);
expectType<boolean>(rawNode.isGroup);
expectType<number>(rawNode.prevId);
expectType<number>(rawNode.nextId);
expectType<number>(rawNode.headId);
expectType<string>(rawNode.defName);

declare const rawTree: RawTree;
expectType<number>(rawTree.nodeCount);
expectType<number>(rawTree.version);
expectType<number>(rawTree.droppedCount);
expectType<RawTreeNode[]>(rawTree.nodes);

// ============================================================================
// Section 5: Info & Snapshot Types
// ============================================================================

declare const info: SuperSonicInfo;
expectType<number>(info.sampleRate);
expectType<number>(info.numBuffers);
expectType<number>(info.totalMemory);
expectType<number>(info.wasmHeapSize);
expectType<number>(info.bufferPoolSize);
expectType<number | null>(info.bootTimeMs);
expectType<string | null>(info.version);
expectType<boolean>(info.capabilities.audioWorklet);
expectType<boolean>(info.capabilities.sharedArrayBuffer);
expectType<boolean>(info.capabilities.crossOriginIsolated);
expectType<boolean>(info.capabilities.atomics);
expectType<boolean>(info.capabilities.webWorker);

declare const snapshot: Snapshot;
expectType<string>(snapshot.timestamp);
expectType<Record<string, { value: number; description?: string }>>(snapshot.metrics);
expectType<RawTree>(snapshot.nodeTree);

declare const sampleInfo: SampleInfo;
expectType<string>(sampleInfo.hash);
expectType<string | null>(sampleInfo.source);
expectType<number>(sampleInfo.numFrames);
expectType<number>(sampleInfo.numChannels);
expectType<number>(sampleInfo.sampleRate);
expectType<number>(sampleInfo.duration);

declare const loadedBuf: LoadedBufferInfo;
expectType<number>(loadedBuf.bufnum);
expectType<string>(loadedBuf.hash);

declare const loadResult: LoadSynthDefResult;
expectType<string>(loadResult.name);
expectType<number>(loadResult.size);

declare const sampleResult: LoadSampleResult;
expectType<number>(sampleResult.bufnum);
expectType<string>(sampleResult.hash);
expectType<number>(sampleResult.numFrames);

declare const bootStats: BootStats;
expectType<number | null>(bootStats.initStartTime);
expectType<number | null>(bootStats.initDuration);

// ============================================================================
// Section 6: Event Types
// ============================================================================

// All 18 event names are valid SuperSonicEvent values
expectAssignable<SuperSonicEvent>('setup');
expectAssignable<SuperSonicEvent>('ready');
expectAssignable<SuperSonicEvent>('message');
expectAssignable<SuperSonicEvent>('message:raw');
expectAssignable<SuperSonicEvent>('message:sent');
expectAssignable<SuperSonicEvent>('debug');
expectAssignable<SuperSonicEvent>('error');
expectAssignable<SuperSonicEvent>('shutdown');
expectAssignable<SuperSonicEvent>('destroy');
expectAssignable<SuperSonicEvent>('resumed');
expectAssignable<SuperSonicEvent>('reload:start');
expectAssignable<SuperSonicEvent>('reload:complete');
expectAssignable<SuperSonicEvent>('audiocontext:statechange');
expectAssignable<SuperSonicEvent>('audiocontext:suspended');
expectAssignable<SuperSonicEvent>('audiocontext:resumed');
expectAssignable<SuperSonicEvent>('audiocontext:interrupted');
expectAssignable<SuperSonicEvent>('loading:start');
expectAssignable<SuperSonicEvent>('loading:complete');

// Invalid event names
expectNotAssignable<SuperSonicEvent>('invalid');
expectNotAssignable<SuperSonicEvent>('');

// Event callbacks via sonic.on() — verify typed params
declare const sonic: SuperSonic;

// on() returns unsubscribe function
const unsub = sonic.on('message', (msg) => {
  expectType<OscMessage>(msg);
});
expectType<() => void>(unsub);

// ready event params
sonic.on('ready', (data) => {
  expectType<SuperSonicInfo['capabilities']>(data.capabilities);
  expectType<BootStats>(data.bootStats);
});

// error event
sonic.on('error', (err) => {
  expectType<Error>(err);
});

// debug event
sonic.on('debug', (msg) => {
  expectType<string>(msg.text);
  expectType<number>(msg.timestamp);
  expectType<number>(msg.sequence);
});

// message:raw event
sonic.on('message:raw', (data) => {
  expectType<Uint8Array>(data.oscData);
  expectType<number>(data.sequence);
  expectType<number>(data.timestamp);
  expectType<number | null>(data.scheduledTime);
});

// message:sent event
sonic.on('message:sent', (data) => {
  expectType<Uint8Array>(data.oscData);
  expectType<number>(data.sourceId);
  expectType<number>(data.sequence);
  expectType<number>(data.timestamp);
  expectType<number | null>(data.scheduledTime);
});

// reload:complete event
sonic.on('reload:complete', (data) => {
  expectType<boolean>(data.success);
});

// audiocontext:statechange event
sonic.on('audiocontext:statechange', (data) => {
  expectType<AudioContextState>(data.state);
});

// loading events
sonic.on('loading:start', (data) => {
  expectType<string>(data.type);
  expectType<string>(data.name);
});
sonic.on('loading:complete', (data) => {
  expectType<string>(data.type);
  expectType<string>(data.name);
  expectType<number>(data.size);
});

// off() returns this (SuperSonic)
const offResult = sonic.off('message', (_msg) => {});
expectType<SuperSonic>(offResult);

// once() returns unsubscribe function
const onceUnsub = sonic.once('ready', () => {});
expectType<() => void>(onceUnsub);

// removeAllListeners returns this
expectType<SuperSonic>(sonic.removeAllListeners('message'));
expectType<SuperSonic>(sonic.removeAllListeners());

// ============================================================================
// Section 7: OscChannel
// ============================================================================

// OscCategory
expectAssignable<OscCategory>('nonBundle');
expectAssignable<OscCategory>('immediate');
expectAssignable<OscCategory>('nearFuture');
expectAssignable<OscCategory>('late');
expectAssignable<OscCategory>('farFuture');
expectNotAssignable<OscCategory>('invalid');

// OscChannelMetrics
declare const chMetrics: OscChannelMetrics;
expectType<number>(chMetrics.messagesSent);
expectType<number>(chMetrics.bytesSent);
expectType<number>(chMetrics.nonBundle);
expectType<number>(chMetrics.immediate);
expectType<number>(chMetrics.nearFuture);
expectType<number>(chMetrics.late);
expectType<number>(chMetrics.bypassed);

// SAB transferable — discriminated by mode literal
declare const sabTransfer: OscChannelSABTransferable;
expectType<'sab'>(sabTransfer.mode);
expectType<SharedArrayBuffer>(sabTransfer.sharedBuffer);
expectType<number>(sabTransfer.ringBufferBase);
expectType<Record<string, number>>(sabTransfer.bufferConstants);
expectType<Record<string, number>>(sabTransfer.controlIndices);
expectType<MessagePort | null>(sabTransfer.preschedulerPort);
expectType<number>(sabTransfer.bypassLookaheadS);
expectType<number>(sabTransfer.sourceId);
expectType<boolean>(sabTransfer.blocking);

// PM transferable
declare const pmTransfer: OscChannelPMTransferable;
expectType<'postMessage'>(pmTransfer.mode);
expectType<MessagePort>(pmTransfer.port);
expectType<MessagePort | null>(pmTransfer.preschedulerPort);
expectType<number>(pmTransfer.bypassLookaheadS);
expectType<number>(pmTransfer.sourceId);
expectType<boolean>(pmTransfer.blocking);

// OscChannel instance methods
declare const channel: OscChannel;
expectType<OscCategory>(channel.classify(new Uint8Array()));
expectType<boolean>(channel.send(new Uint8Array()));
expectType<boolean>(channel.sendDirect(new Uint8Array()));
expectType<boolean>(channel.sendToPrescheduler(new Uint8Array()));
expectType<number>(channel.nextNodeId());
expectType<OscChannelMetrics>(channel.getMetrics());
expectType<OscChannelMetrics>(channel.getAndResetMetrics());
expectType<void>(channel.close());
expectType<TransportMode>(channel.mode);
expectType<OscChannelTransferable>(channel.transferable);
expectType<Transferable[]>(channel.transferList);

// Static fromTransferable
expectType<OscChannel>(OscChannel.fromTransferable(sabTransfer));
expectType<OscChannel>(OscChannel.fromTransferable(pmTransfer));

// ============================================================================
// Section 8: SuperSonic Class
// ============================================================================

// Constructor
const sonic1 = new SuperSonic();
const sonic2 = new SuperSonic({ baseURL: '/dist/' });
const sonic3 = new SuperSonic({ mode: 'sab', scsynthOptions: { numBuffers: 2048 } });

// Static
expectType<typeof osc>(SuperSonic.osc);
expectType<MetricsSchema>(SuperSonic.getMetricsSchema());
expectType<Record<string, unknown>>(SuperSonic.getTreeSchema());
expectType<Record<string, unknown>>(SuperSonic.getRawTreeSchema());

// State getters
expectType<boolean>(sonic.initialized);
expectType<boolean>(sonic.initializing);
expectType<AudioContext | null>(sonic.audioContext);
expectType<TransportMode>(sonic.mode);
expectType<Record<string, number> | null>(sonic.bufferConstants);
expectType<number>(sonic.ringBufferBase);
expectType<SharedArrayBuffer | null>(sonic.sharedBuffer);
expectType<number>(sonic.initTime);

// node getter
const node = sonic.node;
if (node) {
  expectType<BaseAudioContext>(node.context);
  expectType<number>(node.numberOfOutputs);
  expectType<number>(node.numberOfInputs);
  expectType<number>(node.channelCount);
  expectType<AudioWorkletNode>(node.input);
}

// Instance properties
expectType<Map<string, Uint8Array>>(sonic.loadedSynthDefs);
expectType<BootStats>(sonic.bootStats);

// Lifecycle — all return Promise<void>
expectType<Promise<void>>(sonic.init());
expectType<Promise<void>>(sonic.shutdown());
expectType<Promise<void>>(sonic.destroy());
expectType<Promise<void>>(sonic.reset());

// Recovery
expectType<Promise<boolean>>(sonic.recover());
expectType<Promise<boolean>>(sonic.resume());
expectType<Promise<void>>(sonic.suspend());
expectType<Promise<boolean>>(sonic.reload());

// send() returns void, address is string, rest are OscArg[]
expectType<void>(sonic.send('/s_new', 'beep', 1001, 0, 1, 'freq', 440));
expectType<void>(sonic.send('/n_free', 1001));
expectType<void>(sonic.send('/g_new', 1, 0, 0));

// sendOSC() accepts Uint8Array/ArrayBuffer with optional SendOSCOptions
expectType<void>(sonic.sendOSC(new Uint8Array()));
expectType<void>(sonic.sendOSC(new ArrayBuffer(8)));
expectType<void>(sonic.sendOSC(new Uint8Array(), { sessionId: 'song1', runTag: 'verse' }));

// Cancellation
expectType<void>(sonic.cancelTag('tag1'));
expectType<void>(sonic.cancelSession('session1'));
expectType<void>(sonic.cancelSessionTag('session1', 'tag1'));
expectType<void>(sonic.cancelAll());
expectType<Promise<void>>(sonic.purge());

// createOscChannel
expectType<OscChannel>(sonic.createOscChannel());
expectType<OscChannel>(sonic.createOscChannel({ sourceId: 1 }));
expectType<OscChannel>(sonic.createOscChannel({ sourceId: 1, blocking: true }));

// nextNodeId
expectType<number>(sonic.nextNodeId());

// Asset loading
expectType<Promise<LoadSynthDefResult>>(sonic.loadSynthDef('beep'));
expectType<Promise<LoadSynthDefResult>>(sonic.loadSynthDef('/path/to/beep.scsyndef'));
expectType<Promise<LoadSynthDefResult>>(sonic.loadSynthDef(new ArrayBuffer(100)));
expectType<Promise<LoadSynthDefResult>>(sonic.loadSynthDef(new Uint8Array()));
expectType<Promise<LoadSynthDefResult>>(sonic.loadSynthDef(new Blob()));

expectType<Promise<Record<string, { success: boolean; error?: string }>>>(
  sonic.loadSynthDefs(['beep', 'pad'])
);

expectType<Promise<LoadSampleResult>>(sonic.loadSample(0, '/samples/kick.wav'));
expectType<Promise<LoadSampleResult>>(sonic.loadSample(0, new ArrayBuffer(100)));
expectType<Promise<LoadSampleResult>>(sonic.loadSample(0, '/kick.wav', 0, 44100));

expectType<LoadedBufferInfo[]>(sonic.getLoadedBuffers());

expectType<Promise<SampleInfo>>(sonic.sampleInfo('/samples/kick.wav'));
expectType<Promise<SampleInfo>>(sonic.sampleInfo(new ArrayBuffer(100)));
expectType<Promise<SampleInfo>>(sonic.sampleInfo('/kick.wav', 0, 44100));

expectType<Promise<void>>(sonic.sync());
expectType<Promise<void>>(sonic.sync(42));

// Metrics
expectType<SuperSonicMetrics>(sonic.getMetrics());
expectType<Uint32Array>(sonic.getMetricsArray());
expectType<Snapshot>(sonic.getSnapshot());

// Tree
expectType<RawTree>(sonic.getRawTree());
expectType<Tree>(sonic.getTree());

// Timing
expectType<void>(sonic.setClockOffset(0.001));

// Capture
expectType<void>(sonic.startCapture());
expectType<Float32Array>(sonic.stopCapture());
expectType<boolean>(sonic.isCaptureEnabled());
expectType<number>(sonic.getCaptureFrames());
expectType<number>(sonic.getMaxCaptureDuration());

// Info
expectType<SuperSonicInfo>(sonic.getInfo());

// ============================================================================
// Section 9: osc Utilities
// ============================================================================

// encodeMessage
expectType<Uint8Array>(osc.encodeMessage('/s_new', ['beep', 1001, 0, 1]));
expectType<Uint8Array>(osc.encodeMessage('/n_free'));

// encodeBundle
expectType<Uint8Array>(osc.encodeBundle(1, [['/s_new', 'beep', 1001] as OscMessage]));
expectType<Uint8Array>(osc.encodeBundle(null, []));
expectType<Uint8Array>(osc.encodeBundle([100, 0], [['/n_set', 1001, 'freq', 880] as OscMessage]));

// decode
const decoded = osc.decode(new Uint8Array());
expectAssignable<OscMessage | OscBundle>(decoded);
expectAssignable<OscMessage | OscBundle>(osc.decode(new ArrayBuffer(8)));

// encodeSingleBundle
expectType<Uint8Array>(osc.encodeSingleBundle(1, '/s_new', ['beep', 1001]));
expectType<Uint8Array>(osc.encodeSingleBundle(1, '/n_free'));

// readTimetag
const tt = osc.readTimetag(new Uint8Array(16));
expectType<{ ntpSeconds: number; ntpFraction: number } | null>(tt);

// ntpNow
expectType<number>(osc.ntpNow());

// NTP_EPOCH_OFFSET
expectType<number>(osc.NTP_EPOCH_OFFSET);

// ============================================================================
// Section 10: Cross-cutting Relationships
// ============================================================================

// OscMessage is assignable to OscBundlePacket
declare const msg: OscMessage;
expectAssignable<OscBundlePacket>(msg);

// SuperSonic.osc is same type as standalone osc export
expectType<typeof osc>(SuperSonic.osc);

// MetricsSchema.metrics keys match SuperSonicMetrics
declare const msKey: keyof typeof schema.metrics;
expectAssignable<keyof SuperSonicMetrics>(msKey);

// ============================================================================
// Section 11: AddAction & BlockedCommand Types
// ============================================================================

// AddAction — 0-4 valid
expectAssignable<AddAction>(0);
expectAssignable<AddAction>(1);
expectAssignable<AddAction>(2);
expectAssignable<AddAction>(3);
expectAssignable<AddAction>(4);
expectNotAssignable<AddAction>(5);
expectNotAssignable<AddAction>(-1);

// BlockedCommand — all 9
expectAssignable<BlockedCommand>('/d_load');
expectAssignable<BlockedCommand>('/d_loadDir');
expectAssignable<BlockedCommand>('/b_read');
expectAssignable<BlockedCommand>('/b_readChannel');
expectAssignable<BlockedCommand>('/b_write');
expectAssignable<BlockedCommand>('/b_close');
expectAssignable<BlockedCommand>('/clearSched');
expectAssignable<BlockedCommand>('/error');
expectAssignable<BlockedCommand>('/quit');
expectNotAssignable<BlockedCommand>('/s_new');

// ============================================================================
// Section 12: Typed send() Overloads
// ============================================================================

// --- Blocked commands produce `never` ---
expectType<never>(sonic.send('/d_load', 'beep'));
expectType<never>(sonic.send('/d_loadDir', '/path'));
expectType<never>(sonic.send('/b_read', 0, '/file'));
expectType<never>(sonic.send('/b_readChannel', 0, '/file'));
expectType<never>(sonic.send('/b_write', 0, '/file'));
expectType<never>(sonic.send('/b_close', 0));
expectType<never>(sonic.send('/clearSched'));
expectType<never>(sonic.send('/error', 1));
expectType<never>(sonic.send('/quit'));

// --- Top-level commands ---
expectType<void>(sonic.send('/status'));
expectType<void>(sonic.send('/version'));
expectType<void>(sonic.send('/notify', 1));
expectType<void>(sonic.send('/notify', 0));
expectType<void>(sonic.send('/notify', 1, 5)); // with optional clientID
expectType<void>(sonic.send('/dumpOSC', 0));
expectType<void>(sonic.send('/dumpOSC', 3));
expectType<void>(sonic.send('/sync', 42));
expectType<void>(sonic.send('/rtMemoryStatus'));

// --- SynthDef commands ---
expectType<void>(sonic.send('/d_recv', new Uint8Array([1, 2, 3])));
expectType<void>(sonic.send('/d_recv', new ArrayBuffer(8)));
expectType<void>(sonic.send('/d_recv', new Uint8Array([1, 2, 3]), new Uint8Array([4, 5]))); // with completion message
expectType<void>(sonic.send('/d_recv', new ArrayBuffer(8), new ArrayBuffer(4))); // completion as ArrayBuffer
expectType<void>(sonic.send('/d_free', 'beep'));
expectType<void>(sonic.send('/d_free', 'beep', 'pad', 'kick'));
expectType<void>(sonic.send('/d_freeAll'));

// --- Synth commands ---
expectType<void>(sonic.send('/s_new', 'beep', 1001, 0, 1));
expectType<void>(sonic.send('/s_new', 'beep', 1001, 0, 1, 'freq', 440));
expectType<void>(sonic.send('/s_new', 'beep', 1001, 0, 1, 'freq', 440, 'amp', 0.5));
expectType<void>(sonic.send('/s_get', 1001, 'freq'));
expectType<void>(sonic.send('/s_get', 1001, 0));
expectType<void>(sonic.send('/s_getn', 1001, 0, 5));
expectType<void>(sonic.send('/s_getn', 1001, 'freq', 3)); // control by name
expectType<void>(sonic.send('/s_noid', 1001));
expectType<void>(sonic.send('/s_noid', 1001, 1002, 1003));

// --- Node commands ---
expectType<void>(sonic.send('/n_free', 1001));
expectType<void>(sonic.send('/n_free', 1001, 1002, 1003));
expectType<void>(sonic.send('/n_set', 1001, 'freq', 440));
expectType<void>(sonic.send('/n_set', 1001, 0, 440));
expectType<void>(sonic.send('/n_setn', 1001, 0, 3, 440, 550, 660));
expectType<void>(sonic.send('/n_setn', 1001, 'freq', 3, 440, 550, 660)); // control by name
expectType<void>(sonic.send('/n_fill', 1001, 0, 5, 0.0));
expectType<void>(sonic.send('/n_fill', 1001, 'freq', 1, 440));
expectType<void>(sonic.send('/n_run', 1001, 1));
expectType<void>(sonic.send('/n_run', 1001, 0, 1002, 1));
expectType<void>(sonic.send('/n_before', 1001, 1002));
expectType<void>(sonic.send('/n_after', 1001, 1002));
expectType<void>(sonic.send('/n_order', 0, 0, 1001));
expectType<void>(sonic.send('/n_order', 0, 0, 1001, 1002));
expectType<void>(sonic.send('/n_query', 1001));
expectType<void>(sonic.send('/n_query', 1001, 1002));
expectType<void>(sonic.send('/n_trace', 1001));
expectType<void>(sonic.send('/n_trace', 1001, 1002));
expectType<void>(sonic.send('/n_map', 1001, 'freq', 0));
expectType<void>(sonic.send('/n_mapn', 1001, 'freq', 0, 3));
expectType<void>(sonic.send('/n_mapa', 1001, 'freq', 0));
expectType<void>(sonic.send('/n_mapan', 1001, 'freq', 0, 3));

// --- Group commands ---
expectType<void>(sonic.send('/g_new', 1, 0, 0));
expectType<void>(sonic.send('/g_new', 1, 0, 0, 2, 1, 0));
expectType<void>(sonic.send('/p_new', 1, 0, 0));
expectType<void>(sonic.send('/p_new', 1, 0, 0, 2, 1, 0)); // repeatable triplets
expectType<void>(sonic.send('/g_freeAll', 1));
expectType<void>(sonic.send('/g_freeAll', 1, 2, 3));
expectType<void>(sonic.send('/g_deepFree', 1));
expectType<void>(sonic.send('/g_head', 0, 1001));
expectType<void>(sonic.send('/g_tail', 0, 1001));
expectType<void>(sonic.send('/g_dumpTree', 0, 0));
expectType<void>(sonic.send('/g_dumpTree', 0, 1, 1, 0)); // multiple groups
expectType<void>(sonic.send('/g_queryTree', 0, 0));
expectType<void>(sonic.send('/g_queryTree', 0, 1));

// --- UGen commands ---
expectType<void>(sonic.send('/u_cmd', 1001, 0, 'myCommand'));
expectType<void>(sonic.send('/u_cmd', 1001, 0, 'myCommand', 42, 'hello'));

// --- Buffer commands ---
expectType<void>(sonic.send('/b_alloc', 0, 44100));
expectType<void>(sonic.send('/b_alloc', 0, 44100, 2));
expectType<void>(sonic.send('/b_alloc', 0, 44100, 2, 48000));
expectType<void>(sonic.send('/b_allocRead', 0, '/samples/kick.wav'));
expectType<void>(sonic.send('/b_allocRead', 0, '/samples/kick.wav', 0, 44100));
expectType<void>(sonic.send('/b_allocReadChannel', 0, '/samples/stereo.wav', 0, 0, 0)); // left channel only
expectType<void>(sonic.send('/b_allocReadChannel', 0, '/samples/stereo.wav', 0, 0, 0, 1));
expectType<void>(sonic.send('/b_allocFile', 0, new Uint8Array([1, 2, 3])));
expectType<void>(sonic.send('/b_allocFile', 0, new ArrayBuffer(8)));
expectType<void>(sonic.send('/b_free', 0));
expectType<void>(sonic.send('/b_free', 0, new Uint8Array([1, 2]))); // with completion message
expectType<void>(sonic.send('/b_zero', 0));
expectType<void>(sonic.send('/b_zero', 0, new Uint8Array([1, 2]))); // with completion message
expectType<void>(sonic.send('/b_query', 0));
expectType<void>(sonic.send('/b_query', 0, 1, 2));
expectType<void>(sonic.send('/b_get', 0, 100));
expectType<void>(sonic.send('/b_get', 0, 100, 200, 300));
expectType<void>(sonic.send('/b_set', 0, 100, 0.5, 200, 0.7));
expectType<void>(sonic.send('/b_setn', 0, 0, 3, 0.1, 0.2, 0.3));
expectType<void>(sonic.send('/b_getn', 0, 0, 10));
expectType<void>(sonic.send('/b_fill', 0, 0, 1024, 0.0));
expectType<void>(sonic.send('/b_gen', 0, 'sine1', 7, 1, 0.5, 0.25));

// --- Control bus commands ---
expectType<void>(sonic.send('/c_set', 0, 440));
expectType<void>(sonic.send('/c_get', 0));
expectType<void>(sonic.send('/c_get', 0, 1, 2));
expectType<void>(sonic.send('/c_setn', 0, 3, 440, 550, 660));
expectType<void>(sonic.send('/c_getn', 0, 10));
expectType<void>(sonic.send('/c_fill', 0, 16, 0.0));

// --- Catch-all: unknown addresses still work ---
expectType<void>(sonic.send('/custom_command', 1, 'foo', true));
expectType<void>(sonic.send('/my_plugin'));

// --- Negative: wrong arg types fall through to catch-all (no error) ---
// NOTE: With the catch-all overload, mistyped args for known commands
// silently match `send(address: string, ...args: OscArg[]): void`.
// The typed overloads provide IDE autocomplete and parameter hints,
// not hard compile errors for wrong arg shapes.

# API Reference

> Auto-generated from [`supersonic.d.ts`](../supersonic.d.ts). For worked examples and patterns, see the [Guide](GUIDE.md).

* [SuperSonic](#supersonic) — [Constructor Options](#constructor-options) · [Server Options](#server-options) · [Properties](#properties) · [Accessors](#accessors) · [Methods](#methods) · [Event Types](#event-types) · [OSC Argument Types](#osc-argument-types)

* [OscChannel](#oscchannel) — [Accessors](#accessors-1) · [Methods](#methods-1)

* [osc](#osc)

* **Interfaces** — [ActivityLineConfig](#activitylineconfig) · [BootStats](#bootstats) · [ClockworkClock](#clockworkclock) · [LoadedBufferInfo](#loadedbufferinfo) · [LoadSampleResult](#loadsampleresult) · [LoadSynthDefResult](#loadsynthdefresult) · [MetricDefinition](#metricdefinition) · [MetricsSchema](#metricsschema) · [NativeStatDefinition](#nativestatdefinition) · [OscBundle](#oscbundle) · [OscChannelMetrics](#oscchannelmetrics) · [OscChannelPMTransferable](#oscchannelpmtransferable) · [OscChannelSABTransferable](#oscchannelsabtransferable) · [RawTree](#rawtree) · [RawTreeNode](#rawtreenode) · [RingBufferUsage](#ringbufferusage) · [SampleInfo](#sampleinfo-1) · [Snapshot](#snapshot) · [SuperSonicInfo](#supersonicinfo) · [SuperSonicMetrics](#supersonicmetrics) · [SystemReport](#systemreport) · [Tree](#tree) · [TreeNode](#treenode)

* **Type Aliases** — [AddAction](#addaction) · [EngineState](#enginestate) · [NodeID](#nodeid) · [NTPTimeTag](#ntptimetag) · [OscBundlePacket](#oscbundlepacket) · [OscChannelTransferable](#oscchanneltransferable) · [OscMessage](#oscmessage) · [SuperSonicEvent](#supersonicevent) · [TransportMode](#transportmode) · [UUID](#uuid)

## Classes

### SuperSonic

SuperSonic — WebAssembly SuperCollider synthesis engine for the browser.

Coordinates WASM, AudioWorklet, SharedArrayBuffer, and IO Workers to run
scsynth with low latency inside a web page.

**Core**

| Member                    | Description                                                                                                                 |
| ------------------------- | --------------------------------------------------------------------------------------------------------------------------- |
| [`init()`](#init)         | Initialise the engine.                                                                                                      |
| [`shutdown()`](#shutdown) | Shut down the engine.                                                                                                       |
| [`destroy()`](#destroy)   | Destroy the engine completely.                                                                                              |
| [`recover()`](#recover)   | Smart recovery — tries a quick resume first, falls back to full reload.                                                     |
| [`suspend()`](#suspend)   | Suspend the AudioContext and stop the drift timer.                                                                          |
| [`resume()`](#resume)     | Quick resume.                                                                                                               |
| [`reload()`](#reload)     | Full reload — destroys and recreates the worklet and WASM, then restores all previously loaded synthdefs and audio buffers. |
| [`reset()`](#reset)       | Shutdown and immediately re-initialise.                                                                                     |
| [`send()`](#send)         | Send an OSC message to the engine.                                                                                          |
| [`sendOSC()`](#sendosc)   | Send pre-encoded OSC bytes to the engine.                                                                                   |
| [`sync()`](#sync)         | A barrier: resolves once everything sent before it has reached the engine, in order.                                        |
| [`purge()`](#purge)       | Flush all pending scheduled OSC: clears the engine's scheduler and the IN ring so nothing already in flight will fire.      |

**Asset Loading**

| Member                              | Description                                                               |
| ----------------------------------- | ------------------------------------------------------------------------- |
| [`loadSynthDef()`](#loadsynthdef)   | Load a SynthDef into the engine.                                          |
| [`loadSynthDefs()`](#loadsynthdefs) | Load several SynthDefs in parallel, each as loadSynthDef would.           |
| [`loadSample()`](#loadsample)       | Load an audio sample into a buffer slot.                                  |
| [`sampleInfo()`](#sampleinfo)       | Get sample metadata (including content hash) without allocating a buffer. |

**Events**

| Member                                        | Description                                                   |
| --------------------------------------------- | ------------------------------------------------------------- |
| [`on()`](#on)                                 | Subscribe to an event.                                        |
| [`off()`](#off)                               | Unsubscribe from an event.                                    |
| [`once()`](#once)                             | Subscribe to an event once.                                   |
| [`removeAllListeners()`](#removealllisteners) | Remove all listeners for an event, or all listeners entirely. |

**Node Tree**

| Member                          | Description                                                                              |
| ------------------------------- | ---------------------------------------------------------------------------------------- |
| [`getTree()`](#gettree)         | Get the node tree in hierarchical format.                                                |
| [`getRawTree()`](#getrawtree)   | Get the node tree in flat format with linkage pointers.                                  |
| [`getSnapshot()`](#getsnapshot) | Get a diagnostic snapshot with metrics (and their descriptions) and JS heap memory info. |

**Metrics**

| Member                                    | Description                                                    |
| ----------------------------------------- | -------------------------------------------------------------- |
| [`getMetrics()`](#getmetrics)             | Get current metrics as a named object.                         |
| [`getMetricsArray()`](#getmetricsarray)   | Get metrics as a flat Uint32Array for zero-allocation reading. |
| [`getMetricsSchema()`](#getmetricsschema) | Get the metrics schema describing all available metrics.       |

**Properties**

| Member                                | Description                                                           |
| ------------------------------------- | --------------------------------------------------------------------- |
| [`initialized`](#initialized)         | Whether the engine has completed initialisation.                      |
| [`initializing`](#initializing)       | Whether init is currently in progress.                                |
| [`audioContext`](#audiocontext)       | The underlying AudioContext.                                          |
| [`node`](#node)                       | AudioWorkletNode wrapper for custom audio routing.                    |
| [`loadedSynthDefs`](#loadedsynthdefs) | Map of loaded SynthDef names to their binary data — live, not a copy. |

**Advanced**

| Member                                              | Description                                                                               |
| --------------------------------------------------- | ----------------------------------------------------------------------------------------- |
| [`getInfo()`](#getinfo)                             | Get engine info: sample rate, memory layout, capabilities, and version.                   |
| [`createOscChannel()`](#createoscchannel)           | Create an OscChannel for direct worker-to-worklet communication.                          |
| [`startCapture()`](#startcapture)                   | Start capturing what the engine sends to the audio device.                                |
| [`stopCapture()`](#stopcapture)                     | Stop capturing and return what was captured.                                              |
| [`getCaptureFrames()`](#getcaptureframes)           | Get number of audio frames captured so far — or, with no capture running, written in all. |
| [`isCaptureEnabled()`](#iscaptureenabled)           | Check if audio capture is currently enabled.                                              |
| [`getMaxCaptureDuration()`](#getmaxcaptureduration) | Get maximum capture duration in seconds: the length of the capture ring.                  |
| [`setClockOffset()`](#setclockoffset)               | Set clock offset for multi-system sync (e.g. against an NTP server).                      |

**Advanced**

| Member                                    | Description                                                                                                      |
| ----------------------------------------- | ---------------------------------------------------------------------------------------------------------------- |
| [`bufferConstants`](#bufferconstants)     | Buffer layout constants from the WASM build.                                                                     |
| [`clock`](#clock)                         | Session-timeline service: tempo, beat origin, transport, NTP "now." See ClockworkClock for the full API surface. |
| [`gamepad`](#gamepad)                     | The gamepad manager, when the gamepad option is on and it came up; otherwise null (gamepadError says why).       |
| [`gamepadError`](#gamepaderror)           | Why gamepad is null although it was asked for: the error its start-up threw.                                     |
| [`midi`](#midi)                           | The Web MIDI manager, when MIDI is enabled (the midi option, or enableMidi) and came up.                         |
| [`midiError`](#midierror)                 | Why midi is null although MIDI was asked for: the error its start-up threw.                                      |
| [`mode`](#mode)                           | Active transport mode ('sab' or 'postMessage').                                                                  |
| [`ringBufferBase`](#ringbufferbase)       | Ring buffer base offset in SharedArrayBuffer.                                                                    |
| [`sharedBuffer`](#sharedbuffer)           | The SharedArrayBuffer (SAB mode) or null (postMessage mode).                                                     |
| [`allocSample()`](#allocsample)           | Allocate an empty buffer, as loadSample does for a file: resolves once the engine has it.                        |
| [`enableMidi()`](#enablemidi)             | Bring Web MIDI up after init.                                                                                    |
| [`getEngineState()`](#getenginestate)     | Returns the current engine lifecycle state.                                                                      |
| [`getLoadedBuffers()`](#getloadedbuffers) | Get info about all loaded audio buffers.                                                                         |
| [`getScope()`](#getscope)                 | Copy the newest frames frames of a ScopeOut2 scope stream.                                                       |
| [`getScopes()`](#getscopes)               | List the scope slots in use.                                                                                     |
| [`getSystemReport()`](#getsystemreport)   | Get a comprehensive system performance report.                                                                   |
| [`isRunning()`](#isrunning)               | Returns true if the engine has finished booting and is ready to send and receive messages.                       |
| [`nextNodeId()`](#nextnodeid)             | Get the next unique node ID.                                                                                     |
| [`request()`](#request)                   | Send a message and wait for its reply.                                                                           |
| [`getRawTreeSchema()`](#getrawtreeschema) | Get schema describing the raw flat node tree structure.                                                          |
| [`getScopeSchema()`](#getscopeschema)     | Scope geometry: slot count, per-slot ring frames, channels.                                                      |
| [`getTreeSchema()`](#gettreeschema)       | Get schema describing the hierarchical node tree structure.                                                      |

#### Examples

```ts
// CDN Quick Start
import { SuperSonic } from 'https://unpkg.com/supersonic-scsynth@latest/dist/supersonic.js';

const CDN = 'https://unpkg.com/';
const sonic = new SuperSonic({
  baseURL: CDN + 'supersonic-scsynth@latest/dist/',
  coreBaseURL: CDN + 'supersonic-scsynth-core@latest/',
  synthdefBaseURL: CDN + 'supersonic-scsynth-synthdefs@latest/synthdefs/',
});

// Call init after a user gesture (click/tap) due to browser autoplay policies
myButton.onclick = async () => {
  await sonic.init();
  await sonic.loadSynthDef('sonic-pi-beep');
  sonic.send('/s_new', 'sonic-pi-beep', -1, 0, 0, 'note', 60);
};
```

```ts
// Setup + message listeners
import { SuperSonic } from 'supersonic-scsynth';

const sonic = new SuperSonic({ baseURL: '/dist/' });

sonic.on('setup', async () => {
  await sonic.loadSynthDef('beep');
});

sonic.on('in', (msg) => {
  console.log('OSC from the engine:', msg[0], msg.slice(1));
});

await sonic.init();
sonic.send('/s_new', 'beep', 1001, 0, 0, 'freq', 440);
```

***

#### Constructors

##### Constructor

> **new SuperSonic**(`options?`): [`SuperSonic`](#supersonic)

Create a new SuperSonic instance.

Does not start the engine — call [init](#init) to boot.

###### Parameters

| Parameter  | Type                                        | Description                                                                                                   |
| ---------- | ------------------------------------------- | ------------------------------------------------------------------------------------------------------------- |
| `options?` | [`SuperSonicOptions`](#constructor-options) | Configuration options. Needs `baseURL`, or else `workerBaseURL` together with `coreBaseURL` or `wasmBaseURL`. |

###### Returns

[`SuperSonic`](#supersonic)

###### Throws

If URL configuration is missing or scsynthOptions are invalid.

###### Example

```ts
const sonic = new SuperSonic({
  baseURL: '/supersonic/dist/',
  mode: 'postMessage',
  scsynthOptions: { numBuffers: 2048 },
});
```

***

#### Constructor Options

| Property                                                | Type                                        | Description                                                                                                                                                                                                                                                                                                                                                                                                                      | Required |
| ------------------------------------------------------- | ------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | -------- |
| <a id="activityevent"></a> `activityEvent?`             | [`ActivityLineConfig`](#activitylineconfig) | Length limits for the text the activity events carry.                                                                                                                                                                                                                                                                                                                                                                            |          |
| <a id="audiocontext-1"></a> `audioContext?`             | `AudioContext`                              | Provide your own AudioContext instead of letting SuperSonic create one. It stays yours: kept across `reload()`, left open by `shutdown()`.                                                                                                                                                                                                                                                                                       |          |
| <a id="audiocontextoptions"></a> `audioContextOptions?` | `AudioContextOptions`                       | Options passed to `new AudioContext()`, over the defaults `{ latencyHint: 'interactive', sampleRate: 48000 }`. Ignored if `audioContext` is provided.                                                                                                                                                                                                                                                                            |          |
| <a id="autoconnect"></a> `autoConnect?`                 | `boolean`                                   | Auto-connect the AudioWorkletNode to the AudioContext destination. Default: true.                                                                                                                                                                                                                                                                                                                                                |          |
| <a id="baseurl"></a> `baseURL?`                         | `string`                                    | Convenience shorthand when all assets (WASM, worklet, workers, synthdefs, samples) are co-located.                                                                                                                                                                                                                                                                                                                               | Yes\*    |
| <a id="buffergrowincrement"></a> `bufferGrowIncrement?` | `number`                                    | Bytes the sample buffer pool grows by each time it grows. Default: 32 MB.                                                                                                                                                                                                                                                                                                                                                        |          |
| <a id="bufferpoolsize"></a> `bufferPoolSize?`           | `number`                                    | Initial size of the sample buffer pool in bytes. Default: 4 MB.                                                                                                                                                                                                                                                                                                                                                                  |          |
| <a id="corebaseurl"></a> `coreBaseURL?`                 | `string`                                    | Base URL for the engine: the WASM and the AudioWorklet (the supersonic-scsynth-core package). Defaults to `baseURL`.                                                                                                                                                                                                                                                                                                             |          |
| <a id="debug-1"></a> `debug?`                           | `boolean`                                   | Enable all debug console logging. Default: false.                                                                                                                                                                                                                                                                                                                                                                                |          |
| <a id="debugengine"></a> `debugEngine?`                 | `boolean`                                   | Log the engine's debug output to the console. Default: false.                                                                                                                                                                                                                                                                                                                                                                    |          |
| <a id="debugoscin"></a> `debugOscIn?`                   | `boolean`                                   | Log incoming OSC messages to console. Default: false.                                                                                                                                                                                                                                                                                                                                                                            |          |
| <a id="debugoscout"></a> `debugOscOut?`                 | `boolean`                                   | Log outgoing OSC messages to console. Default: false.                                                                                                                                                                                                                                                                                                                                                                            |          |
| <a id="fetchmaxretries"></a> `fetchMaxRetries?`         | `number`                                    | Max fetch retries when loading assets. Default: 3.                                                                                                                                                                                                                                                                                                                                                                               |          |
| <a id="fetchretrydelay"></a> `fetchRetryDelay?`         | `number`                                    | Base delay between retries in ms (exponential backoff). Default: 1000.                                                                                                                                                                                                                                                                                                                                                           |          |
| <a id="gamepad-1"></a> `gamepad?`                       | `boolean` \| `Record`<`string`, `unknown`>  | The Gamepad API: `true`, or an object of options for the gamepad manager. Default: false.                                                                                                                                                                                                                                                                                                                                        |          |
| <a id="maxbuffermemory"></a> `maxBufferMemory?`         | `number`                                    | Most the sample buffer pool may grow to, in bytes. The pool grows on demand up to this limit. Default: 768 MB.                                                                                                                                                                                                                                                                                                                   |          |
| <a id="midi-1"></a> `midi?`                             | `boolean` \| `Record`<`string`, `unknown`>  | Web MIDI. Off by default: some browsers ask the user's permission, and a page that never uses MIDI should not ask. `true`, or an object of options for the MIDI manager. Brought up during `init()`; to ask later, from the gesture that wants it, use [SuperSonic.enableMidi](#enablemidi). If it cannot come up the engine boots without it, emits `'error'`, and [SuperSonic.midiError](#midierror) says why. Default: false. |          |
| <a id="mode-5"></a> `mode?`                             | [`TransportMode`](#transportmode)           | Transport mode. - `'postMessage'` — works everywhere, no special headers needed (the default off an isolated page) - `'sab'` — lowest latency, requires the Cross-Origin-Opener-Policy and Cross-Origin-Embedder-Policy headers (the default on a cross-origin isolated page) See docs/MODES.md for a full comparison of communication modes.                                                                                    |          |
| <a id="pagelifecycle"></a> `pageLifecycle?`             | `object`                                    | What the page going away, or out of sight, does to the engine. `pagehide`: `'shutdown'` (default — an engine left running after its page is audio with no page to stop it) or `'none'`. `hidden`: `'keep'` (default — a tab behind another plays on) or `'suspend'` (and resume when shown). After a `'shutdown'`, a page restored from the back/forward cache calls `init()` again.                                             |          |
| `pageLifecycle.hidden?`                                 | `"keep"` \| `"suspend"`                     | -                                                                                                                                                                                                                                                                                                                                                                                                                                |          |
| `pageLifecycle.pagehide?`                               | `"shutdown"` \| `"none"`                    | -                                                                                                                                                                                                                                                                                                                                                                                                                                |          |
| <a id="samplebaseurl"></a> `sampleBaseURL?`             | `string`                                    | Base URL for audio sample files (used by [SuperSonic.loadSample](#loadsample)). Defaults to `baseURL + 'samples/'`.                                                                                                                                                                                                                                                                                                              |          |
| <a id="scsynthoptions-1"></a> `scsynthOptions?`         | [`ScsynthOptions`](#server-options)         | The engine's options (see [ScsynthOptions](#server-options)). Validated by the constructor, which throws on a bad one.                                                                                                                                                                                                                                                                                                           |          |
| <a id="snapshotintervalms"></a> `snapshotIntervalMs?`   | `number`                                    | How often to snapshot metrics and the node tree in postMessage mode (ms). Default: 150.                                                                                                                                                                                                                                                                                                                                          |          |
| <a id="synthdefbaseurl"></a> `synthdefBaseURL?`         | `string`                                    | Base URL for synthdef files (used by [SuperSonic.loadSynthDef](#loadsynthdef)). Defaults to `baseURL + 'synthdefs/'`.                                                                                                                                                                                                                                                                                                            |          |
| <a id="wasmbaseurl"></a> `wasmBaseURL?`                 | `string`                                    | Base URL for WASM files. Defaults to `coreBaseURL + 'wasm/'`.                                                                                                                                                                                                                                                                                                                                                                    |          |
| <a id="wasmurl"></a> `wasmUrl?`                         | `string`                                    | Full URL to the WASM binary. Overrides `wasmBaseURL`.                                                                                                                                                                                                                                                                                                                                                                            |          |
| <a id="workerbaseurl"></a> `workerBaseURL?`             | `string`                                    | Base URL for the worker scripts. Defaults to `baseURL + 'workers/'`.                                                                                                                                                                                                                                                                                                                                                             |          |
| <a id="workleturl"></a> `workletUrl?`                   | `string`                                    | Full URL to the AudioWorklet script. Overrides `coreBaseURL`.                                                                                                                                                                                                                                                                                                                                                                    |          |

*Required unless `workerBaseURL` and one of `coreBaseURL` / `wasmBaseURL` are provided.*

***

#### Server Options

| Property                                                    | Type       | Description                                                                                                                                                                                       | Default | Range          |
| ----------------------------------------------------------- | ---------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------- | -------------- |
| <a id="buflength"></a> `bufLength?`                         | `128`      | Audio buffer length — must be 128 (WebAudio API constraint).                                                                                                                                      | 128     | 128 (fixed)    |
| <a id="loadgraphdefs"></a> `loadGraphDefs?`                 | `0` \| `1` | Load synth definitions from the synthdef directory at boot: 0 or 1. Default: 0.                                                                                                                   | 0       | 0–1            |
| <a id="maxgraphdefs"></a> `maxGraphDefs?`                   | `number`   | Max synth definitions. Default: 1024.                                                                                                                                                             | 1024    | 1–4194304      |
| <a id="maxnodes"></a> `maxNodes?`                           | `number`   | Max synthesis nodes — synths + groups. Default: 1024.                                                                                                                                             | 1024    | 1–4194304      |
| <a id="maxwirebufs"></a> `maxWireBufs?`                     | `number`   | Max wire buffers for internal UGen routing. Default: 64.                                                                                                                                          | 64      | 1–4194304      |
| <a id="memorylocking"></a> `memoryLocking?`                 | `boolean`  | Memory locking — not applicable in browser. Default: false.                                                                                                                                       | false   | —              |
| <a id="numaudiobuschannels"></a> `numAudioBusChannels?`     | `number`   | Audio bus channels for routing between synths. Default: 1024.                                                                                                                                     | 1024    | 1–4194304      |
| <a id="numbuffers"></a> `numBuffers?`                       | `number`   | Max audio buffers (1–65535). Default: 1024.                                                                                                                                                       | 1024    | 1–65535        |
| <a id="numcontrolbuschannels"></a> `numControlBusChannels?` | `number`   | Control bus channels for control-rate data. Default: 16384.                                                                                                                                       | 16384   | 1–16777216     |
| <a id="numinputbuschannels"></a> `numInputBusChannels?`     | `number`   | Input channels read from the device (0 disables input). Default: 2.                                                                                                                               | 2       | 0+             |
| <a id="numoutputbuschannels"></a> `numOutputBusChannels?`   | `number`   | Output channels opened on the audio graph (1–128). Default: 2.                                                                                                                                    | 2       | 1–128          |
| <a id="numrgens"></a> `numRGens?`                           | `number`   | Random number generators. Default: 64.                                                                                                                                                            | 64      | 1–65536        |
| <a id="preferredsamplerate"></a> `preferredSampleRate?`     | `number`   | Preferred sample rate: 0, or 8000–384000. The rate the engine runs at is the AudioContext's: set it with `audioContextOptions.sampleRate` (default 48000), or by passing your own `audioContext`. | 0       | 0, 8000–384000 |
| <a id="realtime"></a> `realTime?`                           | `boolean`  | scsynth's real-time flag. Always false here: the AudioWorklet drives the engine.                                                                                                                  | false   | —              |
| <a id="realtimememorysize"></a> `realTimeMemorySize?`       | `number`   | Real-time memory pool in KB for synthesis allocations. Default: 8192 (8MB).                                                                                                                       | 8192    | 1–4194304      |
| <a id="verbosity"></a> `verbosity?`                         | `number`   | How much the engine prints, 0–4; 0 is quiet. Default: 0.                                                                                                                                          | 0       | 0–4            |

***

#### Properties

| Property                           | Modifier | Type                                                     | Description                                                                                                                                                          |
| ---------------------------------- | -------- | -------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| <a id="bootstats"></a> `bootStats` | `public` | [`BootStats`](#bootstats-1)                              | Boot timing statistics.                                                                                                                                              |
| <a id="osc"></a> `osc`             | `static` | `object`                                                 | OSC encoding/decoding utilities. **Example** `const msg = SuperSonic.osc.encodeMessage('/s_new', ['beep', 1001, 0, 0]); const decoded = SuperSonic.osc.decode(msg);` |
| `osc.NTP_EPOCH_OFFSET`             | `public` | `number`                                                 | Seconds between NTP epoch (1900) and Unix epoch (1970): `2208988800`.                                                                                                |
| `osc.decode`                       | `public` | [`OscBundle`](#oscbundle) \| [`OscMessage`](#oscmessage) | -                                                                                                                                                                    |
| `osc.encodeBundle`                 | `public` | `Uint8Array`                                             | -                                                                                                                                                                    |
| `osc.encodeMessage`                | `public` | `Uint8Array`                                             | -                                                                                                                                                                    |
| `osc.encodeSingleBundle`           | `public` | `Uint8Array`                                             | -                                                                                                                                                                    |
| `osc.ntpNow`                       | `public` | `number`                                                 | -                                                                                                                                                                    |
| `osc.readTimetag`                  | `public` | `object`                                                 | -                                                                                                                                                                    |

***

#### Accessors

##### audioContext

###### Get Signature

> **get** **audioContext**(): `AudioContext`

The underlying AudioContext.

Available after [init](#init). Use this to read `sampleRate`, `currentTime`,
or to connect additional audio nodes.

###### Returns

`AudioContext`

##### bufferConstants

###### Get Signature

> **get** **bufferConstants**(): `Record`<`string`, `number`>

Buffer layout constants from the WASM build. Mostly internal.

###### Returns

`Record`<`string`, `number`>

##### clock

###### Get Signature

> **get** **clock**(): [`ClockworkClock`](#clockworkclock)

Session-timeline service: tempo, beat origin, transport, NTP "now."
See [ClockworkClock](#clockworkclock) for the full API surface. Undefined until the first [init](#init).

###### Returns

[`ClockworkClock`](#clockworkclock)

##### gamepad

###### Get Signature

> **get** **gamepad**(): `object`

The gamepad manager, when the `gamepad` option is on and it came up; otherwise null ([gamepadError](#gamepaderror) says why).

###### Returns

`object`

##### gamepadError

###### Get Signature

> **get** **gamepadError**(): `unknown`

Why [gamepad](#gamepad) is null although it was asked for: the error its start-up threw. Null otherwise.

###### Returns

`unknown`

##### initialized

###### Get Signature

> **get** **initialized**(): `boolean`

Whether the engine has completed initialisation.

###### Returns

`boolean`

##### initializing

###### Get Signature

> **get** **initializing**(): `boolean`

Whether [init](#init) is currently in progress.

###### Returns

`boolean`

##### loadedSynthDefs

###### Get Signature

> **get** **loadedSynthDefs**(): `Map`<`string`, `Uint8Array`<`ArrayBufferLike`>>

Map of loaded SynthDef names to their binary data — live, not a copy. SynthDefs appear after a `/d_recv`
through `send()` or `loadSynthDef()`, and are removed on `/d_free` or `/d_freeAll`. Kept for restoring after
`reload()`; cleared by `shutdown()`.

###### Returns

`Map`<`string`, `Uint8Array`<`ArrayBufferLike`>>

##### midi

###### Get Signature

> **get** **midi**(): `object`

The Web MIDI manager, when MIDI is enabled (the `midi` option, or [enableMidi](#enablemidi)) and came up.
Null when not enabled, before [init](#init), or when it could not come up ([midiError](#midierror) says why).

###### Returns

`object`

##### midiError

###### Get Signature

> **get** **midiError**(): `unknown`

Why [midi](#midi) is null although MIDI was asked for: the error its start-up threw. Null otherwise.

###### Returns

`unknown`

##### mode

###### Get Signature

> **get** **mode**(): [`TransportMode`](#transportmode)

Active transport mode (`'sab'` or `'postMessage'`).

###### Returns

[`TransportMode`](#transportmode)

##### node

###### Get Signature

> **get** **node**(): `object`

AudioWorkletNode wrapper for custom audio routing.

Use `node.connect()` / `node.disconnect()` to route audio.
Use `node.input` to connect external audio sources into the engine.
Null before [init](#init).

###### Example

```ts
// Route the engine's output through an AnalyserNode:
sonic.node.disconnect();
sonic.node.connect(analyser);
analyser.connect(sonic.audioContext.destination);
```

###### Returns

| Name              | Type                  | Description                                                      |
| ----------------- | --------------------- | ---------------------------------------------------------------- |
| `channelCount`    | `number`              | -                                                                |
| `context`         | `BaseAudioContext`    | -                                                                |
| `input`           | `AudioWorkletNode`    | The underlying AudioWorkletNode — connect external sources here. |
| `numberOfInputs`  | `number`              | -                                                                |
| `numberOfOutputs` | `number`              | -                                                                |
| `connect()`       | (...`args`) => `void` | -                                                                |
| `disconnect()`    | (...`args`) => `void` | -                                                                |

##### ringBufferBase

###### Get Signature

> **get** **ringBufferBase**(): `number`

Ring buffer base offset in SharedArrayBuffer. Internal.

###### Returns

`number`

##### sharedBuffer

###### Get Signature

> **get** **sharedBuffer**(): `SharedArrayBuffer`

The SharedArrayBuffer (SAB mode) or null (postMessage mode). Internal.

###### Returns

`SharedArrayBuffer`

***

#### Methods

##### allocSample()

> **allocSample**(`bufnum`, `numFrames`, `numChannels?`, `sampleRate?`): `Promise`<{ `bufnum`: `number`; `numChannels`: `number`; `numFrames`: `number`; `sampleRate`: `number`; }>

Allocate an empty buffer, as [loadSample](#loadsample) does for a file: resolves
once the engine has it.

###### Parameters

| Parameter      | Type     | Description                                     |
| -------------- | -------- | ----------------------------------------------- |
| `bufnum`       | `number` | Buffer slot number (0 to numBuffers-1)          |
| `numFrames`    | `number` | Frames to allocate                              |
| `numChannels?` | `number` | Channels (default 1)                            |
| `sampleRate?`  | `number` | Sample rate in Hz (default: the AudioContext's) |

###### Returns

`Promise`<{ `bufnum`: `number`; `numChannels`: `number`; `numFrames`: `number`; `sampleRate`: `number`; }>

The buffer's number and shape

###### Example

```ts
const buf = await sonic.allocSample(1, 48000, 2);
```

##### createOscChannel()

> **createOscChannel**(`options?`): [`OscChannel`](#oscchannel)

Create an OscChannel for direct worker-to-worklet communication.

The returned channel can be transferred to a Web Worker, allowing that
worker to send OSC directly to the AudioWorklet without going through
the main thread. Works in both SAB and postMessage modes.

For AudioWorkletProcessor use, import from `'supersonic-scsynth/osc-channel'`
which avoids DOM APIs unavailable in the worklet scope.

See docs/WORKERS.md for the full workers guide.

###### Parameters

| Parameter           | Type                       | Description                                                                                                       |
| ------------------- | -------------------------- | ----------------------------------------------------------------------------------------------------------------- |
| `options?`          | { `sourceId?`: `number`; } | Channel options                                                                                                   |
| `options.sourceId?` | `number`                   | The channel's numeric source ID, shown on `'out:osc'` (0 is the main thread's). Default: the next unused, from 1. |

###### Returns

[`OscChannel`](#oscchannel)

###### Throws

If the engine is not initialised

###### Example

```ts
const channel = sonic.createOscChannel();
myWorker.postMessage(
  { channel: channel.transferable },
  channel.transferList,
);
```

##### destroy()

> **destroy**(): `Promise`<`void`>

Destroy the engine completely. The instance cannot be re-used.

Emits `'destroy'`, calls [shutdown](#shutdown), then clears the WASM cache and
all event listeners.

###### Returns

`Promise`<`void`>

##### enableMidi()

> **enableMidi**(`options?`): `Promise`<`object`>

Bring Web MIDI up after [init](#init).

For a page that asks for MIDI when the player does: some browsers prompt
for it, so ask from the gesture that wants it. Rebuilds the MIDI and
gamepad managers, so calling it again re-acquires MIDI.

###### Parameters

| Parameter  | Type                                       | Description                                                  |
| ---------- | ------------------------------------------ | ------------------------------------------------------------ |
| `options?` | `boolean` \| `Record`<`string`, `unknown`> | `true`, or the MIDI manager's options (as the `midi` option) |

###### Returns

`Promise`<`object`>

The MIDI manager ([midi](#midi)), or null if it could not come up
([midiError](#midierror) says why)

##### getCaptureFrames()

> **getCaptureFrames**(): `number`

Get number of audio frames captured so far — or, with no capture running, written in all.

###### Returns

`number`

##### getEngineState()

> **getEngineState**(): [`EngineState`](#enginestate)

Returns the current engine lifecycle state.

One of:

* `'stopped'` — before `init()` or after `shutdown()`/`destroy()`.
* `'booting'` — while `init()` is in progress.
* `'running'` — up. Whether the audio itself is running is the AudioContext's (see the `audiocontext:*` events).
* `'restarting'` — while `reload()` rebuilds the worklet and engine.
* `'error'` — the last `init()` or `reload()` failed; `init()` or `reset()` tries again.

The same states, in the same words, as the C++ `ClockworkEngine::engineState()`. Every change is emitted as
`statechange`.

###### Returns

[`EngineState`](#enginestate)

##### getInfo()

> **getInfo**(): [`SuperSonicInfo`](#supersonicinfo)

Get engine info: sample rate, memory layout, capabilities, and version.

###### Returns

[`SuperSonicInfo`](#supersonicinfo)

###### Throws

If the engine is not initialised

###### Example

```ts
const info = sonic.getInfo();
console.log(`Sample rate: ${info.sampleRate}Hz`);
console.log(`Boot time: ${info.bootTimeMs}ms`);
console.log(`Version: ${info.version}`);
```

##### getLoadedBuffers()

> **getLoadedBuffers**(): [`LoadedBufferInfo`](#loadedbufferinfo)\[]

Get info about all loaded audio buffers.

###### Returns

[`LoadedBufferInfo`](#loadedbufferinfo)\[]

###### Example

```ts
const buffers = sonic.getLoadedBuffers();
for (const buf of buffers) {
  console.log(`Buffer ${buf.bufnum}: ${buf.duration.toFixed(1)}s, ${buf.source}`);
}
```

##### getMaxCaptureDuration()

> **getMaxCaptureDuration**(): `number`

Get maximum capture duration in seconds: the length of the capture ring.

###### Returns

`number`

##### getMetrics()

> **getMetrics**(): [`SuperSonicMetrics`](#supersonicmetrics)

Get current metrics as a named object.

A local memory read in both SAB and postMessage modes — no IPC. Safe to
call from `requestAnimationFrame`. For reading without allocating, see
[getMetricsArray](#getmetricsarray).

See docs/METRICS.md for the full metrics guide.

###### Returns

[`SuperSonicMetrics`](#supersonicmetrics)

###### Example

```ts
const m = sonic.getMetrics();
console.log(`Messages sent: ${m.oscOutMessagesSent}`);
console.log(`Scheduler depth: ${m.engineSchedulerDepth}`);
```

##### getMetricsArray()

> **getMetricsArray**(): `Uint32Array`

Get metrics as a flat Uint32Array for zero-allocation reading.

Returns the same array reference every call — values are updated in-place.
Use [SuperSonic.getMetricsSchema](#getmetricsschema) for offset mappings.

###### Returns

`Uint32Array`

###### Example

```ts
const schema = SuperSonic.getMetricsSchema();
const arr = sonic.getMetricsArray();
const sent = arr[schema.metrics.oscOutMessagesSent.offset];
```

##### getRawTree()

> **getRawTree**(): [`RawTree`](#rawtree)

Get the node tree in flat format with linkage pointers.

More efficient than [getTree](#gettree) for serialization or custom rendering.

###### Returns

[`RawTree`](#rawtree)

##### getScope()

> **getScope**(`scopeNum`, `frames?`): `object`

Copy the newest `frames` frames of a ScopeOut2 scope stream.

Scope slots are lossless interleaved rings with a monotonic write
cursor (see docs/scope-streams-sample-clock.md); this returns the
window ending at the current cursor, zero-filled at the start when
fewer frames are available. Allocates the output per call.
SAB mode only; returns null when uninitialised, out of range, the
slot is inactive, or nothing has been written to it yet.

###### Parameters

| Parameter  | Type     | Description                                                       |
| ---------- | -------- | ----------------------------------------------------------------- |
| `scopeNum` | `number` | Scope slot index, from 0                                          |
| `frames?`  | `number` | Window length in frames (default 1024; at most the ring's length) |

###### Returns

`object`

| Name            | Type           |
| --------------- | -------------- |
| `channels`      | `number`       |
| `frames`        | `number`       |
| `interleaved`   | `Float32Array` |
| `writePosition` | `bigint`       |

##### getScopes()

> **getScopes**(): `object`\[]

List the scope slots in use. SAB mode only; empty otherwise.

###### Returns

`object`\[]

##### getSnapshot()

> **getSnapshot**(): [`Snapshot`](#snapshot)

Get a diagnostic snapshot with metrics (and their descriptions) and JS heap memory info.

Useful for capturing state for bug reports or debugging timing issues.

###### Returns

[`Snapshot`](#snapshot)

##### getSystemReport()

> **getSystemReport**(): [`SystemReport`](#systemreport)

Get a comprehensive system performance report.

Includes hardware info, audio configuration, Chrome playbackStats (if available),
a cross-browser audio health percentage, and a human-readable health assessment.
Useful for diagnosing audio crackling on constrained hardware.

###### Returns

[`SystemReport`](#systemreport)

###### Throws

If the engine is not initialised

###### Example

```ts
const report = sonic.getSystemReport();
console.log(report.health.summary);
if (report.health.audioHealthPct < 95) {
  console.warn('Audio thread struggling:', report.health.issues);
}
```

##### getTree()

> **getTree**(): [`Tree`](#tree)

Get the node tree in hierarchical format.

The mirror has a default capacity of 1024 nodes. If exceeded,
`droppedCount` will be non-zero and the tree may be incomplete,
but audio continues normally.

###### Returns

[`Tree`](#tree)

###### Example

```ts
const tree = sonic.getTree();
function printTree(node, indent = 0) {
  const prefix = '  '.repeat(indent);
  const label = node.type === 'synth' ? node.defName : 'group';
  console.log(`${prefix}[${node.id}] ${label}`);
  for (const child of node.children) printTree(child, indent + 1);
}
if (tree.root) printTree(tree.root);
```

##### init()

> **init**(): `Promise`<`void`>

Initialise the engine.

Loads the WASM binary, creates the AudioContext and AudioWorklet,
starts IO workers, and syncs timing. Emits `'setup'` then `'ready'`
when complete.

Safe to call multiple times: a call while booting gets the same boot,
and a call once booted does nothing.
Call it from a user gesture (click/tap): browsers let audio start only inside one.

###### Returns

`Promise`<`void`>

###### Throws

If required browser features are missing or WASM fails to load.
What was built is taken down again, the engine state becomes `'error'`,
`'error'` is emitted, and `init()` can be called again.

###### Example

```ts
await sonic.init();
// Engine is now ready to send/receive OSC
```

##### isCaptureEnabled()

> **isCaptureEnabled**(): `boolean`

Check if audio capture is currently enabled.

###### Returns

`boolean`

##### isRunning()

> **isRunning**(): `boolean`

Returns true if the engine has finished booting and is ready to send
and receive messages.

Mirrors the C++ `ClockworkEngine::isRunning()` accessor; returns the
same value as the `initialized` getter, exposed as a method to match
the C++ API shape.

###### Returns

`boolean`

##### loadSample()

> **loadSample**(`bufnum`, `source`, `startFrame?`, `numFrames?`): `Promise`<[`LoadSampleResult`](#loadsampleresult)>

Load an audio sample into a buffer slot.

Decodes the audio file (WAV, AIFF, etc.) and copies the samples into
the sample buffer pool. Resolves once the engine has the buffer, which is
then available for use with `PlayBuf`, `BufRd`, etc. A bare filename is
resolved against `sampleBaseURL`.

###### Parameters

| Parameter     | Type                                                                        | Description                                 |
| ------------- | --------------------------------------------------------------------------- | ------------------------------------------- |
| `bufnum`      | `number`                                                                    | Buffer slot number (0 to numBuffers-1)      |
| `source`      | `string` \| `ArrayBuffer` \| `ArrayBufferView`<`ArrayBufferLike`> \| `Blob` | Sample path/URL, raw bytes, or File/Blob    |
| `startFrame?` | `number`                                                                    | First frame to read (default: 0)            |
| `numFrames?`  | `number`                                                                    | Number of frames to read (default: 0 = all) |

###### Returns

`Promise`<[`LoadSampleResult`](#loadsampleresult)>

Buffer info including frame count, channels, and sample rate

###### Example

```ts
// Load from URL:
await sonic.loadSample(0, '/samples/kick.wav');

// Use in a synth:
sonic.send('/s_new', 'sampler', 1001, 0, 0, 'bufnum', 0);
```

##### loadSynthDef()

> **loadSynthDef**(`source`): `Promise`<[`LoadSynthDefResult`](#loadsynthdefresult)>

Load a SynthDef into the engine.

Accepts multiple source types:

* **Name string** — fetched from `synthdefBaseURL` (e.g. `'beep'` → `synthdefBaseURL/beep.scsyndef`)
* **Path/URL string** — fetched directly (one containing `/` or `\`, starting with `http`, or ending in `.scsyndef`)
* **ArrayBuffer / Uint8Array** — raw synthdef bytes
* **File / Blob** — e.g. from a file input

The definition is sent as `/d_recv` through [send](#send-1), so it is recorded
in [loadedSynthDefs](#loadedsynthdefs) and restored after a reload. Resolves once it
has been sent; [sync](#sync) after it waits for the engine to reach it.

###### Parameters

| Parameter | Type                                                                        | Description                                      |
| --------- | --------------------------------------------------------------------------- | ------------------------------------------------ |
| `source`  | `string` \| `ArrayBuffer` \| `ArrayBufferView`<`ArrayBufferLike`> \| `Blob` | SynthDef name, path/URL, raw bytes, or File/Blob |

###### Returns

`Promise`<[`LoadSynthDefResult`](#loadsynthdefresult)>

The extracted name and byte size

###### Throws

If the source type is invalid, the synthdef can't be parsed, a
name is given with no `synthdefBaseURL`, or the fetch fails

###### Example

```ts
// By name (uses synthdefBaseURL):
await sonic.loadSynthDef('beep');

// By URL:
await sonic.loadSynthDef('/assets/synthdefs/pad.scsyndef');

// From raw bytes:
const bytes = await fetch('/my-synth.scsyndef').then(r => r.arrayBuffer());
await sonic.loadSynthDef(bytes);

// From file input:
fileInput.onchange = async (e) => {
  await sonic.loadSynthDef(e.target.files[0]);
};
```

##### loadSynthDefs()

> **loadSynthDefs**(`names`): `Promise`<[`LoadSynthDefResult`](#loadsynthdefresult)\[]>

Load several SynthDefs in parallel, each as [loadSynthDef](#loadsynthdef) would.

###### Parameters

| Parameter | Type        | Description                    |
| --------- | ----------- | ------------------------------ |
| `names`   | `string`\[] | SynthDef names (or paths/URLs) |

###### Returns

`Promise`<[`LoadSynthDefResult`](#loadsynthdefresult)\[]>

Each one's name and byte size, in the order given. Rejects with
the first failure; the others may still have loaded.

###### Example

```ts
const loaded = await sonic.loadSynthDefs(['beep', 'pad', 'kick']);
console.log(loaded.map((d) => d.name));
```

##### nextNodeId()

> **nextNodeId**(): `number`

Get the next unique node ID.

Thread-safe — can be called concurrently from multiple workers and no
two callers will ever receive the same ID. IDs start at 1000: 0 is the
root group and 1–999 are left for the client to assign by hand.

Also available on [OscChannel](#oscchannel) for use in Web Workers.

###### Returns

`number`

A unique node ID (>= 1000)

###### Throws

If the engine is not initialised

###### Example

```ts
const id = sonic.nextNodeId();
sonic.send('/s_new', 'beep', id, 0, 0, 'freq', 440);
```

##### off()

> **off**<`E`>(`event`, `callback`): `this`

Unsubscribe from an event.

###### Type Parameters

| Type Parameter                                           |
| -------------------------------------------------------- |
| `E` *extends* keyof [`SuperSonicEventMap`](#event-types) |

###### Parameters

| Parameter  | Type                                       | Description                                     |
| ---------- | ------------------------------------------ | ----------------------------------------------- |
| `event`    | `E`                                        | Event name                                      |
| `callback` | [`SuperSonicEventMap`](#event-types)\[`E`] | The same function reference passed to [on](#on) |

###### Returns

`this`

##### on()

> **on**<`E`>(`event`, `callback`): () => `void`

Subscribe to an event.

###### Type Parameters

| Type Parameter                                           |
| -------------------------------------------------------- |
| `E` *extends* keyof [`SuperSonicEventMap`](#event-types) |

###### Parameters

| Parameter  | Type                                       | Description                               |
| ---------- | ------------------------------------------ | ----------------------------------------- |
| `event`    | `E`                                        | Event name                                |
| `callback` | [`SuperSonicEventMap`](#event-types)\[`E`] | Handler function (type-checked per event) |

###### Returns

Unsubscribe function — call it to remove the listener

() => `void`

###### Example

```ts
const unsub = sonic.on('in', (msg) => {
  console.log(msg[0], msg.slice(1));
});

// Later:
unsub();
```

##### once()

> **once**<`E`>(`event`, `callback`): () => `void`

Subscribe to an event once. The handler is automatically removed after the first call.
Returns an unsubscribe function (matching [on](#on)).

###### Type Parameters

| Type Parameter                                           |
| -------------------------------------------------------- |
| `E` *extends* keyof [`SuperSonicEventMap`](#event-types) |

###### Parameters

| Parameter  | Type                                       | Description      |
| ---------- | ------------------------------------------ | ---------------- |
| `event`    | `E`                                        | Event name       |
| `callback` | [`SuperSonicEventMap`](#event-types)\[`E`] | Handler function |

###### Returns

Unsubscribe function — call it to remove the listener before it fires

() => `void`

##### purge()

> **purge**(): `Promise`<`void`>

Flush all pending scheduled OSC: clears the engine's scheduler and the IN
ring so nothing already in flight will fire. Resolves when the worklet
confirms — or after a second without an answer, when it emits `'warning'`
(the worklet may be gone).

###### Returns

`Promise`<`void`>

##### recover()

> **recover**(): `Promise`<`boolean`>

Smart recovery — tries a quick resume first, falls back to full reload.

Use when you're not sure if the worklet is still alive (e.g. returning
from a long background period).

Call it from a user gesture (click/tap). Before anything else it makes a
spare AudioContext — browsers let audio start only inside a gesture, and a
context that iOS hands back after an interruption may never render again.
If the quick resume fails, the reload moves onto the spare; otherwise the
spare is closed.

###### Returns

`Promise`<`boolean`>

true if audio is running after recovery; false if the engine was
not initialised or the reload failed

###### Example

```ts
sonic.on('audiocontext:suspended', () => showResumeButton());
resumeButton.onclick = async () => {
  if (await sonic.recover()) hideResumeButton();
};
```

##### reload()

> **reload**(`options?`): `Promise`<`boolean`>

Full reload — destroys and recreates the worklet and WASM, then restores
all previously loaded synthdefs and audio buffers.

Emits `'reload:start'`, then `'setup'` (so you can rebuild groups, FX
chains, and bus routing), `'ready'` and `'reload:complete'`. On failure
it takes down what it built, emits `'reload:failed'` and
`'reload:complete'` with `success: false`, and resolves to false.
A call while a reload is under way gets that reload.
Use when the worklet was killed (e.g. long background, browser reclaimed memory).

###### Parameters

| Parameter               | Type                                 | Description                                                                                                                                 |
| ----------------------- | ------------------------------------ | ------------------------------------------------------------------------------------------------------------------------------------------- |
| `options?`              | { `audioContext?`: `AudioContext`; } | Reload options                                                                                                                              |
| `options.audioContext?` | `AudioContext`                       | A context to reload onto instead of the current one (what [recover](#recover) passes). It becomes SuperSonic's own, closed with the engine. |

###### Returns

`Promise`<`boolean`>

true if reload succeeded; false if it failed or the engine was not initialised

##### removeAllListeners()

> **removeAllListeners**(`event?`): `this`

Remove all listeners for an event, or all listeners entirely.

###### Parameters

| Parameter | Type                                       | Description                              |
| --------- | ------------------------------------------ | ---------------------------------------- |
| `event?`  | keyof [`SuperSonicEventMap`](#event-types) | Event name, or omit to remove everything |

###### Returns

`this`

***

#### Event Types

| Event                                                           | Description                                                                                                                                                                                                                                                                         |
| --------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| <a id="audiocontextinterrupted"></a> `audiocontext:interrupted` | AudioContext was interrupted (iOS-specific). Another app or system event took audio focus. Similar to suspended but triggered externally.                                                                                                                                           |
| <a id="audiocontextresumed"></a> `audiocontext:resumed`         | AudioContext changed to the 'running' state.                                                                                                                                                                                                                                        |
| <a id="audiocontextstatechange"></a> `audiocontext:statechange` | AudioContext state changed. State is one of: `'running'`, `'suspended'`, `'closed'`, or `'interrupted'`.                                                                                                                                                                            |
| <a id="audiocontextsuspended"></a> `audiocontext:suspended`     | AudioContext was suspended (e.g. tab backgrounded, autoplay policy, iOS audio interruption). Show a restart UI and call `recover()` when the user interacts.                                                                                                                        |
| <a id="bufferpoolgrown"></a> `buffer:pool:grown`                | The sample buffer pool grew on demand: a new segment was added.                                                                                                                                                                                                                     |
| <a id="debug"></a> `debug`                                      | A line of the engine's debug output. Not also emitted as `'in'`.                                                                                                                                                                                                                    |
| <a id="destroy-1"></a> `destroy`                                | `destroy()` has been called. Fired first — before the shutdown, and before every listener is removed: the last chance to clean up. Not fired by `shutdown()` or `reset()`.                                                                                                          |
| <a id="error"></a> `error`                                      | Error from any component (worklet, transport, workers), a failed boot, a queued buffer command that failed, a `'setup'` listener that threw, or a MIDI or gamepad subsystem that could not come up.                                                                                 |
| <a id="in"></a> `in`                                            | Decoded OSC message received from the engine. Messages are plain arrays: `[address, ...args]`.                                                                                                                                                                                      |
| <a id="inhtml"></a> `in:html`                                   | Pre-formatted HTML representation of an incoming OSC message with CSS classes for colourisation. Only emitted when listeners are attached.                                                                                                                                          |
| <a id="inosc"></a> `in:osc`                                     | Raw OSC bytes received (before decoding), with when they arrived and the bundle's time tag (`scheduledTime`, null for a message). `sequence` is -1 for a reply the page made itself (MIDI, gamepad).                                                                                |
| <a id="intext"></a> `in:text`                                   | Pre-formatted text representation of an incoming OSC message. Only emitted when listeners are attached or debug logging is enabled.                                                                                                                                                 |
| <a id="loadingcomplete"></a> `loading:complete`                 | An asset finished loading. Size is in bytes.                                                                                                                                                                                                                                        |
| <a id="loadingstart"></a> `loading:start`                       | An asset started loading. Type is `'wasm'`, `'synthdef'`, or `'sample'`. `size` (bytes) is given when known in advance.                                                                                                                                                             |
| <a id="out"></a> `out`                                          | Decoded OSC message sent to the engine. Messages are plain arrays: `[address, ...args]`. Mirrors the `'in'` event for outgoing messages.                                                                                                                                            |
| <a id="outhtml"></a> `out:html`                                 | Pre-formatted HTML representation of an outgoing OSC message with CSS classes for colourisation. Only emitted when listeners are attached.                                                                                                                                          |
| <a id="outosc"></a> `out:osc`                                   | Raw OSC bytes sent to the engine. Includes the sending channel's source ID (0 for the main thread), sequence number, NTP timestamp and the bundle's time tag (null for a message).                                                                                                  |
| <a id="outtext"></a> `out:text`                                 | Pre-formatted text representation of an outgoing OSC message (a bundle as its messages, one per line). Only emitted when listeners are attached or debug logging is enabled.                                                                                                        |
| <a id="ready"></a> `ready`                                      | Fired when the engine is fully booted and ready to receive messages, after `'setup'` (also after a reload). Payload includes browser capabilities and boot timing.                                                                                                                  |
| <a id="reloadcomplete"></a> `reload:complete`                   | Full reload completed, or failed (`success: false`, with the error).                                                                                                                                                                                                                |
| <a id="reloadfailed"></a> `reload:failed`                       | A reload failed. What it built has been taken down; `reload()` and `recover()` answer false.                                                                                                                                                                                        |
| <a id="reloadstart"></a> `reload:start`                         | Full reload started (worklet and WASM will be recreated).                                                                                                                                                                                                                           |
| <a id="resumed"></a> `resumed`                                  | Audio resumed after a suspend: `resume()` restarted the AudioContext and the audio thread is running. Not fired when the context was already running.                                                                                                                               |
| <a id="setup"></a> `setup`                                      | Fired after init completes, before `'ready'`. Use for setting up groups, FX chains, and bus routing. Can be async — init waits for all setup handlers to resolve; one that throws is reported on `'error'`. Also fires after a `reload()`, including one `recover()` falls back to. |
| <a id="shutdown-1"></a> `shutdown`                              | Engine is shutting down. Fired by `shutdown()`, `reset()`, and `destroy()`, when the engine was running or booting.                                                                                                                                                                 |
| <a id="statechange"></a> `statechange`                          | The engine's state changed (see `getEngineState()`), as native sends `/clockwork/statechange`. `error` is set on `'error'`.                                                                                                                                                         |
| <a id="warning"></a> `warning`                                  | Something a host should hear of in every build, for its own log (`{ message }`) — for instance `purge()` getting no answer from the worklet.                                                                                                                                        |

##### request()

> **request**(`address`, `args`, `options`): `Promise`<[`OscMessage`](#oscmessage)>

Send a message and wait for its reply.

Resolves with the decoded reply: the first incoming message at `reply`
(for which `match` is true, when given). Rejects when a `/fail` arrives
first — scsynth's word for a refusal; name another address with `error`,
or pass `error: null` to wait only for the reply — and on the timeout.
The rejection's Error carries the refusal as `reply`.

###### Parameters

| Parameter            | Type                                                                                               | Description                                                       |
| -------------------- | -------------------------------------------------------------------------------------------------- | ----------------------------------------------------------------- |
| `address`            | `string`                                                                                           | The command to send                                               |
| `args`               | [`OscArg`](#osc-argument-types)\[]                                                                 | Its arguments                                                     |
| `options`            | { `error?`: `string`; `match?`: (`msg`) => `boolean`; `reply`: `string`; `timeoutMs?`: `number`; } | What to wait for                                                  |
| `options.error?`     | `string`                                                                                           | The address a refusal comes on (default `'/fail'`; null for none) |
| `options.match?`     | (`msg`) => `boolean`                                                                               | Narrows the reply and the refusal, e.g. by an ID                  |
| `options.reply`      | `string`                                                                                           | The address the answer comes on                                   |
| `options.timeoutMs?` | `number`                                                                                           | How long to wait (default 10000)                                  |

###### Returns

`Promise`<[`OscMessage`](#oscmessage)>

###### Throws

If the engine is not initialised, or `reply` is missing (thrown, not a rejection)

###### Example

```ts
const [, , numUGens, numSynths] = await sonic.request('/status', [], { reply: '/status.reply' });
```

##### reset()

> **reset**(): `Promise`<`void`>

Shutdown and immediately re-initialise.

Equivalent to `await sonic.shutdown(); await sonic.init();`

###### Returns

`Promise`<`void`>

##### resume()

> **resume**(): `Promise`<`boolean`>

Quick resume. If the AudioContext is already running, only checks that the
audio thread is alive: nothing is purged, resynced or emitted. Otherwise it
starts the context, calls [purge](#purge) to drop what queued while it slept,
restarts the drift timer and, if the audio thread is running, resyncs timing
and emits `'resumed'`.

Memory, node tree, and loaded synthdefs are preserved. Does not emit `'setup'`.
Use when you know the worklet is still running (e.g. tab was briefly backgrounded).
Call it from a user gesture: browsers let audio start only inside one.

###### Returns

`Promise`<`boolean`>

true if the audio thread is running after resume; false if not, or if the engine is not initialised

##### sampleInfo()

> **sampleInfo**(`source`, `startFrame?`, `numFrames?`): `Promise`<[`SampleInfo`](#sampleinfo-1)>

Get sample metadata (including content hash) without allocating a buffer.

Fetches, decodes, and hashes the audio, returning the same info that
would appear in the [loadSample](#loadsample) result if the content were loaded.
No buffer slot is consumed and no OSC is sent to the engine.

Use this to inspect content or check for duplicates before loading.

###### Parameters

| Parameter     | Type                                                                        | Description                                 |
| ------------- | --------------------------------------------------------------------------- | ------------------------------------------- |
| `source`      | `string` \| `ArrayBuffer` \| `ArrayBufferView`<`ArrayBufferLike`> \| `Blob` | Sample path/URL, raw bytes, or File/Blob    |
| `startFrame?` | `number`                                                                    | First frame to read (default: 0)            |
| `numFrames?`  | `number`                                                                    | Number of frames to read (default: 0 = all) |

###### Returns

`Promise`<[`SampleInfo`](#sampleinfo-1)>

Sample metadata including hash, frame count, channels, sample rate, and duration

###### Example

```ts
const info = await sonic.sampleInfo('kick.wav');
console.log(info.hash, info.duration, info.numChannels);

const loaded = sonic.getLoadedBuffers();
if (loaded.some(b => b.hash === info.hash)) {
  console.log('Already loaded');
}
```

##### send()

###### Call Signature

> **send**(`address`): `void`

Query server status. Replies with `/status.reply`: unused, numUGens, numSynths, numGroups, numSynthDefs, avgCPU%, peakCPU%, nominalSampleRate, actualSampleRate.

###### Parameters

| Parameter | Type        |
| --------- | ----------- |
| `address` | `"/status"` |

###### Returns

`void`

###### Call Signature

> **send**(`address`): `void`

Query server version. Replies with `/version.reply`: programName, majorVersion, minorVersion, patchVersion, gitBranch, commitHash.

###### Parameters

| Parameter | Type         |
| --------- | ------------ |
| `address` | `"/version"` |

###### Returns

`void`

###### Call Signature

> **send**(`address`, `flag`, `clientID?`): `void`

Register (1) or unregister (0) for server notifications (`/n_go`, `/n_end`, `/n_on`, `/n_off`, `/n_move`). Replies with `/done /notify clientID [maxLogins]`.

###### Parameters

| Parameter   | Type        |
| ----------- | ----------- |
| `address`   | `"/notify"` |
| `flag`      | `0` \| `1`  |
| `clientID?` | `number`    |

###### Returns

`void`

###### Call Signature

> **send**(`address`, `flag`): `void`

Enable/disable OSC message dumping to debug output. 0=off, 1=parsed, 2=hex, 3=both.

###### Parameters

| Parameter | Type                     |
| --------- | ------------------------ |
| `address` | `"/dumpOSC"`             |
| `flag`    | `0` \| `1` \| `2` \| `3` |

###### Returns

`void`

###### Call Signature

> **send**(`address`, `syncID`): `void`

Async. Wait for all prior async commands to complete. Replies with `/synced syncID`.

###### Parameters

| Parameter | Type      |
| --------- | --------- |
| `address` | `"/sync"` |
| `syncID`  | `number`  |

###### Returns

`void`

###### Call Signature

> **send**(`address`): `void`

Query realtime memory usage. Replies with `/rtMemoryStatus.reply`: freeBytes, largestFreeBlockBytes.

###### Parameters

| Parameter | Type                |
| --------- | ------------------- |
| `address` | `"/rtMemoryStatus"` |

###### Returns

`void`

###### Call Signature

> **send**(`address`, `bytes`, `completionMessage?`): `void`

Async. Load a compiled synthdef from bytes. Optional completionMessage is an encoded OSC message executed after loading. Replies with `/done /d_recv`.

###### Parameters

| Parameter            | Type                                             |
| -------------------- | ------------------------------------------------ |
| `address`            | `"/d_recv"`                                      |
| `bytes`              | `ArrayBuffer` \| `Uint8Array`<`ArrayBufferLike`> |
| `completionMessage?` | `ArrayBuffer` \| `Uint8Array`<`ArrayBufferLike`> |

###### Returns

`void`

###### Call Signature

> **send**(`address`, ...`names`): `void`

Free one or more loaded synthdefs by name.

###### Parameters

| Parameter  | Type                       |
| ---------- | -------------------------- |
| `address`  | `"/d_free"`                |
| ...`names` | \[`string`, `...string[]`] |

###### Returns

`void`

###### Call Signature

> **send**(`address`): `void`

Free all loaded synthdefs. Not in the official SC reference but supported by scsynth.

###### Parameters

| Parameter | Type           |
| --------- | -------------- |
| `address` | `"/d_freeAll"` |

###### Returns

`void`

###### Call Signature

> **send**(`address`, `defName`, `nodeID`, `addAction`, `targetID`, ...`controls`): `void`

Create a new synth from a loaded synthdef. addAction: 0=head, 1=tail, 2=before, 3=after, 4=replace. Controls are alternating name/index and value pairs. Values can be numbers or bus mapping strings like `"c0"` (control bus 0) or `"a0"` (audio bus 0). Use nodeID=-1 for auto-assign.

###### Parameters

| Parameter     | Type                      |
| ------------- | ------------------------- |
| `address`     | `"/s_new"`                |
| `defName`     | `string`                  |
| `nodeID`      | `number`                  |
| `addAction`   | [`AddAction`](#addaction) |
| `targetID`    | `number`                  |
| ...`controls` | (`string` \| `number`)\[] |

###### Returns

`void`

###### Call Signature

> **send**(`address`, `nodeID`, ...`controls`): `void`

Get synth control values. Controls can be indices or names. Replies with `/n_set nodeID control value ...`.

###### Parameters

| Parameter     | Type                      |
| ------------- | ------------------------- |
| `address`     | `"/s_get"`                |
| `nodeID`      | `number`                  |
| ...`controls` | (`string` \| `number`)\[] |

###### Returns

`void`

###### Call Signature

> **send**(`address`, `nodeID`, `control`, `count`): `void`

Get sequential synth control values. Control can be an index or name. Replies with `/n_setn nodeID control count values...`. For multiple ranges, use the catch-all overload.

###### Parameters

| Parameter | Type                 |
| --------- | -------------------- |
| `address` | `"/s_getn"`          |
| `nodeID`  | `number`             |
| `control` | `string` \| `number` |
| `count`   | `number`             |

###### Returns

`void`

###### Call Signature

> **send**(`address`, ...`nodeIDs`): `void`

Release client-side synth ID tracking. Synths continue running but are reassigned to reserved negative IDs. Use when you no longer need to communicate with the synth and want to reuse the ID.

###### Parameters

| Parameter    | Type                       |
| ------------ | -------------------------- |
| `address`    | `"/s_noid"`                |
| ...`nodeIDs` | \[`number`, `...number[]`] |

###### Returns

`void`

###### Call Signature

> **send**(`address`, ...`nodeIDs`): `void`

Free (delete) one or more nodes.

###### Parameters

| Parameter    | Type                       |
| ------------ | -------------------------- |
| `address`    | `"/n_free"`                |
| ...`nodeIDs` | \[`number`, `...number[]`] |

###### Returns

`void`

###### Call Signature

> **send**(`address`, `nodeID`, ...`controls`): `void`

Set node control values. Controls are alternating name/index and value pairs. If the node is a group, sets the control on all nodes in the group.

###### Parameters

| Parameter     | Type                      |
| ------------- | ------------------------- |
| `address`     | `"/n_set"`                |
| `nodeID`      | `number`                  |
| ...`controls` | (`string` \| `number`)\[] |

###### Returns

`void`

###### Call Signature

> **send**(`address`, `nodeID`, `control`, `count`, ...`values`): `void`

Set sequential control values starting at the given control index/name. For multiple ranges, use the catch-all overload.

###### Parameters

| Parameter   | Type                 |
| ----------- | -------------------- |
| `address`   | `"/n_setn"`          |
| `nodeID`    | `number`             |
| `control`   | `string` \| `number` |
| `count`     | `number`             |
| ...`values` | `number`\[]          |

###### Returns

`void`

###### Call Signature

> **send**(`address`, `nodeID`, `control`, `count`, `value`): `void`

Fill sequential controls with a single value. For multiple ranges, use the catch-all overload.

###### Parameters

| Parameter | Type                 |
| --------- | -------------------- |
| `address` | `"/n_fill"`          |
| `nodeID`  | `number`             |
| `control` | `string` \| `number` |
| `count`   | `number`             |
| `value`   | `number`             |

###### Returns

`void`

###### Call Signature

> **send**(`address`, ...`pairs`): `void`

Turn nodes on (1) or off (0). Args are repeating \[nodeID, flag] pairs.

###### Parameters

| Parameter  | Type                                   |
| ---------- | -------------------------------------- |
| `address`  | `"/n_run"`                             |
| ...`pairs` | \[`number`, `0` \| `1`, `...number[]`] |

###### Returns

`void`

###### Call Signature

> **send**(`address`, ...`pairs`): `void`

Move nodeA to execute immediately before nodeB. Args are repeating \[nodeA, nodeB] pairs.

###### Parameters

| Parameter  | Type                                 |
| ---------- | ------------------------------------ |
| `address`  | `"/n_before"`                        |
| ...`pairs` | \[`number`, `number`, `...number[]`] |

###### Returns

`void`

###### Call Signature

> **send**(`address`, ...`pairs`): `void`

Move nodeA to execute immediately after nodeB. Args are repeating \[nodeA, nodeB] pairs.

###### Parameters

| Parameter  | Type                                 |
| ---------- | ------------------------------------ |
| `address`  | `"/n_after"`                         |
| ...`pairs` | \[`number`, `number`, `...number[]`] |

###### Returns

`void`

###### Call Signature

> **send**(`address`, `addAction`, `targetID`, ...`nodeIDs`): `void`

Reorder nodes within a group. addAction: 0=head, 1=tail, 2=before target, 3=after target. Does not support 4 (replace).

###### Parameters

| Parameter    | Type                       |
| ------------ | -------------------------- |
| `address`    | `"/n_order"`               |
| `addAction`  | `0` \| `1` \| `2` \| `3`   |
| `targetID`   | `number`                   |
| ...`nodeIDs` | \[`number`, `...number[]`] |

###### Returns

`void`

###### Call Signature

> **send**(`address`, ...`nodeIDs`): `void`

Query node info. Replies with `/n_info` for each node: nodeID, parentGroupID, prevNodeID, nextNodeID, isGroup, \[headNodeID, tailNodeID].

###### Parameters

| Parameter    | Type                       |
| ------------ | -------------------------- |
| `address`    | `"/n_query"`               |
| ...`nodeIDs` | \[`number`, `...number[]`] |

###### Returns

`void`

###### Call Signature

> **send**(`address`, ...`nodeIDs`): `void`

Print control values and calculation rates for each node to debug output. No reply message.

###### Parameters

| Parameter    | Type                       |
| ------------ | -------------------------- |
| `address`    | `"/n_trace"`               |
| ...`nodeIDs` | \[`number`, `...number[]`] |

###### Returns

`void`

###### Call Signature

> **send**(`address`, `nodeID`, ...`mappings`): `void`

Map controls to read from control buses. Mappings are repeating \[control, busIndex] pairs. Set busIndex to -1 to unmap.

###### Parameters

| Parameter     | Type                      |
| ------------- | ------------------------- |
| `address`     | `"/n_map"`                |
| `nodeID`      | `number`                  |
| ...`mappings` | (`string` \| `number`)\[] |

###### Returns

`void`

###### Call Signature

> **send**(`address`, `nodeID`, ...`mappings`): `void`

Map a range of sequential controls to sequential control buses. Mappings are repeating \[control, busIndex, count] triplets.

###### Parameters

| Parameter     | Type                      |
| ------------- | ------------------------- |
| `address`     | `"/n_mapn"`               |
| `nodeID`      | `number`                  |
| ...`mappings` | (`string` \| `number`)\[] |

###### Returns

`void`

###### Call Signature

> **send**(`address`, `nodeID`, ...`mappings`): `void`

Map controls to read from audio buses. Mappings are repeating \[control, busIndex] pairs. Set busIndex to -1 to unmap.

###### Parameters

| Parameter     | Type                      |
| ------------- | ------------------------- |
| `address`     | `"/n_mapa"`               |
| `nodeID`      | `number`                  |
| ...`mappings` | (`string` \| `number`)\[] |

###### Returns

`void`

###### Call Signature

> **send**(`address`, `nodeID`, ...`mappings`): `void`

Map a range of sequential controls to sequential audio buses. Mappings are repeating \[control, busIndex, count] triplets.

###### Parameters

| Parameter     | Type                      |
| ------------- | ------------------------- |
| `address`     | `"/n_mapan"`              |
| `nodeID`      | `number`                  |
| ...`mappings` | (`string` \| `number`)\[] |

###### Returns

`void`

###### Call Signature

> **send**(`address`, ...`args`): `void`

Create new groups. Args are repeating \[groupID, addAction, targetID] triplets. addAction: 0=head, 1=tail, 2=before, 3=after, 4=replace.

###### Parameters

| Parameter | Type                                                            |
| --------- | --------------------------------------------------------------- |
| `address` | `"/g_new"`                                                      |
| ...`args` | \[`number`, [`AddAction`](#addaction), `number`, `...number[]`] |

###### Returns

`void`

###### Call Signature

> **send**(`address`, ...`args`): `void`

Create new parallel groups (children evaluated in unspecified order). Same signature as /g\_new.

###### Parameters

| Parameter | Type                                                            |
| --------- | --------------------------------------------------------------- |
| `address` | `"/p_new"`                                                      |
| ...`args` | \[`number`, [`AddAction`](#addaction), `number`, `...number[]`] |

###### Returns

`void`

###### Call Signature

> **send**(`address`, ...`groupIDs`): `void`

Free all immediate children of one or more groups (groups themselves remain).

###### Parameters

| Parameter     | Type                       |
| ------------- | -------------------------- |
| `address`     | `"/g_freeAll"`             |
| ...`groupIDs` | \[`number`, `...number[]`] |

###### Returns

`void`

###### Call Signature

> **send**(`address`, ...`groupIDs`): `void`

Recursively free all synths inside one or more groups and their nested sub-groups.

###### Parameters

| Parameter     | Type                       |
| ------------- | -------------------------- |
| `address`     | `"/g_deepFree"`            |
| ...`groupIDs` | \[`number`, `...number[]`] |

###### Returns

`void`

###### Call Signature

> **send**(`address`, ...`pairs`): `void`

Move node to head of group. Args are repeating \[groupID, nodeID] pairs.

###### Parameters

| Parameter  | Type                                 |
| ---------- | ------------------------------------ |
| `address`  | `"/g_head"`                          |
| ...`pairs` | \[`number`, `number`, `...number[]`] |

###### Returns

`void`

###### Call Signature

> **send**(`address`, ...`pairs`): `void`

Move node to tail of group. Args are repeating \[groupID, nodeID] pairs.

###### Parameters

| Parameter  | Type                                 |
| ---------- | ------------------------------------ |
| `address`  | `"/g_tail"`                          |
| ...`pairs` | \[`number`, `number`, `...number[]`] |

###### Returns

`void`

###### Call Signature

> **send**(`address`, ...`groupFlagPairs`): `void`

Print group's node tree to debug output. Args are repeating \[groupID, flag] pairs. flag: 0=structure only, non-zero=include control values. No reply message.

###### Parameters

| Parameter           | Type                                 |
| ------------------- | ------------------------------------ |
| `address`           | `"/g_dumpTree"`                      |
| ...`groupFlagPairs` | \[`number`, `number`, `...number[]`] |

###### Returns

`void`

###### Call Signature

> **send**(`address`, ...`groupFlagPairs`): `void`

Query group tree structure. Args are repeating \[groupID, flag] pairs. flag: 0=structure only, non-zero=include control values. Replies with `/g_queryTree.reply`.

###### Parameters

| Parameter           | Type                                 |
| ------------------- | ------------------------------------ |
| `address`           | `"/g_queryTree"`                     |
| ...`groupFlagPairs` | \[`number`, `number`, `...number[]`] |

###### Returns

`void`

###### Call Signature

> **send**(`address`, `nodeID`, `ugenIndex`, `command`, ...`args`): `void`

Send a command to a specific UGen instance within a synth. The command name and args are UGen-specific.

###### Parameters

| Parameter   | Type                               |
| ----------- | ---------------------------------- |
| `address`   | `"/u_cmd"`                         |
| `nodeID`    | `number`                           |
| `ugenIndex` | `number`                           |
| `command`   | `string`                           |
| ...`args`   | [`OscArg`](#osc-argument-types)\[] |

###### Returns

`void`

###### Call Signature

> **send**(`address`, `bufnum`, `numFrames`, `numChannels?`, `sampleRate?`): `void`

Async. Allocate an empty buffer. Queued and rewritten to /b\_allocPtr internally. Use sync() after to ensure completion. Replies with `/done /b_allocPtr bufnum`. Note: completion messages are not supported (dropped during rewrite).

###### Parameters

| Parameter      | Type         |
| -------------- | ------------ |
| `address`      | `"/b_alloc"` |
| `bufnum`       | `number`     |
| `numFrames`    | `number`     |
| `numChannels?` | `number`     |
| `sampleRate?`  | `number`     |

###### Returns

`void`

###### Call Signature

> **send**(`address`, `bufnum`, `path`, `startFrame?`, `numFrames?`): `void`

Async. Allocate a buffer and read an audio file into it. The path is fetched via the configured sampleBaseURL. Queued and rewritten internally. Replies with `/done /b_allocPtr bufnum`.

###### Parameters

| Parameter     | Type             |
| ------------- | ---------------- |
| `address`     | `"/b_allocRead"` |
| `bufnum`      | `number`         |
| `path`        | `string`         |
| `startFrame?` | `number`         |
| `numFrames?`  | `number`         |

###### Returns

`void`

###### Call Signature

> **send**(`address`, `bufnum`, `path`, `startFrame`, `numFrames`, ...`channels`): `void`

Async. Allocate a buffer and read specific channels from an audio file. Queued and rewritten internally. Replies with `/done /b_allocPtr bufnum`.

###### Parameters

| Parameter     | Type                    |
| ------------- | ----------------------- |
| `address`     | `"/b_allocReadChannel"` |
| `bufnum`      | `number`                |
| `path`        | `string`                |
| `startFrame`  | `number`                |
| `numFrames`   | `number`                |
| ...`channels` | `number`\[]             |

###### Returns

`void`

###### Call Signature

> **send**(`address`, `bufnum`, `data`): `void`

Async. SuperSonic extension: allocate a buffer from inline audio file bytes (WAV, FLAC, OGG, etc.) without URL fetch. Queued and rewritten internally. Replies with `/done /b_allocPtr bufnum`.

###### Parameters

| Parameter | Type                                             |
| --------- | ------------------------------------------------ |
| `address` | `"/b_allocFile"`                                 |
| `bufnum`  | `number`                                         |
| `data`    | `ArrayBuffer` \| `Uint8Array`<`ArrayBufferLike`> |

###### Returns

`void`

###### Call Signature

> **send**(`address`, `bufnum`, `completionMessage?`): `void`

Async. Free a buffer. Optional completionMessage is an encoded OSC message executed after freeing. Replies with `/done /b_free bufnum`.

###### Parameters

| Parameter            | Type                                             |
| -------------------- | ------------------------------------------------ |
| `address`            | `"/b_free"`                                      |
| `bufnum`             | `number`                                         |
| `completionMessage?` | `ArrayBuffer` \| `Uint8Array`<`ArrayBufferLike`> |

###### Returns

`void`

###### Call Signature

> **send**(`address`, `bufnum`, `completionMessage?`): `void`

Async. Zero a buffer's sample data. Optional completionMessage is an encoded OSC message executed after zeroing. Replies with `/done /b_zero bufnum`.

###### Parameters

| Parameter            | Type                                             |
| -------------------- | ------------------------------------------------ |
| `address`            | `"/b_zero"`                                      |
| `bufnum`             | `number`                                         |
| `completionMessage?` | `ArrayBuffer` \| `Uint8Array`<`ArrayBufferLike`> |

###### Returns

`void`

###### Call Signature

> **send**(`address`, ...`bufnums`): `void`

Query buffer info. Replies with `/b_info` for each buffer: bufnum, numFrames, numChannels, sampleRate.

###### Parameters

| Parameter    | Type                       |
| ------------ | -------------------------- |
| `address`    | `"/b_query"`               |
| ...`bufnums` | \[`number`, `...number[]`] |

###### Returns

`void`

###### Call Signature

> **send**(`address`, `bufnum`, ...`sampleIndices`): `void`

Get individual sample values. Replies with `/b_set bufnum index value ...`.

###### Parameters

| Parameter          | Type                       |
| ------------------ | -------------------------- |
| `address`          | `"/b_get"`                 |
| `bufnum`           | `number`                   |
| ...`sampleIndices` | \[`number`, `...number[]`] |

###### Returns

`void`

###### Call Signature

> **send**(`address`, `bufnum`, ...`indexValuePairs`): `void`

Set individual buffer samples. Args are repeating \[index, value] pairs after bufnum.

###### Parameters

| Parameter            | Type        |
| -------------------- | ----------- |
| `address`            | `"/b_set"`  |
| `bufnum`             | `number`    |
| ...`indexValuePairs` | `number`\[] |

###### Returns

`void`

###### Call Signature

> **send**(`address`, `bufnum`, `startIndex`, `count`, ...`values`): `void`

Set sequential buffer samples starting at startIndex. For multiple ranges, use the catch-all overload.

###### Parameters

| Parameter    | Type        |
| ------------ | ----------- |
| `address`    | `"/b_setn"` |
| `bufnum`     | `number`    |
| `startIndex` | `number`    |
| `count`      | `number`    |
| ...`values`  | `number`\[] |

###### Returns

`void`

###### Call Signature

> **send**(`address`, `bufnum`, `startIndex`, `count`): `void`

Get sequential sample values. Replies with `/b_setn bufnum startIndex count values...`. For multiple ranges, use the catch-all overload.

###### Parameters

| Parameter    | Type        |
| ------------ | ----------- |
| `address`    | `"/b_getn"` |
| `bufnum`     | `number`    |
| `startIndex` | `number`    |
| `count`      | `number`    |

###### Returns

`void`

###### Call Signature

> **send**(`address`, `bufnum`, `startIndex`, `count`, `value`): `void`

Fill sequential buffer samples with a single value. For multiple ranges, use the catch-all overload.

###### Parameters

| Parameter    | Type        |
| ------------ | ----------- |
| `address`    | `"/b_fill"` |
| `bufnum`     | `number`    |
| `startIndex` | `number`    |
| `count`      | `number`    |
| `value`      | `number`    |

###### Returns

`void`

###### Call Signature

> **send**(`address`, `bufnum`, `command`, ...`args`): `void`

Async. Generate buffer contents. Commands: "sine1", "sine2", "sine3", "cheby", "copy". Flags (for sine/cheby): 1=normalize, 2=wavetable, 4=clear (OR together, e.g. 7=all). Replies with `/done /b_gen bufnum`.

###### Parameters

| Parameter | Type                               |
| --------- | ---------------------------------- |
| `address` | `"/b_gen"`                         |
| `bufnum`  | `number`                           |
| `command` | `string`                           |
| ...`args` | [`OscArg`](#osc-argument-types)\[] |

###### Returns

`void`

###### Call Signature

> **send**(`address`, ...`busIndexValuePairs`): `void`

Set control bus values. Args are repeating \[busIndex, value] pairs.

###### Parameters

| Parameter               | Type        |
| ----------------------- | ----------- |
| `address`               | `"/c_set"`  |
| ...`busIndexValuePairs` | `number`\[] |

###### Returns

`void`

###### Call Signature

> **send**(`address`, ...`busIndices`): `void`

Get control bus values. Replies with `/c_set index value ...`.

###### Parameters

| Parameter       | Type                       |
| --------------- | -------------------------- |
| `address`       | `"/c_get"`                 |
| ...`busIndices` | \[`number`, `...number[]`] |

###### Returns

`void`

###### Call Signature

> **send**(`address`, `startIndex`, `count`, ...`values`): `void`

Set sequential control bus values starting at startIndex. For multiple ranges, use the catch-all overload.

###### Parameters

| Parameter    | Type        |
| ------------ | ----------- |
| `address`    | `"/c_setn"` |
| `startIndex` | `number`    |
| `count`      | `number`    |
| ...`values`  | `number`\[] |

###### Returns

`void`

###### Call Signature

> **send**(`address`, `startIndex`, `count`): `void`

Get sequential control bus values. Replies with `/c_setn startIndex count values...`. For multiple ranges, use the catch-all overload.

###### Parameters

| Parameter    | Type        |
| ------------ | ----------- |
| `address`    | `"/c_getn"` |
| `startIndex` | `number`    |
| `count`      | `number`    |

###### Returns

`void`

###### Call Signature

> **send**(`address`, `startIndex`, `count`, `value`): `void`

Fill sequential control buses with a single value. For multiple ranges, use the catch-all overload.

###### Parameters

| Parameter    | Type        |
| ------------ | ----------- |
| `address`    | `"/c_fill"` |
| `startIndex` | `number`    |
| `count`      | `number`    |
| `value`      | `number`    |

###### Returns

`void`

###### Call Signature

> **send**(`address`, ...`args`): `void`

Send an OSC message to the engine.

This is the primary way to communicate with the engine. Arguments are
automatically encoded to OSC format. The typed overloads cover scsynth's
commands; this one takes any other address, and the multi-range forms of
commands like `/n_setn`, `/b_fill` and `/c_getn`.

Sent at once, except buffer allocation (`/b_alloc`, `/b_allocRead`,
`/b_allocReadChannel`, `/b_allocFile`): those are queued, in the order
written, while their material is fetched and decoded, then reach the
engine as `/b_allocPtr`. A queued command that fails is reported on
`'error'`. [sync](#sync) waits for the queue before its barrier.

Nothing is refused here: a command the browser cannot serve (a file-path
load, a file write) gets the engine's own `/fail`. A `/d_recv`, `/d_free`
or `/d_freeAll` sent this way also updates [loadedSynthDefs](#loadedsynthdefs).

###### Parameters

| Parameter | Type                               | Description                                       |
| --------- | ---------------------------------- | ------------------------------------------------- |
| `address` | `string`                           | OSC address pattern (e.g. `'/s_new'`, `'/n_set'`) |
| ...`args` | [`OscArg`](#osc-argument-types)\[] | Message arguments                                 |

###### Returns

`void`

###### Throws

If the engine is not initialised

###### Throws

If a buffer allocation command is malformed (checked before it is queued)

###### Throws

If the message is larger than the IN ring

###### Example

```ts
// Create a synth
sonic.send('/s_new', 'beep', 1001, 0, 0, 'freq', 440);

// Set a control
sonic.send('/n_set', 1001, 'freq', 880);

// Free a synth
sonic.send('/n_free', 1001);

// Send a synthdef as raw bytes
sonic.send('/d_recv', synthdefBytes);

// Buffer commands are queued; sync() waits for them, then for the engine:
sonic.send('/b_alloc', 0, 44100, 1);
await sonic.sync();
```

***

#### OSC Argument Types

OSC argument types that can be sent in a message.

Plain JS values are mapped to OSC types automatically:

* `number` (integer) → `i` (int32)
* `number` (float) → `f` (float32)
* `string` → `s`
* `boolean` → `T` / `F`
* `Uint8Array` / `ArrayBuffer` → `b` (blob)
* an array → an OSC array (`[` … `]`)

For 64-bit, timetag or UUID types, use the tagged object form:

```ts
{ type: 'int', value: 42 }
{ type: 'float', value: 440 }     // force float32 for whole numbers
{ type: 'string', value: 'hello' }
{ type: 'blob', value: new Uint8Array([1,2,3]) }
{ type: 'bool', value: true }
{ type: 'int64', value: 9007199254740992n }
{ type: 'double', value: 3.141592653589793 }
{ type: 'timetag', value: ntpTimestamp }
{ type: 'uuid', value: uuidBytes } // 16 bytes, OSC tag `u`
```

##### sendOSC()

> **sendOSC**(`oscData`): `void`

Send pre-encoded OSC bytes to the engine.

Use this when you've already encoded the message (e.g. via `SuperSonic.osc.encodeMessage`)
or when sending from a worker that produces raw OSC. Sends bytes as-is without
rewriting — buffer allocation commands (`/b_alloc*`) are not transformed.
Use [send](#send-1) for buffer commands so they are handled correctly. A
`/d_recv` sent this way is not recorded in [loadedSynthDefs](#loadedsynthdefs), so it
is not restored after a reload.

###### Parameters

| Parameter | Type                                             | Description                         |
| --------- | ------------------------------------------------ | ----------------------------------- |
| `oscData` | `ArrayBuffer` \| `Uint8Array`<`ArrayBufferLike`> | Encoded OSC message or bundle bytes |

###### Returns

`void`

###### Throws

If the engine is not initialised

###### Throws

If the message exceeds the IN ring size

###### Example

```ts
const msg = SuperSonic.osc.encodeMessage('/n_set', [1001, 'freq', 880]);
sonic.sendOSC(msg);
```

##### setClockOffset()

> **setClockOffset**(`offsetS`): `void`

Set clock offset for multi-system sync (e.g. against an NTP server).

Shifts all scheduled bundle execution times by the specified offset.
Positive values mean the shared/server clock is ahead of local time.

###### Parameters

| Parameter | Type     | Description                                           |
| --------- | -------- | ----------------------------------------------------- |
| `offsetS` | `number` | Offset in seconds (stored rounded to the millisecond) |

###### Returns

`void`

###### Throws

If the engine is not initialised

##### shutdown()

> **shutdown**(): `Promise`<`void`>

Shut down the engine. The instance can be re-initialised with [init](#init).

Terminates workers and releases memory, and closes the AudioContext if
SuperSonic made it (one passed as the `audioContext` option is left open).
Forgets the loaded synthdefs and buffers. Emits `'shutdown'` when the
engine was running or booting.

###### Returns

`Promise`<`void`>

##### startCapture()

> **startCapture**(): `void`

Start capturing what the engine sends to the audio device. SAB mode only.

###### Returns

`void`

###### Throws

If the engine is not initialised, or not in SAB mode

##### stopCapture()

> **stopCapture**(): `object`

Stop capturing and return what was captured. A capture longer than the
ring ([getMaxCaptureDuration](#getmaxcaptureduration)) keeps the newest ring's worth.

###### Returns

| Name          | Type                                 | Description                                      |
| ------------- | ------------------------------------ | ------------------------------------------------ |
| `channelData` | `Float32Array`<`ArrayBufferLike`>\[] | One array per channel.                           |
| `channels`    | `number`                             | -                                                |
| `frames`      | `number`                             | -                                                |
| `left`        | `Float32Array`                       | The first channel.                               |
| `lost`        | `number`                             | Frames the ring overwrote before they were read. |
| `right`       | `Float32Array`<`ArrayBufferLike`>    | The second channel, or null.                     |
| `sampleRate`  | `number`                             | -                                                |

###### Throws

If the engine is not initialised, or not in SAB mode

##### suspend()

> **suspend**(): `Promise`<`void`>

Suspend the AudioContext and stop the drift timer.

The worklet remains loaded but audio processing stops.
Use [resume](#resume) or [recover](#recover) to restart.

###### Returns

`Promise`<`void`>

##### sync()

> **sync**(`syncId?`, `timeoutMs?`): `Promise`<`void`>

A barrier: resolves once everything sent before it has reached the engine, in order.

Waits for the buffer commands [send](#send-1) has queued, then sends
`/clockwork/sync` and waits for the matching `/clockwork/synced`, which the
audio thread answers when it reaches the message. (Not scsynth's `/sync`:
send that yourself, or use [request](#request), for scsynth's own barrier.)
In postMessage mode it then waits two snapshot intervals, so metrics and
the node tree have caught up. Use after loading synthdefs or buffers,
before creating synths that use them.

###### Parameters

| Parameter    | Type     | Description                                     |
| ------------ | -------- | ----------------------------------------------- |
| `syncId?`    | `number` | Optional custom sync ID (random if omitted)     |
| `timeoutMs?` | `number` | How long to wait for the answer (default 10000) |

###### Returns

`Promise`<`void`>

###### Throws

Rejects on the timeout, or if the engine shuts down first.

###### Example

```ts
await sonic.loadSynthDef('beep');
await sonic.sync();
sonic.send('/s_new', 'beep', 1001, 0, 0);
```

##### getMetricsSchema()

> `static` **getMetricsSchema**(): [`MetricsSchema`](#metricsschema)

Get the metrics schema describing all available metrics.

Includes array offsets for zero-allocation reading via [getMetricsArray](#getmetricsarray),
metric types/units/descriptions, and a declarative UI layout used by the
`<clockwork-metrics>` web component.

See docs/METRICS\_COMPONENT.md for the metrics component guide.

###### Returns

[`MetricsSchema`](#metricsschema)

##### getRawTreeSchema()

> `static` **getRawTreeSchema**(): `Record`<`string`, `unknown`>

Get schema describing the raw flat node tree structure.

###### Returns

`Record`<`string`, `unknown`>

##### getScopeSchema()

> `static` **getScopeSchema**(): `object`

Scope geometry: slot count, per-slot ring frames, channels. These are the
web build's compiled-in defaults, not read from the running engine.

###### Returns

`object`

| Name         | Type     |
| ------------ | -------- |
| `channels`   | `number` |
| `maxScopes`  | `number` |
| `ringFrames` | `number` |

##### getTreeSchema()

> `static` **getTreeSchema**(): `Record`<`string`, `unknown`>

Get schema describing the hierarchical node tree structure.

###### Returns

`Record`<`string`, `unknown`>

***

### OscChannel

OscChannel — unified dispatch for sending OSC to the AudioWorklet.

Obtain a channel via [SuperSonic.createOscChannel](#createoscchannel) on the main thread,
then transfer it to a Web Worker for direct communication with the AudioWorklet.

| Member                                        | Description                                                                                                                                                 |
| --------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------- |
| [`mode`](#mode)                               | Transport mode this channel is using.                                                                                                                       |
| [`transferable`](#transferable)               | Serializable config for transferring this channel to a worker via postMessage.                                                                              |
| [`transferList`](#transferlist)               | Array of transferable objects (MessagePorts) for the postMessage transfer list.                                                                             |
| [`close()`](#close)                           | Close the channel.                                                                                                                                          |
| [`getAndResetMetrics()`](#getandresetmetrics) | Get and reset this channel's local counters (for periodic reporting).                                                                                       |
| [`getMetrics()`](#getmetrics)                 | Get current metrics.                                                                                                                                        |
| [`nextNodeId()`](#nextnodeid)                 | Get the next unique node ID.                                                                                                                                |
| [`now()`](#now)                               | The engine's clock, in NTP seconds: the time its audio thread has reached, readable on any thread the channel is on — a worker cannot see the AudioContext. |
| [`send()`](#send)                             | Send an OSC message: frames it onto the IN ring (SAB) or postMessages it to the worklet (PM).                                                               |
| [`fromTransferable()`](#fromtransferable)     | Reconstruct an OscChannel from data received via postMessage in a worker.                                                                                   |

#### Example

```ts
// Main thread: create and transfer to worker
const channel = sonic.createOscChannel();
myWorker.postMessage(
  { channel: channel.transferable },
  channel.transferList,
);

// Inside worker: reconstruct and send
import { OscChannel } from 'supersonic-scsynth/osc-channel';
const channel = await OscChannel.fromTransferable(event.data.channel);
channel.send(oscBytes);
```

***

#### Constructors

##### Constructor

> **new OscChannel**(): [`OscChannel`](#oscchannel)

###### Returns

[`OscChannel`](#oscchannel)

***

#### Accessors

##### mode

###### Get Signature

> **get** **mode**(): [`TransportMode`](#transportmode)

Transport mode this channel is using.

###### Returns

[`TransportMode`](#transportmode)

##### transferable

###### Get Signature

> **get** **transferable**(): [`OscChannelTransferable`](#oscchanneltransferable-1)

Serializable config for transferring this channel to a worker via postMessage.
In postMessage mode each read hands out a fresh range of node IDs and a port
for more, so read it once per transfer.

###### Example

```ts
worker.postMessage({ ch: channel.transferable }, channel.transferList);
```

###### Returns

[`OscChannelTransferable`](#oscchanneltransferable-1)

##### transferList

###### Get Signature

> **get** **transferList**(): `Transferable`\[]

Array of transferable objects (MessagePorts) for the postMessage transfer list.
Read it after [transferable](#transferable).

###### Example

```ts
worker.postMessage({ ch: channel.transferable }, channel.transferList);
```

###### Returns

`Transferable`\[]

***

#### Methods

##### close()

> **close**(): `void`

Close the channel. In postMessage mode this closes its port; in SAB mode it does nothing.

###### Returns

`void`

##### getAndResetMetrics()

> **getAndResetMetrics**(): [`OscChannelMetrics`](#oscchannelmetrics)

Get and reset this channel's local counters (for periodic reporting).

###### Returns

[`OscChannelMetrics`](#oscchannelmetrics)

##### getMetrics()

> **getMetrics**(): [`OscChannelMetrics`](#oscchannelmetrics)

Get current metrics. In SAB mode these are the shared totals for every sender; in postMessage mode, this channel's own.

###### Returns

[`OscChannelMetrics`](#oscchannelmetrics)

##### nextNodeId()

> **nextNodeId**(): `number`

Get the next unique node ID.

Thread-safe — can be called concurrently from multiple workers and no
two callers will ever receive the same ID. IDs start at 1000: 0 is the
root group and 1–999 are left for the client to assign by hand.

In postMessage mode a worker's channel takes IDs in ranges from the main
thread, asking for the next range before it needs it; it throws if a
tight loop uses a range up before the next has arrived.

###### Returns

`number`

A unique node ID (>= 1000)

##### now()

> **now**(): `number`

The engine's clock, in NTP seconds: the time its audio thread has reached, readable on any thread the channel is
on — a worker cannot see the AudioContext. The clock bundles are stamped on ([SuperSonic.clock](#clock)'s now()),
taken from the audio thread itself once a block: it stands still while the audio does (suspended, interrupted),
and after a reload it is the new engine's from its first block. 0 until the engine has rendered one.

SAB mode reads the sample clock the audio thread publishes into shared memory; postMessage mode hears it from the
audio thread every few blocks and counts on from the last word by the wall clock, a tenth of a second at most.

###### Returns

`number`

NTP seconds, or 0

###### Example

```ts
// Inside a worker: schedule half a second ahead on the engine's own clock
channel.send(osc.encodeBundle(channel.now() + 0.5, [["/s_new", "beep", -1, 0, 0]]));
```

##### send()

> **send**(`oscData`): `boolean`

Send an OSC message: frames it onto the IN ring (SAB) or postMessages it to
the worklet (PM). Classification and scheduling happen on the audio thread
(the engine's OscIngress + BundleScheduler) — the producer never classifies.

###### Parameters

| Parameter | Type         | Description       |
| --------- | ------------ | ----------------- |
| `oscData` | `Uint8Array` | Encoded OSC bytes |

###### Returns

`boolean`

true if sent; false if the IN ring had no room (SAB, counted as
`ringBufferDirectWriteFails`) or the channel is closed (PM)

##### fromTransferable()

> `static` **fromTransferable**(`data`): `Promise`<[`OscChannel`](#oscchannel)>

Reconstruct an OscChannel from data received via postMessage in a worker.
Asynchronous: in SAB mode the worker opens its own instance of the engine
module over the shared memory.

###### Parameters

| Parameter | Type                                                  | Description                                         |
| --------- | ----------------------------------------------------- | --------------------------------------------------- |
| `data`    | [`OscChannelTransferable`](#oscchanneltransferable-1) | The transferable config from `channel.transferable` |

###### Returns

`Promise`<[`OscChannel`](#oscchannel)>

###### Example

```ts
// In a Web Worker:
self.onmessage = async (e) => {
  const channel = await OscChannel.fromTransferable(e.data.ch);
  channel.send(oscBytes);
};
```

## Variables

### osc

> `const` **osc**: `object`

Static OSC encoding/decoding utilities.

Available as `SuperSonic.osc` or via the named `osc` export.
All encode methods return independent copies safe to store or transfer.

#### Type Declaration

| Name                                                      | Type                                                                 | Description                                                                                                                                                                                                                                                                                  |
| --------------------------------------------------------- | -------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| <a id="property-ntp_epoch_offset"></a> `NTP_EPOCH_OFFSET` | `number`                                                             | Seconds between NTP epoch (1900) and Unix epoch (1970): `2208988800`.                                                                                                                                                                                                                        |
| `decode()`                                                | (`data`) => [`OscBundle`](#oscbundle) \| [`OscMessage`](#oscmessage) | Decode an OSC packet (message or bundle).                                                                                                                                                                                                                                                    |
| `encodeBundle()`                                          | (`timeTag`, `packets`) => `Uint8Array`                               | Encode an OSC bundle with multiple packets. **Example** `const time = osc.ntpNow() + 1.0; // 1 second from now osc.encodeBundle(time, [ ['/n_set', 1001, 'freq', 880], ['/n_set', 1001, 'amp', 0.5], ])`                                                                                     |
| `encodeMessage()`                                         | (`address`, `args?`) => `Uint8Array`                                 | Encode an OSC message. **Example** `osc.encodeMessage('/s_new', ['beep', 1001, 0, 0, 'freq', 440])`                                                                                                                                                                                          |
| `encodeSingleBundle()`                                    | (`timeTag`, `address`, `args?`) => `Uint8Array`                      | Encode a single-message bundle (common case optimisation). Equivalent to `encodeBundle(timeTag, [[address, ...args]])` but faster.                                                                                                                                                           |
| `ntpNow()`                                                | () => `number`                                                       | Get the current wall-clock time as an NTP timestamp (seconds since 1900). To schedule against the engine's own clock, prefer `sonic.clock.now()` (or `channel.now()` in a worker). Use this to schedule bundles relative to now: **Example** `const halfSecondFromNow = osc.ntpNow() + 0.5;` |
| `readTimetag()`                                           | (`bundleData`) => `object`                                           | Read the timetag from a bundle without fully decoding it.                                                                                                                                                                                                                                    |

***

#### Example

```ts
import { SuperSonic } from 'supersonic-scsynth';

// Encode a message
const msg = SuperSonic.osc.encodeMessage('/s_new', ['beep', 1001, 0, 0]);

// Encode a timed bundle
const time = SuperSonic.osc.ntpNow() + 0.5; // 500ms from now
const bundle = SuperSonic.osc.encodeBundle(time, [
  ['/s_new', 'beep', 1001, 0, 0, 'freq', 440],
  ['/s_new', 'beep', 1002, 0, 0, 'freq', 660],
]);

// Decode incoming data
const decoded = SuperSonic.osc.decode(rawBytes);
```

## Interfaces

### ActivityLineConfig

Length limits for the text the activity events carry: `debug`, `in:text`
and `out:text`. A limit of 0 means no limit.

#### Properties

| Property                                                | Type     | Description                                                                                                  |
| ------------------------------------------------------- | -------- | ------------------------------------------------------------------------------------------------------------ |
| <a id="enginemaxlinelength"></a> `engineMaxLineLength?` | `number` | Override for the engine's debug lines (the `debug` event). null = use maxLineLength.                         |
| <a id="maxlinelength"></a> `maxLineLength?`             | `number` | Longest text for every activity type: an engine debug line, or one argument of an OSC message. Default: 200. |
| <a id="oscinmaxlinelength"></a> `oscInMaxLineLength?`   | `number` | Override for incoming OSC (`in:text`), per argument. null = use maxLineLength.                               |
| <a id="oscoutmaxlinelength"></a> `oscOutMaxLineLength?` | `number` | Override for outgoing OSC (`out:text`), per argument. null = use maxLineLength.                              |

***

### BootStats

Boot timing statistics.

#### Properties

| Property                                   | Type     | Description                                                 |
| ------------------------------------------ | -------- | ----------------------------------------------------------- |
| <a id="initduration"></a> `initDuration`   | `number` | Total boot duration in ms, or null if not yet booted.       |
| <a id="initstarttime"></a> `initStartTime` | `number` | Timestamp when init() started (performance.now()), or null. |

***

### ClockworkClock

Engine session-timeline service. Tempo, beat origin, transport, meter and
NTP-derived "now." Accessed via [SuperSonic.clock](#clock), after `init()`.

Each field is read/written independently — no multi-field coherence
guarantee. There is no Ableton Link on the web: `setLinkEnabled(true)` is
ignored with a console warning, `isLinkEnabled()` is always false and
`numPeers()` always 0.

| Member                                      | Description                                                                                 |
| ------------------------------------------- | ------------------------------------------------------------------------------------------- |
| [`beatAtTime()`](#beatattime)               |                                                                                             |
| [`forceBeatAtTime()`](#forcebeatattime)     | Identical to requestBeatAtTime on the web.                                                  |
| [`getBeatOriginNtp()`](#getbeatoriginntp)   |                                                                                             |
| [`getBpm()`](#getbpm)                       |                                                                                             |
| [`getClockOffset()`](#getclockoffset)       | The clock offset set by setClockOffset, in milliseconds.                                    |
| [`getDriftOffset()`](#getdriftoffset)       | Drift between the AudioContext and the wall clock, in milliseconds (signed).                |
| [`getGeneration()`](#getgeneration)         | One more each time the beat grid moves (a tempo change, a new origin), by any writer.       |
| [`getIsPlayingAtNtp()`](#getisplayingatntp) |                                                                                             |
| [`getMeter()`](#getmeter)                   | The meter set by setMeter.                                                                  |
| [`getNTPStartTime()`](#getntpstarttime)     | NTP time (seconds since 1900) when the AudioContext started.                                |
| [`initialize()`](#initialize)               | Measure the NTP start time and the first drift.                                             |
| [`isLinkEnabled()`](#islinkenabled)         | Always false on the web.                                                                    |
| [`isPlaying()`](#isplaying)                 |                                                                                             |
| [`now()`](#now)                             | Current NTP time as seen by the audio thread.                                               |
| [`nowAt()`](#nowat)                         | Compute audio-thread NTP for a specific AudioContext.currentTime.                           |
| [`numPeers()`](#numpeers)                   | Always 0 on the web.                                                                        |
| [`phaseAtTime()`](#phaseattime)             |                                                                                             |
| [`requestBeatAtTime()`](#requestbeatattime) |                                                                                             |
| [`reset()`](#reset)                         | Stop the drift timer and forget the timing state.                                           |
| [`resync()`](#resync)                       | Re-measure the NTP start time and drift, as after a suspend.                                |
| [`setBpm()`](#setbpm)                       | Change the tempo without moving the beat playing at the instant it changes.                 |
| [`setClockOffset()`](#setclockoffset)       | Set the clock offset for multi-system sync, in seconds (stored rounded to the millisecond). |
| [`setIsPlaying()`](#setisplaying)           |                                                                                             |
| [`setLinkEnabled()`](#setlinkenabled)       | Ignored on the web (with a console warning when true): Link is native only.                 |
| [`setMeter()`](#setmeter)                   | Set the meter: how quarter-note beats group into bars.                                      |
| [`startDriftTimer()`](#startdrifttimer)     | Start re-measuring the drift periodically.                                                  |
| [`stopDriftTimer()`](#stopdrifttimer)       | Stop re-measuring the drift.                                                                |
| [`timeAtBeat()`](#timeatbeat)               |                                                                                             |
| [`updateDriftOffset()`](#updatedriftoffset) | Re-measure the drift now.                                                                   |
| [`wallNow()`](#wallnow)                     | Current NTP time from the system wall clock.                                                |

#### Methods

##### beatAtTime()

> **beatAtTime**(`ntpSeconds`, `quantum`): `number`

###### Parameters

| Parameter    | Type     |
| ------------ | -------- |
| `ntpSeconds` | `number` |
| `quantum`    | `number` |

###### Returns

`number`

##### forceBeatAtTime()

> **forceBeatAtTime**(`beat`, `atNtpSeconds`, `quantum`): `void`

Identical to [requestBeatAtTime](#requestbeatattime) on the web.

###### Parameters

| Parameter      | Type     |
| -------------- | -------- |
| `beat`         | `number` |
| `atNtpSeconds` | `number` |
| `quantum`      | `number` |

###### Returns

`void`

##### getBeatOriginNtp()

> **getBeatOriginNtp**(): `number`

###### Returns

`number`

##### getBpm()

> **getBpm**(): `number`

###### Returns

`number`

##### getClockOffset()

> **getClockOffset**(): `number`

The clock offset set by [setClockOffset](#setclockoffset-1), in milliseconds.

###### Returns

`number`

##### getDriftOffset()

> **getDriftOffset**(): `number`

Drift between the AudioContext and the wall clock, in milliseconds (signed).

###### Returns

`number`

##### getGeneration()

> **getGeneration**(): `number`

One more each time the beat grid moves (a tempo change, a new origin), by
any writer. A follower keeping its own copy of the grid reads this, then
the grid, and reads the grid again when it has changed. In postMessage
mode only this clock's own changes count.

###### Returns

`number`

##### getIsPlayingAtNtp()

> **getIsPlayingAtNtp**(): `number`

###### Returns

`number`

##### getMeter()

> **getMeter**(): `object`

The meter set by [setMeter](#setmeter).

###### Returns

`object`

| Name  | Type     |
| ----- | -------- |
| `den` | `number` |
| `num` | `number` |

##### getNTPStartTime()

> **getNTPStartTime**(): `number`

NTP time (seconds since 1900) when the AudioContext started.

###### Returns

`number`

##### initialize()

> **initialize**(): `Promise`<`void`>

Measure the NTP start time and the first drift. `init()` does this; a client does not need to.

###### Returns

`Promise`<`void`>

##### isLinkEnabled()

> **isLinkEnabled**(): `boolean`

Always `false` on the web.

###### Returns

`boolean`

##### isPlaying()

> **isPlaying**(): `boolean`

###### Returns

`boolean`

##### now()

> **now**(): `number`

Current NTP time as seen by the audio thread. Use this for scheduling:
`sonic.clock.now() + 0.05` gives a timestamp 50ms in audio-clock
future, which the audio thread reaches in 50ms of audio time —
independent of any wall-clock-vs-audio-clock skew.

###### Returns

`number`

##### nowAt()

> **nowAt**(`audioCurrentTime`): `number`

Compute audio-thread NTP for a specific `AudioContext.currentTime`.
Lower-level than [now](#now-1) — pass a value obtained from
`audioContext.getOutputTimestamp()` for sample-aligned scheduling.

###### Parameters

| Parameter          | Type     |
| ------------------ | -------- |
| `audioCurrentTime` | `number` |

###### Returns

`number`

##### numPeers()

> **numPeers**(): `number`

Always `0` on the web.

###### Returns

`number`

##### phaseAtTime()

> **phaseAtTime**(`ntpSeconds`, `quantum`): `number`

###### Parameters

| Parameter    | Type     |
| ------------ | -------- |
| `ntpSeconds` | `number` |
| `quantum`    | `number` |

###### Returns

`number`

##### requestBeatAtTime()

> **requestBeatAtTime**(`beat`, `atNtpSeconds`, `quantum`): `void`

###### Parameters

| Parameter      | Type     |
| -------------- | -------- |
| `beat`         | `number` |
| `atNtpSeconds` | `number` |
| `quantum`      | `number` |

###### Returns

`void`

##### reset()

> **reset**(): `void`

Stop the drift timer and forget the timing state.

###### Returns

`void`

##### resync()

> **resync**(): `void`

Re-measure the NTP start time and drift, as after a suspend.

###### Returns

`void`

##### setBpm()

> **setBpm**(`bpm`, `atNtpSeconds?`): `void`

Change the tempo without moving the beat playing at the instant it changes.

###### Parameters

| Parameter       | Type     | Description                                                                                                           |
| --------------- | -------- | --------------------------------------------------------------------------------------------------------------------- |
| `bpm`           | `number` | -                                                                                                                     |
| `atNtpSeconds?` | `number` | the instant the tempo changes (omitted or 0: now). A scheduler working ahead gives the time its change will be heard. |

###### Returns

`void`

##### setClockOffset()

> **setClockOffset**(`offsetS`): `void`

Set the clock offset for multi-system sync, in seconds (stored rounded to the millisecond).

###### Parameters

| Parameter | Type     |
| --------- | -------- |
| `offsetS` | `number` |

###### Returns

`void`

##### setIsPlaying()

> **setIsPlaying**(`playing`, `atNtpSeconds?`): `void`

###### Parameters

| Parameter       | Type      |
| --------------- | --------- |
| `playing`       | `boolean` |
| `atNtpSeconds?` | `number`  |

###### Returns

`void`

##### setLinkEnabled()

> **setLinkEnabled**(`enabled`): `void`

Ignored on the web (with a console warning when `true`): Link is native only.

###### Parameters

| Parameter | Type      |
| --------- | --------- |
| `enabled` | `boolean` |

###### Returns

`void`

##### setMeter()

> **setMeter**(`num`, `den`): `boolean`

Set the meter: how quarter-note beats group into bars. 4/4 until set.
The beat grid is not moved.

###### Parameters

| Parameter | Type     | Description                              |
| --------- | -------- | ---------------------------------------- |
| `num`     | `number` | beats per bar: a whole number, 1 or more |
| `den`     | `number` | 1, 2, 4, 8, 16 or 32                     |

###### Returns

`boolean`

false, and nothing changed, for any other meter

##### startDriftTimer()

> **startDriftTimer**(): `void`

Start re-measuring the drift periodically.

###### Returns

`void`

##### stopDriftTimer()

> **stopDriftTimer**(): `void`

Stop re-measuring the drift.

###### Returns

`void`

##### timeAtBeat()

> **timeAtBeat**(`beat`, `quantum`): `number`

###### Parameters

| Parameter | Type     |
| --------- | -------- |
| `beat`    | `number` |
| `quantum` | `number` |

###### Returns

`number`

##### updateDriftOffset()

> **updateDriftOffset**(): `void`

Re-measure the drift now.

###### Returns

`void`

##### wallNow()

> **wallNow**(): `number`

Current NTP time from the system wall clock. Use only when matching
against external wall-clock events; prefer [now](#now-1) for scheduling
engine events.

###### Returns

`number`

***

### LoadedBufferInfo

Info about a loaded audio buffer, returned by [SuperSonic.getLoadedBuffers](#getloadedbuffers).

#### Extends

* [`SampleInfo`](#sampleinfo-1)

***

#### Properties

| Property                               | Type     | Description                                                | Inherited from                                                |
| -------------------------------------- | -------- | ---------------------------------------------------------- | ------------------------------------------------------------- |
| <a id="bufnum"></a> `bufnum`           | `number` | Buffer slot number.                                        | -                                                             |
| <a id="duration"></a> `duration`       | `number` | Duration in seconds.                                       | [`SampleInfo`](#sampleinfo-1).[`duration`](#duration-2)       |
| <a id="hash"></a> `hash`               | `string` | SHA-256 hex hash of the decoded interleaved audio content. | [`SampleInfo`](#sampleinfo-1).[`hash`](#hash-2)               |
| <a id="numchannels"></a> `numChannels` | `number` | Number of channels.                                        | [`SampleInfo`](#sampleinfo-1).[`numChannels`](#numchannels-2) |
| <a id="numframes"></a> `numFrames`     | `number` | Number of sample frames.                                   | [`SampleInfo`](#sampleinfo-1).[`numFrames`](#numframes-2)     |
| <a id="samplerate"></a> `sampleRate`   | `number` | Sample rate in Hz.                                         | [`SampleInfo`](#sampleinfo-1).[`sampleRate`](#samplerate-2)   |
| <a id="source"></a> `source`           | `string` | Original source path/URL, or null for inline data.         | [`SampleInfo`](#sampleinfo-1).[`source`](#source-2)           |

***

### LoadSampleResult

Result from [SuperSonic.loadSample](#loadsample).

#### Extends

* [`SampleInfo`](#sampleinfo-1)

***

#### Properties

| Property                                 | Type     | Description                                                | Inherited from                                                |
| ---------------------------------------- | -------- | ---------------------------------------------------------- | ------------------------------------------------------------- |
| <a id="bufnum-1"></a> `bufnum`           | `number` | Buffer slot the sample was loaded into.                    | -                                                             |
| <a id="duration-1"></a> `duration`       | `number` | Duration in seconds.                                       | [`SampleInfo`](#sampleinfo-1).[`duration`](#duration-2)       |
| <a id="hash-1"></a> `hash`               | `string` | SHA-256 hex hash of the decoded interleaved audio content. | [`SampleInfo`](#sampleinfo-1).[`hash`](#hash-2)               |
| <a id="numchannels-1"></a> `numChannels` | `number` | Number of channels.                                        | [`SampleInfo`](#sampleinfo-1).[`numChannels`](#numchannels-2) |
| <a id="numframes-1"></a> `numFrames`     | `number` | Number of sample frames.                                   | [`SampleInfo`](#sampleinfo-1).[`numFrames`](#numframes-2)     |
| <a id="samplerate-1"></a> `sampleRate`   | `number` | Sample rate in Hz.                                         | [`SampleInfo`](#sampleinfo-1).[`sampleRate`](#samplerate-2)   |
| <a id="source-1"></a> `source`           | `string` | Original source path/URL, or null for inline data.         | [`SampleInfo`](#sampleinfo-1).[`source`](#source-2)           |

***

### LoadSynthDefResult

Result from [SuperSonic.loadSynthDef](#loadsynthdef).

#### Properties

| Property                 | Type     | Description                           |
| ------------------------ | -------- | ------------------------------------- |
| <a id="name"></a> `name` | `string` | Extracted SynthDef name.              |
| <a id="size"></a> `size` | `number` | Size of the synthdef binary in bytes. |

***

### MetricDefinition

Schema entry describing a single metric field.

#### Properties

| Property                               | Type                                                            | Description                                                                                                                                                        |
| -------------------------------------- | --------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| <a id="description"></a> `description` | `string`                                                        | Human-readable description.                                                                                                                                        |
| <a id="nativeonly"></a> `nativeOnly?`  | `boolean`                                                       | Native only — nothing writes it on the web, where it reads 0.                                                                                                      |
| <a id="offset"></a> `offset`           | `number`                                                        | Offset into the flat metrics Uint32Array.                                                                                                                          |
| <a id="signed"></a> `signed?`          | `boolean`                                                       | Whether the value should be read as signed int32.                                                                                                                  |
| <a id="slot"></a> `slot?`              | `number`                                                        | For a metric SuperSonic declares: its index within the range clockwork leaves for them.                                                                            |
| <a id="type"></a> `type`               | `"counter"` \| `"gauge"` \| `"constant"` \| `"enum"` \| `"u32"` | Metric type: counter (cumulative), gauge (current), constant, or enum; `'u32'` for the metrics SuperSonic declares (the sample buffer pool and `loadedSynthDefs`). |
| <a id="unit"></a> `unit?`              | `string`                                                        | Unit of measurement.                                                                                                                                               |
| <a id="values"></a> `values?`          | `string`\[]                                                     | Enum value names (for type 'enum').                                                                                                                                |

***

### MetricsSchema

Metrics schema returned by [SuperSonic.getMetricsSchema](#getmetricsschema).

Contains metric definitions with array offsets (for zero-allocation reading)
and a declarative UI layout for rendering metrics panels.

#### Properties

| Property                               | Type                                                                | Description                                                                                                                                                                                                                                                                                                                                                                            |                                                |
| -------------------------------------- | ------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ---------------------------------------------- |
| <a id="composites"></a> `composites`   | `Record`<`string`, { `description`: `string`; }>                    | Descriptions for rows combining several metrics in one reading ("current                                                                                                                                                                                                                                                                                                               | peak", ...), shared by web and native layouts. |
| <a id="layout"></a> `layout`           | `object`                                                            | Panel structure for rendering a metrics UI. Used by the `<clockwork-metrics>` web component.                                                                                                                                                                                                                                                                                           |                                                |
| `layout.panels`                        | `object`\[]                                                         | -                                                                                                                                                                                                                                                                                                                                                                                      |                                                |
| <a id="metrics"></a> `metrics`         | `Record`<`string`, [`MetricDefinition`](#metricdefinition)>         | Every metric in the flat array [SuperSonic.getMetricsArray](#getmetricsarray) returns, by name, with its offset, type, unit and description: clockwork's own and the ones SuperSonic declares. Native-only ones are marked `nativeOnly` and read 0 on the web. See [SuperSonicMetrics](#supersonicmetrics) for how these names relate to [SuperSonic.getMetrics](#getmetrics-1)' keys. |                                                |
| <a id="nativestats"></a> `nativeStats` | `Record`<`string`, [`NativeStatDefinition`](#nativestatdefinition)> | NATIVE\_STATS shm segment descriptions (native only). `index` is the u32 slot within that segment, not an offset into the metrics array.                                                                                                                                                                                                                                               |                                                |

***

### NativeStatDefinition

A NATIVE\_STATS segment entry (native only).

#### Properties

| Property                                 | Type                     | Description                                           |
| ---------------------------------------- | ------------------------ | ----------------------------------------------------- |
| <a id="description-1"></a> `description` | `string`                 | Human-readable description.                           |
| <a id="index"></a> `index`               | `number`                 | u32 slot within the NATIVE\_STATS segment.            |
| <a id="type-1"></a> `type`               | `"counter"` \| `"gauge"` | Metric type: counter (cumulative) or gauge (current). |
| <a id="unit-1"></a> `unit?`              | `string`                 | Unit of measurement.                                  |

***

### OscBundle

Decoded OSC bundle containing a timetag and nested packets.

#### Properties

| Property                       | Type                                                          | Description                          |
| ------------------------------ | ------------------------------------------------------------- | ------------------------------------ |
| <a id="packets"></a> `packets` | ([`OscBundle`](#oscbundle) \| [`OscMessage`](#oscmessage))\[] | Nested messages or bundles.          |
| <a id="timetag"></a> `timeTag` | `number`                                                      | NTP timestamp in seconds since 1900. |

***

### OscChannelMetrics

OscChannel metrics counters.

#### Properties

| Property                                 | Type     |
| ---------------------------------------- | -------- |
| <a id="bytessent"></a> `bytesSent`       | `number` |
| <a id="messagessent"></a> `messagesSent` | `number` |

***

### OscChannelPMTransferable

Transferable config for postMessage mode OscChannel.

#### Properties

| Property                                | Type            | Description                                    |
| --------------------------------------- | --------------- | ---------------------------------------------- |
| <a id="mode-2"></a> `mode`              | `"postMessage"` | -                                              |
| <a id="nodeidport"></a> `nodeIdPort?`   | `MessagePort`   | The port the worker asks for more node IDs on. |
| <a id="nodeidrange"></a> `nodeIdRange?` | `object`        | The node IDs handed to the worker up front.    |
| `nodeIdRange.from`                      | `number`        | -                                              |
| `nodeIdRange.to`                        | `number`        | -                                              |
| <a id="port"></a> `port`                | `MessagePort`   | -                                              |
| <a id="sourceid"></a> `sourceId`        | `number`        | -                                              |

***

### OscChannelSABTransferable

Transferable config for SAB mode OscChannel.

#### Properties

| Property                                         | Type                         | Description                                                             |
| ------------------------------------------------ | ---------------------------- | ----------------------------------------------------------------------- |
| <a id="bufferconstants-1"></a> `bufferConstants` | `Record`<`string`, `number`> | -                                                                       |
| <a id="controlindices"></a> `controlIndices`     | `Record`<`string`, `number`> | -                                                                       |
| <a id="mode-3"></a> `mode`                       | `"sab"`                      | -                                                                       |
| <a id="ringbufferbase-1"></a> `ringBufferBase`   | `number`                     | -                                                                       |
| <a id="sharedbuffer-1"></a> `sharedBuffer`       | `SharedArrayBuffer`          | -                                                                       |
| <a id="sourceid-1"></a> `sourceId`               | `number`                     | -                                                                       |
| <a id="wasmmemory"></a> `wasmMemory`             | `Memory`                     | The engine's memory: the receiving worker opens its own client over it. |
| <a id="wasmmodule"></a> `wasmModule`             | `Module`                     | The compiled engine module, shared rather than compiled again.          |

***

### RawTree

Flat node tree returned by [SuperSonic.getRawTree](#getrawtree).

Contains all nodes as a flat array with parent/sibling linkage pointers.
More efficient than the hierarchical tree for serialization or custom rendering.

#### Properties

| Property                                 | Type                             | Description                          |
| ---------------------------------------- | -------------------------------- | ------------------------------------ |
| <a id="droppedcount"></a> `droppedCount` | `number`                         | Nodes that exceeded mirror capacity. |
| <a id="nodecount"></a> `nodeCount`       | `number`                         | Total number of nodes.               |
| <a id="nodes"></a> `nodes`               | [`RawTreeNode`](#rawtreenode)\[] | Flat array of all nodes.             |
| <a id="version"></a> `version`           | `number`                         | Increments on any tree change.       |

***

### RawTreeNode

A node in the flat (raw) tree representation with linkage pointers.

#### Properties

| Property                             | Type              | Description                                                       |
| ------------------------------------ | ----------------- | ----------------------------------------------------------------- |
| <a id="defname"></a> `defName`       | `string`          | SynthDef name (synths only, empty string for groups).             |
| <a id="headid"></a> `headId`         | `number`          | First child node ID (groups only, -1 if empty).                   |
| <a id="id"></a> `id`                 | `number`          | Numeric node ID; null for a node that has only a UUID.            |
| <a id="isgroup"></a> `isGroup`       | `boolean`         | true if group, false if synth.                                    |
| <a id="listens"></a> `listens`       | `boolean`         | Whether the node subscribes to input.                             |
| <a id="nextid"></a> `nextId`         | `number`          | Next sibling node ID (-1 if none).                                |
| <a id="outpeak"></a> `outPeak`       | `number`          | Peak of the node's output over the last block.                    |
| <a id="parentid"></a> `parentId`     | `number`          | Parent node ID (-1 for root).                                     |
| <a id="parentuuid"></a> `parentUuid` | [`UUID`](#uuid-1) | The parent's UUID, or null.                                       |
| <a id="previd"></a> `prevId`         | `number`          | Previous sibling node ID (-1 if none).                            |
| <a id="synthcount"></a> `synthCount` | `number`          | Synths under this node, itself included.                          |
| <a id="uuid"></a> `uuid`             | [`UUID`](#uuid-1) | UUID if the node was created with a UUID node ID, null otherwise. |

***

### RingBufferUsage

How full a ring buffer is, as [SuperSonic.getMetrics](#getmetrics-1) reports it.

#### Properties

| Property                                     | Type     | Description                                  |
| -------------------------------------------- | -------- | -------------------------------------------- |
| <a id="bytes"></a> `bytes`                   | `number` | Bytes in use now.                            |
| <a id="capacity"></a> `capacity`             | `number` | The ring's size in bytes.                    |
| <a id="peakbytes"></a> `peakBytes`           | `number` | Most bytes ever in use (high water mark).    |
| <a id="peakpercentage"></a> `peakPercentage` | `number` | `peakBytes` as a percentage of `capacity`.   |
| <a id="percentage"></a> `percentage`         | `number` | Bytes in use, as a percentage of `capacity`. |

***

### SampleInfo

Metadata about decoded audio content.

Returned by [SuperSonic.sampleInfo](#sampleinfo). Also the shape of each entry
in [SuperSonic.getLoadedBuffers](#getloadedbuffers) (with `bufnum`) and the return
value of [SuperSonic.loadSample](#loadsample) (with `bufnum`).

#### Extended by

* [`LoadedBufferInfo`](#loadedbufferinfo)
* [`LoadSampleResult`](#loadsampleresult)

***

#### Properties

| Property                                 | Type     | Description                                                |
| ---------------------------------------- | -------- | ---------------------------------------------------------- |
| <a id="duration-2"></a> `duration`       | `number` | Duration in seconds.                                       |
| <a id="hash-2"></a> `hash`               | `string` | SHA-256 hex hash of the decoded interleaved audio content. |
| <a id="numchannels-2"></a> `numChannels` | `number` | Number of channels.                                        |
| <a id="numframes-2"></a> `numFrames`     | `number` | Number of sample frames.                                   |
| <a id="samplerate-2"></a> `sampleRate`   | `number` | Sample rate in Hz.                                         |
| <a id="source-2"></a> `source`           | `string` | Original source path/URL, or null for inline data.         |

### Snapshot

Diagnostic snapshot returned by [SuperSonic.getSnapshot](#getsnapshot).

Captures metrics with descriptions, and JS heap memory info. Useful for
bug reports and debugging timing issues. For the node tree, see
[SuperSonic.getRawTree](#getrawtree).

#### Properties

| Property                           | Type                                                                                                                                                    | Description                                                                                                       |
| ---------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------- |
| <a id="memory"></a> `memory`       | `object`                                                                                                                                                | JS heap memory info (Chrome only, null in other browsers).                                                        |
| `memory.jsHeapSizeLimit`           | `number`                                                                                                                                                | -                                                                                                                 |
| `memory.totalJSHeapSize`           | `number`                                                                                                                                                | -                                                                                                                 |
| `memory.usedJSHeapSize`            | `number`                                                                                                                                                | -                                                                                                                 |
| <a id="metrics-1"></a> `metrics`   | `Record`<`string`, { `description?`: `string`; `value`: [`SuperSonicMetrics`](#supersonicmetrics)\[keyof [`SuperSonicMetrics`](#supersonicmetrics)]; }> | Every [SuperSonicMetrics](#supersonicmetrics) value by name, with clockwork's description of it where it has one. |
| <a id="timestamp"></a> `timestamp` | `string`                                                                                                                                                | ISO 8601 timestamp when the snapshot was taken.                                                                   |

### SuperSonicInfo

Engine info returned by [SuperSonic.getInfo](#getinfo).

#### Properties

| Property                                       | Type      | Description                                                     |
| ---------------------------------------------- | --------- | --------------------------------------------------------------- |
| <a id="boottimems"></a> `bootTimeMs`           | `number`  | Time taken to boot in ms, or null if not yet booted.            |
| <a id="capabilities"></a> `capabilities`       | `object`  | Browser capability detection results.                           |
| `capabilities.atomics`                         | `boolean` | -                                                               |
| `capabilities.audioWorklet`                    | `boolean` | -                                                               |
| `capabilities.crossOriginIsolated`             | `boolean` | -                                                               |
| `capabilities.playbackStats`                   | `boolean` | -                                                               |
| `capabilities.sharedArrayBuffer`               | `boolean` | -                                                               |
| `capabilities.webWorker`                       | `boolean` | -                                                               |
| <a id="guestmemorysize"></a> `guestMemorySize` | `number`  | Size of the engine's fixed memory region, in bytes.             |
| <a id="samplerate-3"></a> `sampleRate`         | `number`  | AudioContext sample rate (e.g. 48000).                          |
| <a id="totalmemory"></a> `totalMemory`         | `number`  | WebAssembly memory committed at boot, in bytes.                 |
| <a id="version-1"></a> `version`               | `string`  | The engine's version string as the worklet reports it, or null. |
| <a id="wasmheapsize"></a> `wasmHeapSize`       | `number`  | Size of the WASM heap in the memory layout, in bytes.           |

***

### SuperSonicMetrics

Metrics returned by [SuperSonic.getMetrics](#getmetrics-1), by name.

Counters are cumulative; gauges reflect current state. Most keys are also
in [SuperSonic.getMetricsSchema](#getmetricsschema), which gives their descriptions,
units and offsets into [SuperSonic.getMetricsArray](#getmetricsarray). Where the two
differ: the schema's flat ring-buffer numbers (`inBufferUsedBytes`,
`inBufferPeakBytes`, `inBufferCapacity` and the same for `out` and
`nrtOut`) are grouped here as `inBufferUsed`, `outBufferUsed` and
`nrtOutBufferUsed`; `mode` and `audioContextState` are strings here and
enum indexes in the array; `hasPlaybackStats` is a boolean here and 0 or 1
in the array; `ntpStartTime` is only here.

#### Properties

| Property                                                               | Type                                                                         | Description                                                                                               |
| ---------------------------------------------------------------------- | ---------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------- |
| <a id="audioblocksize"></a> `audioBlockSize`                           | `number`                                                                     | Audio block size in frames (128 on the web).                                                              |
| <a id="audiocontextstate"></a> `audioContextState`                     | `"running"` \| `"suspended"` \| `"closed"` \| `"interrupted"` \| `"unknown"` | AudioContext state; `'unknown'` when there is no context.                                                 |
| <a id="audiohealthpct"></a> `audioHealthPct`                           | `number`                                                                     | Audio health: fraction of expected audio frames delivered, 0–100 (every browser).                         |
| <a id="audioinputchannels"></a> `audioInputChannels`                   | `number`                                                                     | Number of input bus channels.                                                                             |
| <a id="audiooutputchannels"></a> `audioOutputChannels`                 | `number`                                                                     | Number of output bus channels.                                                                            |
| <a id="audiosamplerate"></a> `audioSampleRate`                         | `number`                                                                     | Output sample rate in Hz.                                                                                 |
| <a id="averagelatencyus"></a> `averageLatencyUs`                       | `number`                                                                     | Average audio output latency in microseconds (Chrome playbackStats; 0 elsewhere).                         |
| <a id="bufferpoolallocations"></a> `bufferPoolAllocations`             | `number`                                                                     | Buffers currently allocated.                                                                              |
| <a id="bufferpoolavailablebytes"></a> `bufferPoolAvailableBytes`       | `number`                                                                     | Sample buffer pool bytes free.                                                                            |
| <a id="bufferpoolgrowthcount"></a> `bufferPoolGrowthCount`             | `number`                                                                     | Times the pool has grown.                                                                                 |
| <a id="bufferpoolmaxcapacity"></a> `bufferPoolMaxCapacity`             | `number`                                                                     | Most the pool may grow to, in bytes.                                                                      |
| <a id="bufferpoolpoolcount"></a> `bufferPoolPoolCount`                 | `number`                                                                     | Pool segments; 1 means it has never grown.                                                                |
| <a id="bufferpooltotalcapacity"></a> `bufferPoolTotalCapacity`         | `number`                                                                     | Committed capacity across all pool segments, in bytes.                                                    |
| <a id="bufferpoolusedbytes"></a> `bufferPoolUsedBytes`                 | `number`                                                                     | Sample buffer pool bytes in use.                                                                          |
| <a id="clockbeatcenti"></a> `clockBeatCenti`                           | `number`                                                                     | Beat position \* 100. Divide by 100 for the beat.                                                         |
| <a id="clockoffsetms"></a> `clockOffsetMs`                             | `number`                                                                     | Clock offset for multi-system sync (ms, signed).                                                          |
| <a id="clockphasecenti"></a> `clockPhaseCenti`                         | `number`                                                                     | Phase within the quantum \* 100. Divide by 100 for the phase.                                             |
| <a id="clockplaying"></a> `clockPlaying`                               | `number`                                                                     | Transport playing (0 = stopped, 1 = playing).                                                             |
| <a id="clocktempombpm"></a> `clockTempoMbpm`                           | `number`                                                                     | Tempo in milli-BPM (bpm \* 1000). Divide by 1000 for BPM.                                                 |
| <a id="clockworkcommit"></a> `clockworkCommit`                         | `number`                                                                     | The Clockwork commit the engine was built from: its first 8 hex digits, read as a number; 0 when unknown. |
| <a id="debugbytesreceived"></a> `debugBytesReceived`                   | `number`                                                                     | Debug bytes received from the engine.                                                                     |
| <a id="debugmessagesreceived"></a> `debugMessagesReceived`             | `number`                                                                     | Debug messages received from the engine.                                                                  |
| <a id="driftoffsetms"></a> `driftOffsetMs`                             | `number`                                                                     | Clock drift between AudioContext and wall clock (ms, signed).                                             |
| <a id="enginemessagesdropped"></a> `engineMessagesDropped`             | `number`                                                                     | Messages dropped (ring buffer full).                                                                      |
| <a id="enginemessagesprocessed"></a> `engineMessagesProcessed`         | `number`                                                                     | Messages drained from the IN ring and dispatched.                                                         |
| <a id="engineprocesscount"></a> `engineProcessCount`                   | `number`                                                                     | Audio process() calls (cumulative).                                                                       |
| <a id="engineschedulercapacity"></a> `engineSchedulerCapacity?`        | `number`                                                                     | Maximum scheduler queue size.                                                                             |
| <a id="engineschedulerdepth"></a> `engineSchedulerDepth`               | `number`                                                                     | Current scheduler queue depth.                                                                            |
| <a id="engineschedulerdropped"></a> `engineSchedulerDropped`           | `number`                                                                     | Events dropped because the scheduler queue overflowed.                                                    |
| <a id="engineschedulerlastlatems"></a> `engineSchedulerLastLateMs`     | `number`                                                                     | Most recent late magnitude in the scheduler (ms).                                                         |
| <a id="engineschedulerlastlatetick"></a> `engineSchedulerLastLateTick` | `number`                                                                     | Process count when the last scheduler late occurred.                                                      |
| <a id="engineschedulerlates"></a> `engineSchedulerLates`               | `number`                                                                     | Bundles executed after their scheduled time.                                                              |
| <a id="engineschedulermaxlatems"></a> `engineSchedulerMaxLateMs`       | `number`                                                                     | Maximum lateness observed in the scheduler (ms).                                                          |
| <a id="engineschedulerpeakdepth"></a> `engineSchedulerPeakDepth`       | `number`                                                                     | Peak scheduler queue depth (high water mark).                                                             |
| <a id="enginesequencegaps"></a> `engineSequenceGaps`                   | `number`                                                                     | Messages lost in transit from JS to the engine.                                                           |
| <a id="enginewasmerrors"></a> `engineWasmErrors`                       | `number`                                                                     | WASM execution errors in the audio worklet.                                                               |
| <a id="glitchcount"></a> `glitchCount`                                 | `number`                                                                     | Audio underrun/glitch events (Chrome playbackStats; 0 elsewhere).                                         |
| <a id="glitchdurationms"></a> `glitchDurationMs`                       | `number`                                                                     | Total silence from audio underruns in ms (Chrome playbackStats; 0 elsewhere).                             |
| <a id="hasplaybackstats"></a> `hasPlaybackStats`                       | `boolean`                                                                    | Whether the Chrome playbackStats API is available.                                                        |
| <a id="inbufferused"></a> `inBufferUsed?`                              | [`RingBufferUsage`](#ringbufferusage)                                        | The IN ring (JS → engine).                                                                                |
| <a id="linkaudiobufferedms"></a> `linkAudioBufferedMs`                 | `number`                                                                     | Received Link Audio queued in the receiver (ms).                                                          |
| <a id="linkaudiodriftppm"></a> `linkAudioDriftPpm`                     | `number`                                                                     | Read-rate deviation from the sender's clock (parts per million, signed).                                  |
| <a id="linkaudioinchannels"></a> `linkAudioInChannels`                 | `number`                                                                     | Active received Link Audio channels.                                                                      |
| <a id="linkaudiopublish"></a> `linkAudioPublish`                       | `number`                                                                     | Link Audio publishing enabled (0 = off, 1 = on).                                                          |
| <a id="linkaudiosinks"></a> `linkAudioSinks`                           | `number`                                                                     | Active Link Audio output sinks.                                                                           |
| <a id="linkaudiostreamrate"></a> `linkAudioStreamRate`                 | `number`                                                                     | Received Link Audio stream sample rate in Hz.                                                             |
| <a id="linkaudiounderruns"></a> `linkAudioUnderruns`                   | `number`                                                                     | Receiver queue underruns — stream audio arrived too late to play.                                         |
| <a id="linkbeatcenti"></a> `linkBeatCenti`                             | `number`                                                                     | Current Link beat position \* 100.                                                                        |
| <a id="linkpeers"></a> `linkPeers`                                     | `number`                                                                     | Connected Ableton Link peers on the network.                                                              |
| <a id="linkphasecenti"></a> `linkPhaseCenti`                           | `number`                                                                     | Phase within the Link quantum \* 100.                                                                     |
| <a id="linkplaying"></a> `linkPlaying`                                 | `number`                                                                     | Link transport playing (0 = stopped, 1 = playing).                                                        |
| <a id="linktempombpm"></a> `linkTempoMbpm`                             | `number`                                                                     | Shared Link session tempo in milli-BPM (bpm \* 1000).                                                     |
| <a id="loadedsynthdefs-1"></a> `loadedSynthDefs`                       | `number`                                                                     | Synthdefs this client holds, and will restore after a reload.                                             |
| <a id="maxlatencyus"></a> `maxLatencyUs`                               | `number`                                                                     | Maximum audio output latency in microseconds (Chrome playbackStats; 0 elsewhere).                         |
| <a id="mode-4"></a> `mode`                                             | [`TransportMode`](#transportmode)                                            | Transport mode.                                                                                           |
| <a id="nrtoutbufferused"></a> `nrtOutBufferUsed?`                      | [`RingBufferUsage`](#ringbufferusage)                                        | The NRT-out ring (replies, notifications, debug).                                                         |
| <a id="ntpstarttime"></a> `ntpStartTime`                               | `number`                                                                     | NTP time (seconds since 1900) when the AudioContext started; 0 before boot.                               |
| <a id="oscinbytesreceived"></a> `oscInBytesReceived`                   | `number`                                                                     | Total bytes received from the engine.                                                                     |
| <a id="oscincorrupted"></a> `oscInCorrupted`                           | `number`                                                                     | Corrupted messages detected in the ring buffer.                                                           |
| <a id="oscinmessagesdropped"></a> `oscInMessagesDropped`               | `number`                                                                     | Replies lost in transit from the engine to JS.                                                            |
| <a id="oscinmessagesreceived"></a> `oscInMessagesReceived`             | `number`                                                                     | OSC replies received from the engine.                                                                     |
| <a id="oscoutbytessent"></a> `oscOutBytesSent`                         | `number`                                                                     | Total bytes sent from JS to the engine.                                                                   |
| <a id="oscoutmessagessent"></a> `oscOutMessagesSent`                   | `number`                                                                     | OSC messages sent from JS to the engine.                                                                  |
| <a id="outbufferused"></a> `outBufferUsed?`                            | [`RingBufferUsage`](#ringbufferusage)                                        | The OUT ring (engine replies → JS).                                                                       |
| <a id="ringbufferdirectwritefails"></a> `ringBufferDirectWriteFails`   | `number`                                                                     | SAB mode only: IN-ring writes dropped because the ring had no room.                                       |
| <a id="totalframesdurationms"></a> `totalFramesDurationMs`             | `number`                                                                     | Total audio rendered duration in ms (Chrome playbackStats; 0 elsewhere).                                  |

### SystemReport

System performance report returned by [SuperSonic.getSystemReport](#getsystemreport).

Includes hardware info, audio configuration, Chrome playbackStats (if available),
a cross-browser audio health percentage, and a human-readable health assessment.
Useful for diagnosing audio crackling on constrained hardware.

#### Properties

| Property                                   | Type                                      | Description                                               |
| ------------------------------------------ | ----------------------------------------- | --------------------------------------------------------- |
| <a id="audio"></a> `audio`                 | `object`                                  | AudioContext configuration and state.                     |
| `audio.baseLatency`                        | `number`                                  | -                                                         |
| `audio.channelCount`                       | `number`                                  | -                                                         |
| `audio.outputLatency`                      | `number`                                  | -                                                         |
| `audio.sampleRate`                         | `number`                                  | -                                                         |
| `audio.state`                              | `string`                                  | -                                                         |
| <a id="engine"></a> `engine`               | `object`                                  | Engine configuration.                                     |
| `engine.bootTimeMs`                        | `number`                                  | -                                                         |
| `engine.mode`                              | [`TransportMode`](#transportmode)         | -                                                         |
| `engine.version`                           | `string`                                  | -                                                         |
| <a id="health"></a> `health`               | `object`                                  | Health assessment with issues and human-readable summary. |
| `health.audioHealthPct`                    | `number`                                  | -                                                         |
| `health.issues`                            | `object`\[]                               | -                                                         |
| `health.summary`                           | `string`                                  | -                                                         |
| <a id="metrics-2"></a> `metrics`           | [`SuperSonicMetrics`](#supersonicmetrics) | Full metrics snapshot at time of report.                  |
| <a id="playbackstats"></a> `playbackStats` | `object`                                  | Chrome playbackStats (null on browsers without support).  |
| `playbackStats.averageLatencyS`            | `number`                                  | -                                                         |
| `playbackStats.glitchCount`                | `number`                                  | -                                                         |
| `playbackStats.glitchDurationS`            | `number`                                  | -                                                         |
| `playbackStats.maximumLatencyS`            | `number`                                  | -                                                         |
| `playbackStats.totalDurationS`             | `number`                                  | -                                                         |
| <a id="system"></a> `system`               | `object`                                  | Hardware and browser info.                                |
| `system.deviceMemory`                      | `number`                                  | -                                                         |
| `system.hardwareConcurrency`               | `number`                                  | -                                                         |
| `system.platform`                          | `string`                                  | -                                                         |
| `system.userAgent`                         | `string`                                  | -                                                         |
| <a id="timestamp-1"></a> `timestamp`       | `string`                                  | ISO 8601 timestamp when the report was generated.         |

***

### Tree

Hierarchical node tree returned by [SuperSonic.getTree](#gettree).

#### Example

```ts
const tree = sonic.getTree();
console.log(tree.root?.children); // top-level groups and synths
console.log(tree.nodeCount);      // total nodes in the tree
```

***

#### Properties

| Property                                   | Type                    | Description                                                          |
| ------------------------------------------ | ----------------------- | -------------------------------------------------------------------- |
| <a id="droppedcount-1"></a> `droppedCount` | `number`                | Nodes that exceeded mirror capacity (tree may be incomplete if > 0). |
| <a id="nodecount-1"></a> `nodeCount`       | `number`                | Total number of nodes.                                               |
| <a id="root"></a> `root`                   | [`TreeNode`](#treenode) | The root group, or null before the engine has published a tree.      |
| <a id="version-2"></a> `version`           | `number`                | Increments on any tree change — useful for detecting updates.        |

***

### TreeNode

A node in the hierarchical synth tree.

Groups contain children; synths are leaves.

#### Properties

| Property                         | Type                          | Description                                                |
| -------------------------------- | ----------------------------- | ---------------------------------------------------------- |
| <a id="children"></a> `children` | [`TreeNode`](#treenode)\[]    | Child nodes (groups only, empty array for synths).         |
| <a id="defname-1"></a> `defName` | `string`                      | SynthDef name (synths only, empty string for groups).      |
| <a id="id-1"></a> `id`           | `number` \| [`UUID`](#uuid-1) | The node's UUID when it has one, otherwise its numeric ID. |
| <a id="type-2"></a> `type`       | `"group"` \| `"synth"`        | `'group'` for groups, `'synth'` for synth nodes.           |

## Type Aliases

### AddAction

> **AddAction** = `0` | `1` | `2` | `3` | `4`

Node add action: 0=head, 1=tail, 2=before, 3=after, 4=replace

***

### EngineState

> **EngineState** = `"stopped"` | `"booting"` | `"running"` | `"restarting"` | `"error"`

The engine's lifecycle state: the same words as native's `engineState()`.

***

### NodeID

> **NodeID** = `number`

A node identifier (int32).

***

### NTPTimeTag

> **NTPTimeTag** = `number` | \[`number`, `number`] | `1` | `null` | `undefined`

NTP timetag for bundle encoding.

* `1` or `null` or `undefined` → immediate execution
* `number` → NTP seconds since 1900
* `[seconds, fraction]` → raw NTP pair (both uint32)

### OscBundlePacket

> **OscBundlePacket** = [`OscMessage`](#oscmessage) | { `packets`: [`OscBundlePacket`](#oscbundlepacket)\[]; `timeTag`: [`NTPTimeTag`](#ntptimetag); }

A packet that can be included in an OSC bundle.

A message as an array, or a nested bundle:

#### Example

```ts
// Message:
["/s_new", "beep", 1001, 0, 0]

// Nested bundle:
{ timeTag: ntpTime, packets: [ ["/n_set", 1001, "freq", 880] ] }
```

***

### OscChannelTransferable

> **OscChannelTransferable** = [`OscChannelSABTransferable`](#oscchannelsabtransferable) | [`OscChannelPMTransferable`](#oscchannelpmtransferable)

Opaque config produced by `channel.transferable` and consumed by `OscChannel.fromTransferable()`.

***

### OscMessage

> **OscMessage** = \[`string`, `...OscArg[]`]

Decoded OSC message as a plain array.

The first element is always the address string, followed by zero or more arguments.

#### Example

```ts
// A decoded /n_go message received from the engine:
["/n_go", 1001, 0, -1, -1, 0]

// Access parts:
const address = msg[0];  // "/n_go"
const args = msg.slice(1);  // [1001, 0, -1, -1, 0]
```

***

### SuperSonicEvent

> **SuperSonicEvent** = keyof [`SuperSonicEventMap`](#event-types)

Union of all event names.

***

### TransportMode

> **TransportMode** = `"sab"` | `"postMessage"`

Transport mode for communication between JS and the AudioWorklet.

* `'sab'` — SharedArrayBuffer: lowest latency, requires COOP/COEP headers
* `'postMessage'` — postMessage: works everywhere including CDN, slightly higher latency

***

### UUID

> **UUID** = `Uint8Array`

A v7 UUID as 16 raw bytes.

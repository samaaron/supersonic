# API Reference

> Auto-generated from [`supersonic.d.ts`](../supersonic.d.ts). For worked examples and patterns, see the [Guide](GUIDE.md).

* [SuperSonic](#supersonic) — [Constructor Options](#constructor-options) · [Server Options](#server-options) · [Properties](#properties) · [Accessors](#accessors) · [Methods](#methods) · [Event Types](#event-types) · [OSC Argument Types](#osc-argument-types)

* [OscChannel](#oscchannel) — [Accessors](#accessors-1) · [Methods](#methods-1)

* [osc](#osc)

* **Interfaces** — [ActivityLineConfig](#activitylineconfig) · [BootStats](#bootstats) · [LoadedBufferInfo](#loadedbufferinfo) · [LoadSampleResult](#loadsampleresult) · [LoadSynthDefResult](#loadsynthdefresult) · [MetricDefinition](#metricdefinition) · [MetricsSchema](#metricsschema) · [OscBundle](#oscbundle) · [OscChannelMetrics](#oscchannelmetrics) · [OscChannelPMTransferable](#oscchannelpmtransferable) · [OscChannelSABTransferable](#oscchannelsabtransferable) · [RawTree](#rawtree) · [RawTreeNode](#rawtreenode) · [SampleInfo](#sampleinfo-1) · [SendOSCOptions](#sendoscoptions) · [Snapshot](#snapshot) · [SuperSonicInfo](#supersonicinfo) · [SuperSonicMetrics](#supersonicmetrics) · [Tree](#tree) · [TreeNode](#treenode)

* **Type Aliases** — [AddAction](#addaction) · [BlockedCommand](#blockedcommand) · [NodeID](#nodeid) · [NTPTimeTag](#ntptimetag) · [OscBundlePacket](#oscbundlepacket) · [OscCategory](#osccategory) · [OscChannelTransferable](#oscchanneltransferable) · [OscMessage](#oscmessage) · [SuperSonicEvent](#supersonicevent) · [TransportMode](#transportmode) · [UUID](#uuid)

## Classes

### SuperSonic

SuperSonic — WebAssembly SuperCollider synthesis engine for the browser.

Coordinates WASM, AudioWorklet, SharedArrayBuffer, and IO Workers to run
scsynth with low latency inside a web page.

**Core**

| Member                                    | Description                                                                                                                 |
| ----------------------------------------- | --------------------------------------------------------------------------------------------------------------------------- |
| [`init()`](#init)                         | Initialise the engine.                                                                                                      |
| [`shutdown()`](#shutdown)                 | Shut down the engine.                                                                                                       |
| [`destroy()`](#destroy)                   | Destroy the engine completely.                                                                                              |
| [`recover()`](#recover)                   | Smart recovery — tries a quick resume first, falls back to full reload.                                                     |
| [`suspend()`](#suspend)                   | Suspend the AudioContext and stop the drift timer.                                                                          |
| [`resume()`](#resume)                     | Quick resume — calls purge to flush stale messages, resumes AudioContext, and resyncs timing.                               |
| [`reload()`](#reload)                     | Full reload — destroys and recreates the worklet and WASM, then restores all previously loaded synthdefs and audio buffers. |
| [`reset()`](#reset)                       | Shutdown and immediately re-initialise.                                                                                     |
| [`send()`](#send)                         | Send any OSC message.                                                                                                       |
| [`sendOSC()`](#sendosc)                   | Send pre-encoded OSC bytes to scsynth.                                                                                      |
| [`sync()`](#sync)                         | Wait for scsynth to process all pending commands.                                                                           |
| [`purge()`](#purge)                       | Cancel all pending scheduled messages everywhere in the pipeline.                                                           |
| [`cancelAll()`](#cancelall)               | Cancel all scheduled messages in the JS prescheduler.                                                                       |
| [`cancelSession()`](#cancelsession)       | Cancel all scheduled messages for a session.                                                                                |
| [`cancelSessionTag()`](#cancelsessiontag) | Cancel scheduled messages matching both a session and run tag.                                                              |
| [`cancelTag()`](#canceltag)               | Cancel all scheduled messages with the given run tag.                                                                       |

**Asset Loading**

| Member                              | Description                                                               |
| ----------------------------------- | ------------------------------------------------------------------------- |
| [`loadSynthDef()`](#loadsynthdef)   | Load a SynthDef into scsynth.                                             |
| [`loadSynthDefs()`](#loadsynthdefs) | Load multiple SynthDefs by name in parallel.                              |
| [`loadSample()`](#loadsample)       | Load an audio sample into a scsynth buffer slot.                          |
| [`sampleInfo()`](#sampleinfo)       | Get sample metadata (including content hash) without allocating a buffer. |

**Events**

| Member                                        | Description                                                   |
| --------------------------------------------- | ------------------------------------------------------------- |
| [`on()`](#on)                                 | Subscribe to an event.                                        |
| [`off()`](#off)                               | Unsubscribe from an event.                                    |
| [`once()`](#once)                             | Subscribe to an event once.                                   |
| [`removeAllListeners()`](#removealllisteners) | Remove all listeners for an event, or all listeners entirely. |

**Node Tree**

| Member                          | Description                                                         |
| ------------------------------- | ------------------------------------------------------------------- |
| [`getTree()`](#gettree)         | Get the node tree in hierarchical format.                           |
| [`getRawTree()`](#getrawtree)   | Get the node tree in flat format with linkage pointers.             |
| [`getSnapshot()`](#getsnapshot) | Get a diagnostic snapshot with metrics, node tree, and memory info. |

**Metrics**

| Member                                    | Description                                                    |
| ----------------------------------------- | -------------------------------------------------------------- |
| [`getMetrics()`](#getmetrics)             | Get current metrics as a named object.                         |
| [`getMetricsArray()`](#getmetricsarray)   | Get metrics as a flat Uint32Array for zero-allocation reading. |
| [`getMetricsSchema()`](#getmetricsschema) | Get the metrics schema describing all available metrics.       |

**Properties**

| Member                          | Description                                        |
| ------------------------------- | -------------------------------------------------- |
| [`initialized`](#initialized)   | Whether the engine has completed initialisation.   |
| [`initializing`](#initializing) | Whether init is currently in progress.             |
| [`audioContext`](#audiocontext) | The underlying AudioContext.                       |
| [`node`](#node)                 | AudioWorkletNode wrapper for custom audio routing. |

**Advanced**

| Member                                              | Description                                                             |
| --------------------------------------------------- | ----------------------------------------------------------------------- |
| [`getInfo()`](#getinfo)                             | Get engine info: sample rate, memory layout, capabilities, and version. |
| [`createOscChannel()`](#createoscchannel)           | Create an OscChannel for direct worker-to-worklet communication.        |
| [`startCapture()`](#startcapture)                   | Start capturing audio output to a buffer.                               |
| [`stopCapture()`](#stopcapture)                     | Stop capturing and return the captured audio data.                      |
| [`getCaptureFrames()`](#getcaptureframes)           | Get number of audio frames captured so far.                             |
| [`isCaptureEnabled()`](#iscaptureenabled)           | Check if audio capture is currently enabled.                            |
| [`getMaxCaptureDuration()`](#getmaxcaptureduration) | Get maximum capture duration in seconds.                                |
| [`setClockOffset()`](#setclockoffset)               | Set clock offset for multi-system sync (e.g. Ableton Link, NTP server). |

**Advanced**

| Member                                    | Description                                                  |
| ----------------------------------------- | ------------------------------------------------------------ |
| [`bufferConstants`](#bufferconstants)     | Buffer layout constants from the WASM build.                 |
| [`initTime`](#inittime)                   | NTP time (seconds since 1900) when the AudioContext started. |
| [`mode`](#mode)                           | Active transport mode ('sab' or 'postMessage').              |
| [`ringBufferBase`](#ringbufferbase)       | Ring buffer base offset in SharedArrayBuffer.                |
| [`sharedBuffer`](#sharedbuffer)           | The SharedArrayBuffer (SAB mode) or null (postMessage mode). |
| [`getLoadedBuffers()`](#getloadedbuffers) | Get info about all loaded audio buffers.                     |
| [`nextNodeId()`](#nextnodeid)             | Get the next unique node ID.                                 |
| [`getRawTreeSchema()`](#getrawtreeschema) | Get schema describing the raw flat node tree structure.      |
| [`getTreeSchema()`](#gettreeschema)       | Get schema describing the hierarchical node tree structure.  |

#### Examples

```ts
// CDN Quick Start
import { SuperSonic } from 'supersonic-scsynth';

const sonic = new SuperSonic({
  baseURL: 'https://unpkg.com/supersonic-scsynth@latest/dist/',
  synthdefBaseURL: 'https://unpkg.com/supersonic-scsynth-synthdefs@latest/synthdefs/',
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

sonic.on('message', (msg) => {
  console.log('OSC from scsynth:', msg[0], msg.slice(1));
});

await sonic.init();
sonic.send('/s_new', 'beep', 1001, 0, 1, 'freq', 440);
```

***

#### Constructors

##### Constructor

> **new SuperSonic**(`options?`): [`SuperSonic`](#supersonic)

Create a new SuperSonic instance.

Does not start the engine — call [init](#init) to boot.

###### Parameters

| Parameter  | Type                                      | Description                                                                                        |
| ---------- | ----------------------------------------- | -------------------------------------------------------------------------------------------------- |
| `options?` | [`SuperSonicOptions`](#supersonicoptions) | Configuration options. Requires `baseURL` or both `coreBaseURL`/`workerBaseURL` and `wasmBaseURL`. |

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

| Property                                                    | Type                                        | Description                                                                                                                                                                                                                                                        | Required |
| ----------------------------------------------------------- | ------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ | -------- |
| <a id="activityconsolelog"></a> `activityConsoleLog?`       | [`ActivityLineConfig`](#activitylineconfig) | Line length limits for activity console.log output.                                                                                                                                                                                                                |          |
| <a id="activityevent"></a> `activityEvent?`                 | [`ActivityLineConfig`](#activitylineconfig) | Line length limits for activity events emitted to listeners.                                                                                                                                                                                                       |          |
| <a id="audiocontext-1"></a> `audioContext?`                 | `AudioContext`                              | Provide your own AudioContext instead of letting SuperSonic create one.                                                                                                                                                                                            |          |
| <a id="audiocontextoptions"></a> `audioContextOptions?`     | `AudioContextOptions`                       | Options passed to `new AudioContext()`. Ignored if `audioContext` is provided.                                                                                                                                                                                     |          |
| <a id="autoconnect"></a> `autoConnect?`                     | `boolean`                                   | Auto-connect the AudioWorkletNode to the AudioContext destination. Default: true.                                                                                                                                                                                  |          |
| <a id="baseurl"></a> `baseURL?`                             | `string`                                    | Convenience shorthand when all assets (WASM, workers, synthdefs, samples) are co-located.                                                                                                                                                                          | Yes\*    |
| <a id="bypasslookaheadms"></a> `bypassLookaheadMs?`         | `number`                                    | Bundles scheduled within this many ms of now are dispatched immediately for lowest latency. Bundles further in the future are held and dispatched closer to their scheduled time. Default: 500.                                                                    |          |
| <a id="corebaseurl"></a> `coreBaseURL?`                     | `string`                                    | Base URL for GPL assets: WASM and AudioWorklet (supersonic-scsynth-core package). Defaults to `baseURL`.                                                                                                                                                           |          |
| <a id="debug-1"></a> `debug?`                               | `boolean`                                   | Enable all debug console logging. Default: false.                                                                                                                                                                                                                  |          |
| <a id="debugoscin"></a> `debugOscIn?`                       | `boolean`                                   | Log incoming OSC messages to console. Default: false.                                                                                                                                                                                                              |          |
| <a id="debugoscout"></a> `debugOscOut?`                     | `boolean`                                   | Log outgoing OSC messages to console. Default: false.                                                                                                                                                                                                              |          |
| <a id="debugscsynth"></a> `debugScsynth?`                   | `boolean`                                   | Log scsynth debug output to console. Default: false.                                                                                                                                                                                                               |          |
| <a id="fetchmaxretries"></a> `fetchMaxRetries?`             | `number`                                    | Max fetch retries when loading assets. Default: 3.                                                                                                                                                                                                                 |          |
| <a id="fetchretrydelay"></a> `fetchRetryDelay?`             | `number`                                    | Base delay between retries in ms (exponential backoff). Default: 1000.                                                                                                                                                                                             |          |
| <a id="mode-5"></a> `mode?`                                 | [`TransportMode`](#transportmode)           | Transport mode. - `'postMessage'` (default) — works everywhere, no special headers needed - `'sab'` — lowest latency, requires Cross-Origin-Opener-Policy and Cross-Origin-Embedder-Policy headers See docs/MODES.md for a full comparison of communication modes. |          |
| <a id="preschedulercapacity-1"></a> `preschedulerCapacity?` | `number`                                    | Max pending events in the JS prescheduler. Default: 65536.                                                                                                                                                                                                         |          |
| <a id="samplebaseurl"></a> `sampleBaseURL?`                 | `string`                                    | Base URL for audio sample files (used by [SuperSonic.loadSample](#loadsample)).                                                                                                                                                                                    |          |
| <a id="scsynthoptions-1"></a> `scsynthOptions?`             | [`ScsynthOptions`](#scsynthoptions)         | Engine options passed to scsynth World\_New().                                                                                                                                                                                                                     |          |
| <a id="snapshotintervalms"></a> `snapshotIntervalMs?`       | `number`                                    | How often to snapshot metrics/tree in postMessage mode (ms).                                                                                                                                                                                                       |          |
| <a id="synthdefbaseurl"></a> `synthdefBaseURL?`             | `string`                                    | Base URL for synthdef files (used by [SuperSonic.loadSynthDef](#loadsynthdef)).                                                                                                                                                                                    |          |
| <a id="wasmbaseurl"></a> `wasmBaseURL?`                     | `string`                                    | Base URL for WASM files. Defaults to `coreBaseURL + 'wasm/'`.                                                                                                                                                                                                      |          |
| <a id="wasmurl"></a> `wasmUrl?`                             | `string`                                    | Full URL to the WASM binary. Overrides wasmBaseURL.                                                                                                                                                                                                                |          |
| <a id="workerbaseurl"></a> `workerBaseURL?`                 | `string`                                    | Base URL for MIT worker scripts. Defaults to `baseURL + 'workers/'`.                                                                                                                                                                                               |          |
| <a id="workleturl"></a> `workletUrl?`                       | `string`                                    | Full URL to the AudioWorklet script. Overrides `coreBaseURL`.                                                                                                                                                                                                      |          |

*Required unless both `coreBaseURL`/`workerBaseURL` and `wasmBaseURL` are provided.*

***

#### Server Options

| Property                                                    | Type       | Description                                                                    | Default | Range          |
| ----------------------------------------------------------- | ---------- | ------------------------------------------------------------------------------ | ------- | -------------- |
| <a id="buflength"></a> `bufLength?`                         | `128`      | Audio buffer length — must be 128 (WebAudio API constraint).                   | 128     | 128 (fixed)    |
| <a id="loadgraphdefs"></a> `loadGraphDefs?`                 | `0` \| `1` | Auto-load synthdefs from disk: 0 or 1. Default: 0.                             | 0       | 0–1            |
| <a id="maxgraphdefs"></a> `maxGraphDefs?`                   | `number`   | Max synth definitions. Default: 1024.                                          | 1024    | 1+             |
| <a id="maxnodes"></a> `maxNodes?`                           | `number`   | Max synthesis nodes — synths + groups. Default: 1024.                          | 1024    | 1+             |
| <a id="maxwirebufs"></a> `maxWireBufs?`                     | `number`   | Max wire buffers for internal UGen routing. Default: 64.                       | 64      | 1+             |
| <a id="memorylocking"></a> `memoryLocking?`                 | `boolean`  | Memory locking — not applicable in browser. Default: false.                    | false   | —              |
| <a id="numaudiobuschannels"></a> `numAudioBusChannels?`     | `number`   | Audio bus channels for routing between synths. Default: 128.                   | 128     | 1+             |
| <a id="numbuffers"></a> `numBuffers?`                       | `number`   | Max audio buffers (1–65535). Default: 1024.                                    | 1024    | 1–65535        |
| <a id="numcontrolbuschannels"></a> `numControlBusChannels?` | `number`   | Control bus channels for control-rate data. Default: 4096.                     | 4096    | 1+             |
| <a id="numinputbuschannels"></a> `numInputBusChannels?`     | `number`   | Hardware input channels. Default: 2 (stereo).                                  | 2       | 0+             |
| <a id="numoutputbuschannels"></a> `numOutputBusChannels?`   | `number`   | Hardware output channels (1–128). Default: 2 (stereo).                         | 2       | 1–128          |
| <a id="numrgens"></a> `numRGens?`                           | `number`   | Random number generators per synth. Default: 64.                               | 64      | 1+             |
| <a id="preferredsamplerate"></a> `preferredSampleRate?`     | `number`   | Preferred sample rate. 0 = use AudioContext default (typically 48000).         | 0       | 0, 8000–384000 |
| <a id="realtime"></a> `realTime?`                           | `boolean`  | Clock source. Always false in SuperSonic (externally clocked by AudioWorklet). | false   | —              |
| <a id="realtimememorysize"></a> `realTimeMemorySize?`       | `number`   | Real-time memory pool in KB for synthesis allocations. Default: 8192 (8MB).    | 8192    | 1+             |
| <a id="verbosity"></a> `verbosity?`                         | `number`   | Debug verbosity: 0 = quiet, 1 = errors, 2 = warnings, 3 = info, 4 = debug.     | 0       | 0–4            |

***

#### Properties

| Property                                       | Modifier | Type                                                     | Description                                                                                                                                                                                       |
| ---------------------------------------------- | -------- | -------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| <a id="bootstats"></a> `bootStats`             | `public` | [`BootStats`](#bootstats-1)                              | Boot timing statistics.                                                                                                                                                                           |
| <a id="loadedsynthdefs"></a> `loadedSynthDefs` | `public` | `Map`<`string`, `Uint8Array`<`ArrayBufferLike`>>         | Map of loaded SynthDef names to their binary data. SynthDefs appear after `/d_recv` or `loadSynthDef()`. Removed on `/d_free` or `/d_freeAll`. Cached for automatic restoration after `reload()`. |
| <a id="osc"></a> `osc`                         | `static` | `object`                                                 | OSC encoding/decoding utilities. **Example** `const msg = SuperSonic.osc.encodeMessage('/s_new', ['beep', 1001, 0, 0]); const decoded = SuperSonic.osc.decode(msg);`                              |
| `osc.NTP_EPOCH_OFFSET`                         | `public` | `number`                                                 | Seconds between NTP epoch (1900) and Unix epoch (1970): `2208988800`.                                                                                                                             |
| `osc.decode`                                   | `public` | [`OscBundle`](#oscbundle) \| [`OscMessage`](#oscmessage) | -                                                                                                                                                                                                 |
| `osc.encodeBundle`                             | `public` | `Uint8Array`                                             | -                                                                                                                                                                                                 |
| `osc.encodeMessage`                            | `public` | `Uint8Array`                                             | -                                                                                                                                                                                                 |
| `osc.encodeSingleBundle`                       | `public` | `Uint8Array`                                             | -                                                                                                                                                                                                 |
| `osc.ntpNow`                                   | `public` | `number`                                                 | -                                                                                                                                                                                                 |
| `osc.readTimetag`                              | `public` | `object`                                                 | -                                                                                                                                                                                                 |

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

##### initTime

###### Get Signature

> **get** **initTime**(): `number`

NTP time (seconds since 1900) when the AudioContext started. Use to compute relative times: `event.timestamp - sonic.initTime`.

###### Returns

`number`

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
Use `node.input` to connect external audio sources into scsynth.

###### Example

```ts
// Route scsynth output through an AnalyserNode:
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

##### cancelAll()

> **cancelAll**(): `void`

Cancel all scheduled messages in the JS prescheduler.

###### Returns

`void`

##### cancelSession()

> **cancelSession**(`sessionId`): `void`

Cancel all scheduled messages for a session.

###### Parameters

| Parameter   | Type     | Description       |
| ----------- | -------- | ----------------- |
| `sessionId` | `string` | Session to cancel |

###### Returns

`void`

##### cancelSessionTag()

> **cancelSessionTag**(`sessionId`, `runTag`): `void`

Cancel scheduled messages matching both a session and run tag.

###### Parameters

| Parameter   | Type     | Description                      |
| ----------- | -------- | -------------------------------- |
| `sessionId` | `string` | Session to match                 |
| `runTag`    | `string` | Tag to match within that session |

###### Returns

`void`

##### cancelTag()

> **cancelTag**(`runTag`): `void`

Cancel all scheduled messages with the given run tag.
Only affects messages in the JS prescheduler (not yet dispatched to WASM).

###### Parameters

| Parameter | Type     | Description   |
| --------- | -------- | ------------- |
| `runTag`  | `string` | Tag to cancel |

###### Returns

`void`

##### createOscChannel()

> **createOscChannel**(`options?`): [`OscChannel`](#oscchannel)

Create an OscChannel for direct worker-to-worklet communication.

The returned channel can be transferred to a Web Worker, allowing that
worker to send OSC directly to the AudioWorklet without going through
the main thread. Works in both SAB and postMessage modes.

The `blocking` option defaults to `true` for worker channels (sourceId !== 0)
and `false` for main thread. Set to `false` for AudioWorkletProcessor use.
In postMessage mode this has no effect.

For AudioWorkletProcessor use, import from `'supersonic-scsynth/osc-channel'`
which avoids DOM APIs unavailable in the worklet scope.

See docs/WORKERS.md for the full workers guide.

###### Parameters

| Parameter           | Type                                               | Description                                             |
| ------------------- | -------------------------------------------------- | ------------------------------------------------------- |
| `options?`          | { `blocking?`: `boolean`; `sourceId?`: `number`; } | Channel options                                         |
| `options.blocking?` | `boolean`                                          | Whether sends block until the worklet reads the message |
| `options.sourceId?` | `number`                                           | Numeric source ID (0 = main thread, 1+ = workers)       |

###### Returns

[`OscChannel`](#oscchannel)

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

Calls [shutdown](#shutdown) then clears the WASM cache and all event listeners.
Emits `'destroy'`.

###### Returns

`Promise`<`void`>

##### getCaptureFrames()

> **getCaptureFrames**(): `number`

Get number of audio frames captured so far.

###### Returns

`number`

##### getInfo()

> **getInfo**(): [`SuperSonicInfo`](#supersonicinfo)

Get engine info: sample rate, memory layout, capabilities, and version.

###### Returns

[`SuperSonicInfo`](#supersonicinfo)

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

Get maximum capture duration in seconds.

###### Returns

`number`

##### getMetrics()

> **getMetrics**(): [`SuperSonicMetrics`](#supersonicmetrics)

Get current metrics as a named object.

This is a cheap local memory read in both SAB and postMessage modes — no IPC
or copying. Safe to call from `requestAnimationFrame`.

See docs/METRICS.md for the full metrics guide.

###### Returns

[`SuperSonicMetrics`](#supersonicmetrics)

###### Example

```ts
const m = sonic.getMetrics();
console.log(`Messages sent: ${m.oscOutMessagesSent}`);
console.log(`Scheduler depth: ${m.scsynthSchedulerDepth}`);
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

##### getSnapshot()

> **getSnapshot**(): [`Snapshot`](#snapshot)

Get a diagnostic snapshot with metrics, node tree, and memory info.

Useful for capturing state for bug reports or debugging timing issues.

###### Returns

[`Snapshot`](#snapshot)

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
printTree(tree.root);
```

##### init()

> **init**(): `Promise`<`void`>

Initialise the engine.

Loads the WASM binary, creates the AudioContext and AudioWorklet,
starts IO workers, and syncs timing. Emits `'setup'` then `'ready'`
when complete.

Safe to call multiple times — subsequent calls are no-ops.
Must be called from a user gesture (click/tap) due to browser autoplay policies.

###### Returns

`Promise`<`void`>

###### Throws

If required browser features are missing or WASM fails to load.

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

##### loadSample()

> **loadSample**(`bufnum`, `source`, `startFrame?`, `numFrames?`): `Promise`<[`LoadSampleResult`](#loadsampleresult)>

Load an audio sample into a scsynth buffer slot.

Decodes the audio file (WAV, AIFF, etc.) and copies the samples into
the WASM buffer pool. The buffer is then available for use with `PlayBuf`,
`BufRd`, etc.

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
await sonic.send('/s_new', 'sampler', 1001, 0, 1, 'bufnum', 0);
```

##### loadSynthDef()

> **loadSynthDef**(`source`): `Promise`<[`LoadSynthDefResult`](#loadsynthdefresult)>

Load a SynthDef into scsynth.

Accepts multiple source types:

* **Name string** — fetched from `synthdefBaseURL` (e.g. `'beep'` → `synthdefBaseURL/beep.scsyndef`)
* **Path/URL string** — fetched directly (must contain `/` or `://`)
* **ArrayBuffer / Uint8Array** — raw synthdef bytes
* **File / Blob** — e.g. from a file input

###### Parameters

| Parameter | Type                                                                        | Description                                      |
| --------- | --------------------------------------------------------------------------- | ------------------------------------------------ |
| `source`  | `string` \| `ArrayBuffer` \| `ArrayBufferView`<`ArrayBufferLike`> \| `Blob` | SynthDef name, path/URL, raw bytes, or File/Blob |

###### Returns

`Promise`<[`LoadSynthDefResult`](#loadsynthdefresult)>

The extracted name and byte size

###### Throws

If the source type is invalid or the synthdef can't be parsed

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

> **loadSynthDefs**(`names`): `Promise`<`Record`<`string`, { `error?`: `string`; `success`: `boolean`; }>>

Load multiple SynthDefs by name in parallel.

###### Parameters

| Parameter | Type        | Description             |
| --------- | ----------- | ----------------------- |
| `names`   | `string`\[] | Array of synthdef names |

###### Returns

`Promise`<`Record`<`string`, { `error?`: `string`; `success`: `boolean`; }>>

Object mapping each name to `{ success: true }` or `{ success: false, error: string }`

###### Example

```ts
const results = await sonic.loadSynthDefs(['beep', 'pad', 'kick']);
if (!results.kick.success) console.error(results.kick.error);
```

##### nextNodeId()

> **nextNodeId**(): `number`

Get the next unique node ID.

Thread-safe — can be called concurrently from multiple workers and no
two callers will ever receive the same ID. IDs start at 1000 (0 is
the root group, 1 is the default group, 2–999 are reserved for manual use).

Also available on [OscChannel](#oscchannel) for use in Web Workers.

###### Returns

`number`

A unique node ID (>= 1000)

###### Example

```ts
const id = sonic.nextNodeId();
sonic.send('/s_new', 'beep', id, 0, 1, 'freq', 440);
```

##### off()

> **off**<`E`>(`event`, `callback`): `this`

Unsubscribe from an event.

###### Type Parameters

| Type Parameter                                                  |
| --------------------------------------------------------------- |
| `E` *extends* keyof [`SuperSonicEventMap`](#supersoniceventmap) |

###### Parameters

| Parameter  | Type                                              | Description                                     |
| ---------- | ------------------------------------------------- | ----------------------------------------------- |
| `event`    | `E`                                               | Event name                                      |
| `callback` | [`SuperSonicEventMap`](#supersoniceventmap)\[`E`] | The same function reference passed to [on](#on) |

###### Returns

`this`

##### on()

> **on**<`E`>(`event`, `callback`): () => `void`

Subscribe to an event.

###### Type Parameters

| Type Parameter                                                  |
| --------------------------------------------------------------- |
| `E` *extends* keyof [`SuperSonicEventMap`](#supersoniceventmap) |

###### Parameters

| Parameter  | Type                                              | Description                               |
| ---------- | ------------------------------------------------- | ----------------------------------------- |
| `event`    | `E`                                               | Event name                                |
| `callback` | [`SuperSonicEventMap`](#supersoniceventmap)\[`E`] | Handler function (type-checked per event) |

###### Returns

Unsubscribe function — call it to remove the listener

> (): `void`

###### Returns

`void`

###### Example

```ts
const unsub = sonic.on('message', (msg) => {
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

| Type Parameter                                                  |
| --------------------------------------------------------------- |
| `E` *extends* keyof [`SuperSonicEventMap`](#supersoniceventmap) |

###### Parameters

| Parameter  | Type                                              | Description      |
| ---------- | ------------------------------------------------- | ---------------- |
| `event`    | `E`                                               | Event name       |
| `callback` | [`SuperSonicEventMap`](#supersoniceventmap)\[`E`] | Handler function |

###### Returns

Unsubscribe function — call it to remove the listener before it fires

> (): `void`

###### Returns

`void`

##### purge()

> **purge**(): `Promise`<`void`>

Cancel all pending scheduled messages everywhere in the pipeline.

Unlike [cancelAll](#cancelall) which only clears messages still waiting in JS,
`purge()` guarantees that nothing already in-flight will fire either.
Resolves when the flush is confirmed complete.

###### Returns

`Promise`<`void`>

##### recover()

> **recover**(): `Promise`<`boolean`>

Smart recovery — tries a quick resume first, falls back to full reload.

Use when you're not sure if the worklet is still alive (e.g. returning
from a long background period).

###### Returns

`Promise`<`boolean`>

true if audio is running after recovery

###### Example

```ts
document.addEventListener('visibilitychange', () => {
  if (document.visibilityState === 'visible') sonic.recover();
});
```

##### reload()

> **reload**(): `Promise`<`boolean`>

Full reload — destroys and recreates the worklet and WASM, then restores
all previously loaded synthdefs and audio buffers.

Emits `'setup'` so you can rebuild groups, FX chains, and bus routing.
Use when the worklet was killed (e.g. long background, browser reclaimed memory).

###### Returns

`Promise`<`boolean`>

true if reload succeeded

##### removeAllListeners()

> **removeAllListeners**(`event?`): `this`

Remove all listeners for an event, or all listeners entirely.

###### Parameters

| Parameter | Type                                              | Description                              |
| --------- | ------------------------------------------------- | ---------------------------------------- |
| `event?`  | keyof [`SuperSonicEventMap`](#supersoniceventmap) | Event name, or omit to remove everything |

###### Returns

`this`

***

#### Event Types

| Event                                                           | Description                                                                                                                                                                                                           |
| --------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| <a id="audiocontextinterrupted"></a> `audiocontext:interrupted` | AudioContext was interrupted (iOS-specific). Another app or system event took audio focus. Similar to suspended but triggered externally.                                                                             |
| <a id="audiocontextresumed"></a> `audiocontext:resumed`         | AudioContext resumed to 'running' state.                                                                                                                                                                              |
| <a id="audiocontextstatechange"></a> `audiocontext:statechange` | AudioContext state changed. State is one of: `'running'`, `'suspended'`, `'closed'`, or `'interrupted'`.                                                                                                              |
| <a id="audiocontextsuspended"></a> `audiocontext:suspended`     | AudioContext was suspended (e.g. tab backgrounded, autoplay policy, iOS audio interruption). Show a restart UI and call `recover()` when the user interacts.                                                          |
| <a id="debug"></a> `debug`                                      | Debug text output from scsynth (e.g. synthdef compilation messages). Includes NTP timestamp and sequence number.                                                                                                      |
| <a id="destroy-1"></a> `destroy`                                | Engine has been destroyed. Only fired by `destroy()`, not by `shutdown()` or `reset()`. Last chance to clean up before all listeners are cleared.                                                                     |
| <a id="error"></a> `error`                                      | Error from any component (worklet, transport, workers).                                                                                                                                                               |
| <a id="loadingcomplete"></a> `loading:complete`                 | An asset finished loading. Size is in bytes.                                                                                                                                                                          |
| <a id="loadingstart"></a> `loading:start`                       | An asset started loading. Type is `'wasm'`, `'synthdef'`, or `'sample'`.                                                                                                                                              |
| <a id="message"></a> `message`                                  | Decoded OSC message received from scsynth. Messages are plain arrays: `[address, ...args]`.                                                                                                                           |
| <a id="messageraw"></a> `message:raw`                           | Raw OSC bytes received from scsynth (before decoding). Includes NTP timestamps for timing analysis.                                                                                                                   |
| <a id="messagesent"></a> `message:sent`                         | Fired when an OSC message is sent to scsynth. Includes source worker ID, sequence number, and NTP timestamps.                                                                                                         |
| <a id="ready"></a> `ready`                                      | Fired when the engine is fully booted and ready to receive messages. Payload includes browser capabilities and boot timing.                                                                                           |
| <a id="reloadcomplete"></a> `reload:complete`                   | Full reload completed.                                                                                                                                                                                                |
| <a id="reloadstart"></a> `reload:start`                         | Full reload started (worklet and WASM will be recreated).                                                                                                                                                             |
| <a id="resumed"></a> `resumed`                                  | Audio resumed after a suspend (AudioContext was re-started). Emitted after `resume()` succeeds.                                                                                                                       |
| <a id="setup"></a> `setup`                                      | Fired after init completes, before `'ready'`. Use for setting up groups, FX chains, and bus routing. Can be async — init waits for all setup handlers to resolve. Also fires after `recover()` triggers a `reload()`. |
| <a id="shutdown-1"></a> `shutdown`                              | Engine is shutting down. Fired by `shutdown()`, `reset()`, and `destroy()`.                                                                                                                                           |

##### reset()

> **reset**(): `Promise`<`void`>

Shutdown and immediately re-initialise.

Equivalent to `await sonic.shutdown(); await sonic.init();`

###### Returns

`Promise`<`void`>

##### resume()

> **resume**(): `Promise`<`boolean`>

Quick resume — calls [purge](#purge) to flush stale messages, resumes
AudioContext, and resyncs timing.

Memory, node tree, and loaded synthdefs are preserved. Does not emit `'setup'`.
Use when you know the worklet is still running (e.g. tab was briefly backgrounded).

###### Returns

`Promise`<`boolean`>

true if the worklet is running after resume

##### sampleInfo()

> **sampleInfo**(`source`, `startFrame?`, `numFrames?`): `Promise`<[`SampleInfo`](#sampleinfo-1)>

Get sample metadata (including content hash) without allocating a buffer.

Fetches, decodes, and hashes the audio, returning the same info that
would appear in the [loadSample](#loadsample) result if the content were loaded.
No buffer slot is consumed and no OSC is sent to scsynth.

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

> **send**(`address`, ...`args`): `never`

###### Parameters

| Parameter | Type                   |
| --------- | ---------------------- |
| `address` | `"/d_load"`            |
| ...`args` | [`OscArg`](#oscarg)\[] |

###### Returns

`never`

###### Deprecated

Use loadSynthDef() or send('/d\_recv', bytes) instead. Filesystem access is not available in the browser.

###### Call Signature

> **send**(`address`, ...`args`): `never`

###### Parameters

| Parameter | Type                   |
| --------- | ---------------------- |
| `address` | `"/d_loadDir"`         |
| ...`args` | [`OscArg`](#oscarg)\[] |

###### Returns

`never`

###### Deprecated

Use loadSynthDef() or send('/d\_recv', bytes) instead. Filesystem access is not available in the browser.

###### Call Signature

> **send**(`address`, ...`args`): `never`

###### Parameters

| Parameter | Type                   |
| --------- | ---------------------- |
| `address` | `"/b_read"`            |
| ...`args` | [`OscArg`](#oscarg)\[] |

###### Returns

`never`

###### Deprecated

Use loadSample() instead. Filesystem access is not available in the browser.

###### Call Signature

> **send**(`address`, ...`args`): `never`

###### Parameters

| Parameter | Type                   |
| --------- | ---------------------- |
| `address` | `"/b_readChannel"`     |
| ...`args` | [`OscArg`](#oscarg)\[] |

###### Returns

`never`

###### Deprecated

Use loadSample() instead. Filesystem access is not available in the browser.

###### Call Signature

> **send**(`address`, ...`args`): `never`

###### Parameters

| Parameter | Type                   |
| --------- | ---------------------- |
| `address` | `"/b_write"`           |
| ...`args` | [`OscArg`](#oscarg)\[] |

###### Returns

`never`

###### Deprecated

File writing is not available in the browser.

###### Call Signature

> **send**(`address`, ...`args`): `never`

###### Parameters

| Parameter | Type                   |
| --------- | ---------------------- |
| `address` | `"/b_close"`           |
| ...`args` | [`OscArg`](#oscarg)\[] |

###### Returns

`never`

###### Deprecated

File writing is not available in the browser.

###### Call Signature

> **send**(`address`, ...`args`): `never`

###### Parameters

| Parameter | Type                   |
| --------- | ---------------------- |
| `address` | `"/clearSched"`        |
| ...`args` | [`OscArg`](#oscarg)\[] |

###### Returns

`never`

###### Deprecated

Use purge() to clear both the JS prescheduler and WASM scheduler.

###### Call Signature

> **send**(`address`, ...`args`): `never`

###### Parameters

| Parameter | Type                   |
| --------- | ---------------------- |
| `address` | `"/error"`             |
| ...`args` | [`OscArg`](#oscarg)\[] |

###### Returns

`never`

###### Deprecated

SuperSonic always enables error notifications so you never miss a /fail reply.

###### Call Signature

> **send**(`address`, ...`args`): `never`

###### Parameters

| Parameter | Type                   |
| --------- | ---------------------- |
| `address` | `"/quit"`              |
| ...`args` | [`OscArg`](#oscarg)\[] |

###### Returns

`never`

###### Deprecated

Use destroy() to shut down SuperSonic.

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
| `nodeID`      | [`NodeID`](#nodeid)       |
| `addAction`   | [`AddAction`](#addaction) |
| `targetID`    | [`NodeID`](#nodeid)       |
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
| `nodeID`      | [`NodeID`](#nodeid)       |
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
| `nodeID`  | [`NodeID`](#nodeid)  |
| `control` | `string` \| `number` |
| `count`   | `number`             |

###### Returns

`void`

###### Call Signature

> **send**(`address`, ...`nodeIDs`): `void`

Release client-side synth ID tracking. Synths continue running but are reassigned to reserved negative IDs. Use when you no longer need to communicate with the synth and want to reuse the ID.

###### Parameters

| Parameter    | Type                                  |
| ------------ | ------------------------------------- |
| `address`    | `"/s_noid"`                           |
| ...`nodeIDs` | \[[`NodeID`](#nodeid), `...NodeID[]`] |

###### Returns

`void`

###### Call Signature

> **send**(`address`, ...`nodeIDs`): `void`

Free (delete) one or more nodes.

###### Parameters

| Parameter    | Type                                  |
| ------------ | ------------------------------------- |
| `address`    | `"/n_free"`                           |
| ...`nodeIDs` | \[[`NodeID`](#nodeid), `...NodeID[]`] |

###### Returns

`void`

###### Call Signature

> **send**(`address`, `nodeID`, ...`controls`): `void`

Set node control values. Controls are alternating name/index and value pairs. If the node is a group, sets the control on all nodes in the group.

###### Parameters

| Parameter     | Type                      |
| ------------- | ------------------------- |
| `address`     | `"/n_set"`                |
| `nodeID`      | [`NodeID`](#nodeid)       |
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
| `nodeID`    | [`NodeID`](#nodeid)  |
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
| `nodeID`  | [`NodeID`](#nodeid)  |
| `control` | `string` \| `number` |
| `count`   | `number`             |
| `value`   | `number`             |

###### Returns

`void`

###### Call Signature

> **send**(`address`, ...`pairs`): `void`

Turn nodes on (1) or off (0). Args are repeating \[nodeID, flag] pairs.

###### Parameters

| Parameter  | Type                                              |
| ---------- | ------------------------------------------------- |
| `address`  | `"/n_run"`                                        |
| ...`pairs` | \[[`NodeID`](#nodeid), `0` \| `1`, `...NodeID[]`] |

###### Returns

`void`

###### Call Signature

> **send**(`address`, ...`pairs`): `void`

Move nodeA to execute immediately before nodeB. Args are repeating \[nodeA, nodeB] pairs.

###### Parameters

| Parameter  | Type                                                       |
| ---------- | ---------------------------------------------------------- |
| `address`  | `"/n_before"`                                              |
| ...`pairs` | \[[`NodeID`](#nodeid), [`NodeID`](#nodeid), `...NodeID[]`] |

###### Returns

`void`

###### Call Signature

> **send**(`address`, ...`pairs`): `void`

Move nodeA to execute immediately after nodeB. Args are repeating \[nodeA, nodeB] pairs.

###### Parameters

| Parameter  | Type                                                       |
| ---------- | ---------------------------------------------------------- |
| `address`  | `"/n_after"`                                               |
| ...`pairs` | \[[`NodeID`](#nodeid), [`NodeID`](#nodeid), `...NodeID[]`] |

###### Returns

`void`

###### Call Signature

> **send**(`address`, `addAction`, `targetID`, ...`nodeIDs`): `void`

Reorder nodes within a group. addAction: 0=head, 1=tail, 2=before target, 3=after target. Does not support 4 (replace).

###### Parameters

| Parameter    | Type                                  |
| ------------ | ------------------------------------- |
| `address`    | `"/n_order"`                          |
| `addAction`  | `0` \| `1` \| `2` \| `3`              |
| `targetID`   | [`NodeID`](#nodeid)                   |
| ...`nodeIDs` | \[[`NodeID`](#nodeid), `...NodeID[]`] |

###### Returns

`void`

###### Call Signature

> **send**(`address`, ...`nodeIDs`): `void`

Query node info. Replies with `/n_info` for each node: nodeID, parentGroupID, prevNodeID, nextNodeID, isGroup, \[headNodeID, tailNodeID].

###### Parameters

| Parameter    | Type                                  |
| ------------ | ------------------------------------- |
| `address`    | `"/n_query"`                          |
| ...`nodeIDs` | \[[`NodeID`](#nodeid), `...NodeID[]`] |

###### Returns

`void`

###### Call Signature

> **send**(`address`, ...`nodeIDs`): `void`

Print control values and calculation rates for each node to debug output. No reply message.

###### Parameters

| Parameter    | Type                                  |
| ------------ | ------------------------------------- |
| `address`    | `"/n_trace"`                          |
| ...`nodeIDs` | \[[`NodeID`](#nodeid), `...NodeID[]`] |

###### Returns

`void`

###### Call Signature

> **send**(`address`, `nodeID`, ...`mappings`): `void`

Map controls to read from control buses. Mappings are repeating \[control, busIndex] pairs. Set busIndex to -1 to unmap.

###### Parameters

| Parameter     | Type                      |
| ------------- | ------------------------- |
| `address`     | `"/n_map"`                |
| `nodeID`      | [`NodeID`](#nodeid)       |
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
| `nodeID`      | [`NodeID`](#nodeid)       |
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
| `nodeID`      | [`NodeID`](#nodeid)       |
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
| `nodeID`      | [`NodeID`](#nodeid)       |
| ...`mappings` | (`string` \| `number`)\[] |

###### Returns

`void`

###### Call Signature

> **send**(`address`, ...`args`): `void`

Create new groups. Args are repeating \[groupID, addAction, targetID] triplets. addAction: 0=head, 1=tail, 2=before, 3=after, 4=replace.

###### Parameters

| Parameter | Type                                                                                           |
| --------- | ---------------------------------------------------------------------------------------------- |
| `address` | `"/g_new"`                                                                                     |
| ...`args` | \[[`NodeID`](#nodeid), [`AddAction`](#addaction), [`NodeID`](#nodeid), ...(number \| UUID)\[]] |

###### Returns

`void`

###### Call Signature

> **send**(`address`, ...`args`): `void`

Create new parallel groups (children evaluated in unspecified order). Same signature as /g\_new.

###### Parameters

| Parameter | Type                                                                                           |
| --------- | ---------------------------------------------------------------------------------------------- |
| `address` | `"/p_new"`                                                                                     |
| ...`args` | \[[`NodeID`](#nodeid), [`AddAction`](#addaction), [`NodeID`](#nodeid), ...(number \| UUID)\[]] |

###### Returns

`void`

###### Call Signature

> **send**(`address`, ...`groupIDs`): `void`

Free all immediate children of one or more groups (groups themselves remain).

###### Parameters

| Parameter     | Type                                  |
| ------------- | ------------------------------------- |
| `address`     | `"/g_freeAll"`                        |
| ...`groupIDs` | \[[`NodeID`](#nodeid), `...NodeID[]`] |

###### Returns

`void`

###### Call Signature

> **send**(`address`, ...`groupIDs`): `void`

Recursively free all synths inside one or more groups and their nested sub-groups.

###### Parameters

| Parameter     | Type                                  |
| ------------- | ------------------------------------- |
| `address`     | `"/g_deepFree"`                       |
| ...`groupIDs` | \[[`NodeID`](#nodeid), `...NodeID[]`] |

###### Returns

`void`

###### Call Signature

> **send**(`address`, ...`pairs`): `void`

Move node to head of group. Args are repeating \[groupID, nodeID] pairs.

###### Parameters

| Parameter  | Type                                                       |
| ---------- | ---------------------------------------------------------- |
| `address`  | `"/g_head"`                                                |
| ...`pairs` | \[[`NodeID`](#nodeid), [`NodeID`](#nodeid), `...NodeID[]`] |

###### Returns

`void`

###### Call Signature

> **send**(`address`, ...`pairs`): `void`

Move node to tail of group. Args are repeating \[groupID, nodeID] pairs.

###### Parameters

| Parameter  | Type                                                       |
| ---------- | ---------------------------------------------------------- |
| `address`  | `"/g_tail"`                                                |
| ...`pairs` | \[[`NodeID`](#nodeid), [`NodeID`](#nodeid), `...NodeID[]`] |

###### Returns

`void`

###### Call Signature

> **send**(`address`, ...`groupFlagPairs`): `void`

Print group's node tree to debug output. Args are repeating \[groupID, flag] pairs. flag: 0=structure only, non-zero=include control values. No reply message.

###### Parameters

| Parameter           | Type                                            |
| ------------------- | ----------------------------------------------- |
| `address`           | `"/g_dumpTree"`                                 |
| ...`groupFlagPairs` | \[[`NodeID`](#nodeid), `number`, `...NodeID[]`] |

###### Returns

`void`

###### Call Signature

> **send**(`address`, ...`groupFlagPairs`): `void`

Query group tree structure. Args are repeating \[groupID, flag] pairs. flag: 0=structure only, non-zero=include control values. Replies with `/g_queryTree.reply`.

###### Parameters

| Parameter           | Type                                            |
| ------------------- | ----------------------------------------------- |
| `address`           | `"/g_queryTree"`                                |
| ...`groupFlagPairs` | \[[`NodeID`](#nodeid), `number`, `...NodeID[]`] |

###### Returns

`void`

###### Call Signature

> **send**(`address`, `nodeID`, `ugenIndex`, `command`, ...`args`): `void`

Send a command to a specific UGen instance within a synth. The command name and args are UGen-specific.

###### Parameters

| Parameter   | Type                   |
| ----------- | ---------------------- |
| `address`   | `"/u_cmd"`             |
| `nodeID`    | [`NodeID`](#nodeid)    |
| `ugenIndex` | `number`               |
| `command`   | `string`               |
| ...`args`   | [`OscArg`](#oscarg)\[] |

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

| Parameter | Type                   |
| --------- | ---------------------- |
| `address` | `"/b_gen"`             |
| `bufnum`  | `number`               |
| `command` | `string`               |
| ...`args` | [`OscArg`](#oscarg)\[] |

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

Send any OSC message. Use this for commands not covered by typed overloads, or for multi-range variants of commands like /n\_setn, /b\_fill, /c\_getn.

###### Parameters

| Parameter | Type                   |
| --------- | ---------------------- |
| `address` | `string`               |
| ...`args` | [`OscArg`](#oscarg)\[] |

###### Returns

`void`

***

#### OSC Argument Types

OSC argument types that can be sent in a message.

Plain JS values are mapped to OSC types automatically:

* `number` (integer) → `i` (int32)
* `number` (float) → `f` (float32)
* `string` → `s`
* `boolean` → `T` / `F`
* `Uint8Array` / `ArrayBuffer` → `b` (blob)

For 64-bit, timetag, or UUID types, use the tagged object form:

```ts
{ type: 'int', value: 42 }
{ type: 'float', value: 440 }     // force float32 for whole numbers
{ type: 'string', value: 'hello' }
{ type: 'blob', value: new Uint8Array([1,2,3]) }
{ type: 'bool', value: true }
{ type: 'int64', value: 9007199254740992n }
{ type: 'double', value: 3.141592653589793 }
{ type: 'timetag', value: ntpTimestamp }
{ type: 'uuid', value: new Uint8Array(16) }
```

##### sendOSC()

> **sendOSC**(`oscData`, `options?`): `void`

Send pre-encoded OSC bytes to scsynth.

Use this when you've already encoded the message (e.g. via `SuperSonic.osc.encodeMessage`)
or when sending from a worker that produces raw OSC. Sends bytes as-is without
rewriting — buffer allocation commands (`/b_alloc*`) are not transformed.
Use [send](#send-1) for buffer commands so they are handled correctly.

###### Parameters

| Parameter  | Type                                             | Description                           |
| ---------- | ------------------------------------------------ | ------------------------------------- |
| `oscData`  | `ArrayBuffer` \| `Uint8Array`<`ArrayBufferLike`> | Encoded OSC message or bundle bytes   |
| `options?` | [`SendOSCOptions`](#sendoscoptions)              | Optional session/tag for cancellation |

###### Returns

`void`

###### Throws

If the bundle exceeds the maximum schedulable size

###### Example

```ts
const msg = SuperSonic.osc.encodeMessage('/n_set', [1001, 'freq', 880]);
sonic.sendOSC(msg);

// With cancellation tags:
const bundle = SuperSonic.osc.encodeBundle(futureTime, packets);
sonic.sendOSC(bundle, { sessionId: 'song1', runTag: 'verse' });
```

##### setClockOffset()

> **setClockOffset**(`offsetS`): `void`

Set clock offset for multi-system sync (e.g. Ableton Link, NTP server).

Shifts all scheduled bundle execution times by the specified offset.
Positive values mean the shared/server clock is ahead of local time.

###### Parameters

| Parameter | Type     | Description       |
| --------- | -------- | ----------------- |
| `offsetS` | `number` | Offset in seconds |

###### Returns

`void`

##### shutdown()

> **shutdown**(): `Promise`<`void`>

Shut down the engine. The instance can be re-initialised with [init](#init).

Closes the AudioContext, terminates workers, and releases memory.
Emits `'shutdown'`.

###### Returns

`Promise`<`void`>

##### startCapture()

> **startCapture**(): `void`

Start capturing audio output to a buffer. SAB mode only.

###### Returns

`void`

##### stopCapture()

> **stopCapture**(): `Float32Array`

Stop capturing and return the captured audio data.

###### Returns

`Float32Array`

##### suspend()

> **suspend**(): `Promise`<`void`>

Suspend the AudioContext and stop the drift timer.

The worklet remains loaded but audio processing stops.
Use [resume](#resume) or [recover](#recover) to restart.

###### Returns

`Promise`<`void`>

##### sync()

> **sync**(`syncId?`): `Promise`<`void`>

Wait for scsynth to process all pending commands.

Sends a `/sync` message and waits for the `/synced` reply. Use after
loading synthdefs or buffers to ensure they're ready before creating synths.

###### Parameters

| Parameter | Type     | Description                                 |
| --------- | -------- | ------------------------------------------- |
| `syncId?` | `number` | Optional custom sync ID (random if omitted) |

###### Returns

`Promise`<`void`>

###### Throws

Rejects after 10 seconds if scsynth doesn't respond.

###### Example

```ts
await sonic.loadSynthDef('beep');
await sonic.sync();
// SynthDef is now guaranteed to be loaded
await sonic.send('/s_new', 'beep', 1001, 0, 1);
```

##### getMetricsSchema()

> `static` **getMetricsSchema**(): [`MetricsSchema`](#metricsschema)

Get the metrics schema describing all available metrics.

Includes array offsets for zero-allocation reading via [getMetricsArray](#getmetricsarray),
metric types/units/descriptions, and a declarative UI layout used by the
`<supersonic-metrics>` web component.

See docs/METRICS\_COMPONENT.md for the metrics component guide.

###### Returns

[`MetricsSchema`](#metricsschema)

##### getRawTreeSchema()

> `static` **getRawTreeSchema**(): `Record`<`string`, `unknown`>

Get schema describing the raw flat node tree structure.

###### Returns

`Record`<`string`, `unknown`>

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

| Member                                        | Description                                                                     |
| --------------------------------------------- | ------------------------------------------------------------------------------- |
| [`getCurrentNTP`](#getcurrentntp)             | Set the NTP time source for classification (used in AudioWorklet context).      |
| [`mode`](#mode)                               | Transport mode this channel is using.                                           |
| [`transferable`](#transferable)               | Serializable config for transferring this channel to a worker via postMessage.  |
| [`transferList`](#transferlist)               | Array of transferable objects (MessagePorts) for the postMessage transfer list. |
| [`classify()`](#classify)                     | Classify an OSC message to determine its routing.                               |
| [`close()`](#close)                           | Close the channel and release its ports.                                        |
| [`getAndResetMetrics()`](#getandresetmetrics) | Get and reset local metrics (for periodic reporting).                           |
| [`getMetrics()`](#getmetrics)                 | Get current metrics snapshot.                                                   |
| [`nextNodeId()`](#nextnodeid)                 | Get the next unique node ID.                                                    |
| [`send()`](#send)                             | Send an OSC message with automatic routing.                                     |
| [`sendDirect()`](#senddirect)                 | Send directly to worklet without classification or metrics tracking.            |
| [`sendToPrescheduler()`](#sendtoprescheduler) | Send to prescheduler without classification.                                    |
| [`fromTransferable()`](#fromtransferable)     | Reconstruct an OscChannel from data received via postMessage in a worker.       |

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
const channel = OscChannel.fromTransferable(event.data.channel);
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

##### getCurrentNTP

###### Set Signature

> **set** **getCurrentNTP**(`fn`): `void`

Set the NTP time source for classification (used in AudioWorklet context).

###### Parameters

| Parameter | Type           |
| --------- | -------------- |
| `fn`      | () => `number` |

###### Returns

`void`

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

###### Example

```ts
worker.postMessage({ ch: channel.transferable }, channel.transferList);
```

###### Returns

`Transferable`\[]

***

#### Methods

##### classify()

> **classify**(`oscData`): [`OscCategory`](#osccategory)

Classify an OSC message to determine its routing.

###### Parameters

| Parameter | Type         | Description       |
| --------- | ------------ | ----------------- |
| `oscData` | `Uint8Array` | Encoded OSC bytes |

###### Returns

[`OscCategory`](#osccategory)

##### close()

> **close**(): `void`

Close the channel and release its ports.

###### Returns

`void`

##### getAndResetMetrics()

> **getAndResetMetrics**(): [`OscChannelMetrics`](#oscchannelmetrics)

Get and reset local metrics (for periodic reporting).

###### Returns

[`OscChannelMetrics`](#oscchannelmetrics)

##### getMetrics()

> **getMetrics**(): [`OscChannelMetrics`](#oscchannelmetrics)

Get current metrics snapshot.

###### Returns

[`OscChannelMetrics`](#oscchannelmetrics)

##### nextNodeId()

> **nextNodeId**(): `number`

Get the next unique node ID.

Thread-safe — can be called concurrently from multiple workers and no
two callers will ever receive the same ID. IDs start at 1000 (0 is
the root group, 1 is the default group, 2–999 are reserved for manual use).

###### Returns

`number`

A unique node ID (>= 1000)

##### send()

> **send**(`oscData`): `boolean`

Send an OSC message with automatic routing.

Classifies the message and routes it:

* bypass categories → sent directly to the AudioWorklet
* far-future bundles → routed to the prescheduler for timed dispatch

###### Parameters

| Parameter | Type         | Description       |
| --------- | ------------ | ----------------- |
| `oscData` | `Uint8Array` | Encoded OSC bytes |

###### Returns

`boolean`

true if sent successfully

##### sendDirect()

> **sendDirect**(`oscData`): `boolean`

Send directly to worklet without classification or metrics tracking.

###### Parameters

| Parameter | Type         | Description       |
| --------- | ------------ | ----------------- |
| `oscData` | `Uint8Array` | Encoded OSC bytes |

###### Returns

`boolean`

true if sent successfully

##### sendToPrescheduler()

> **sendToPrescheduler**(`oscData`): `boolean`

Send to prescheduler without classification.

###### Parameters

| Parameter | Type         | Description       |
| --------- | ------------ | ----------------- |
| `oscData` | `Uint8Array` | Encoded OSC bytes |

###### Returns

`boolean`

true if sent successfully

##### fromTransferable()

> `static` **fromTransferable**(`data`): [`OscChannel`](#oscchannel)

Reconstruct an OscChannel from data received via postMessage in a worker.

###### Parameters

| Parameter | Type                                                  | Description                                         |
| --------- | ----------------------------------------------------- | --------------------------------------------------- |
| `data`    | [`OscChannelTransferable`](#oscchanneltransferable-1) | The transferable config from `channel.transferable` |

###### Returns

[`OscChannel`](#oscchannel)

###### Example

```ts
// In a Web Worker:
self.onmessage = (e) => {
  const channel = OscChannel.fromTransferable(e.data.ch);
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

| Name                                                      | Type                                                                 | Description                                                                                                                                                                                              |
| --------------------------------------------------------- | -------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| <a id="property-ntp_epoch_offset"></a> `NTP_EPOCH_OFFSET` | `number`                                                             | Seconds between NTP epoch (1900) and Unix epoch (1970): `2208988800`.                                                                                                                                    |
| `decode()`                                                | (`data`) => [`OscBundle`](#oscbundle) \| [`OscMessage`](#oscmessage) | Decode an OSC packet (message or bundle).                                                                                                                                                                |
| `encodeBundle()`                                          | (`timeTag`, `packets`) => `Uint8Array`                               | Encode an OSC bundle with multiple packets. **Example** `const time = osc.ntpNow() + 1.0; // 1 second from now osc.encodeBundle(time, [ ['/n_set', 1001, 'freq', 880], ['/n_set', 1001, 'amp', 0.5], ])` |
| `encodeMessage()`                                         | (`address`, `args?`) => `Uint8Array`                                 | Encode an OSC message. **Example** `osc.encodeMessage('/s_new', ['beep', 1001, 0, 1, 'freq', 440])`                                                                                                      |
| `encodeSingleBundle()`                                    | (`timeTag`, `address`, `args?`) => `Uint8Array`                      | Encode a single-message bundle (common case optimisation). Equivalent to `encodeBundle(timeTag, [[address, ...args]])` but faster.                                                                       |
| `ntpNow()`                                                | () => `number`                                                       | Get the current time as an NTP timestamp (seconds since 1900). Use this to schedule bundles relative to now: **Example** `const halfSecondFromNow = osc.ntpNow() + 0.5;`                                 |
| `readTimetag()`                                           | (`bundleData`) => `object`                                           | Read the timetag from a bundle without fully decoding it.                                                                                                                                                |

***

#### Example

```ts
import { SuperSonic } from 'supersonic-scsynth';

// Encode a message
const msg = SuperSonic.osc.encodeMessage('/s_new', ['beep', 1001, 0, 1]);

// Encode a timed bundle
const time = SuperSonic.osc.ntpNow() + 0.5; // 500ms from now
const bundle = SuperSonic.osc.encodeBundle(time, [
  ['/s_new', 'beep', 1001, 0, 1, 'freq', 440],
  ['/s_new', 'beep', 1002, 0, 1, 'freq', 660],
]);

// Decode incoming data
const decoded = SuperSonic.osc.decode(rawBytes);
```

## Interfaces

### ActivityLineConfig

Configuration for truncating activity log lines.

#### Properties

| Property                                                  | Type     | Description                                                                  |
| --------------------------------------------------------- | -------- | ---------------------------------------------------------------------------- |
| <a id="maxlinelength"></a> `maxLineLength?`               | `number` | Default max line length for all activity types. Default: 200.                |
| <a id="oscinmaxlinelength"></a> `oscInMaxLineLength?`     | `number` | Override max line length for OSC in messages. null = use maxLineLength.      |
| <a id="oscoutmaxlinelength"></a> `oscOutMaxLineLength?`   | `number` | Override max line length for OSC out messages. null = use maxLineLength.     |
| <a id="scsynthmaxlinelength"></a> `scsynthMaxLineLength?` | `number` | Override max line length for scsynth debug output. null = use maxLineLength. |

***

### BootStats

Boot timing statistics.

#### Properties

| Property                                   | Type     | Description                                                 |
| ------------------------------------------ | -------- | ----------------------------------------------------------- |
| <a id="initduration"></a> `initDuration`   | `number` | Total boot duration in ms, or null if not yet booted.       |
| <a id="initstarttime"></a> `initStartTime` | `number` | Timestamp when init() started (performance.now()), or null. |

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

| Property                               | Type                                                 | Description                                                            |
| -------------------------------------- | ---------------------------------------------------- | ---------------------------------------------------------------------- |
| <a id="description"></a> `description` | `string`                                             | Human-readable description.                                            |
| <a id="offset"></a> `offset`           | `number`                                             | Offset into the flat metrics Uint32Array.                              |
| <a id="signed"></a> `signed?`          | `boolean`                                            | Whether the value should be read as signed int32.                      |
| <a id="type"></a> `type`               | `"counter"` \| `"gauge"` \| `"constant"` \| `"enum"` | Metric type: counter (cumulative), gauge (current), constant, or enum. |
| <a id="unit"></a> `unit?`              | `string`                                             | Unit of measurement.                                                   |
| <a id="values"></a> `values?`          | `string`\[]                                          | Enum value names (for type 'enum').                                    |

***

### MetricsSchema

Metrics schema returned by [SuperSonic.getMetricsSchema](#getmetricsschema).

Contains metric definitions with array offsets (for zero-allocation reading),
a declarative UI layout for rendering metrics panels, and sentinel values.

#### Properties

| Property                           | Type                                                                                                | Description                                                                      |
| ---------------------------------- | --------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------- |
| <a id="layout"></a> `layout`       | `object`                                                                                            | Panel structure for rendering a metrics UI. Used by `<supersonic-metrics>`.      |
| `layout.panels`                    | `object`\[]                                                                                         | -                                                                                |
| <a id="metrics"></a> `metrics`     | `Record`\<keyof [`SuperSonicMetrics`](#supersonicmetrics), [`MetricDefinition`](#metricdefinition)> | Each key maps to offset, type, unit, and description for the merged Uint32Array. |
| <a id="sentinels"></a> `sentinels` | `object`                                                                                            | Magic values used in the metrics array.                                          |
| `sentinels.HEADROOM_UNSET`         | `number`                                                                                            | Value of preschedulerMinHeadroomMs before any data arrives.                      |

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
| <a id="bypassed"></a> `bypassed`         | `number` |
| <a id="bytessent"></a> `bytesSent`       | `number` |
| <a id="immediate"></a> `immediate`       | `number` |
| <a id="late"></a> `late`                 | `number` |
| <a id="messagessent"></a> `messagesSent` | `number` |
| <a id="nearfuture"></a> `nearFuture`     | `number` |
| <a id="nonbundle"></a> `nonBundle`       | `number` |

***

### OscChannelPMTransferable

Transferable config for postMessage mode OscChannel.

#### Properties

| Property                                         | Type            |
| ------------------------------------------------ | --------------- |
| <a id="blocking"></a> `blocking`                 | `boolean`       |
| <a id="bypasslookaheads"></a> `bypassLookaheadS` | `number`        |
| <a id="mode-2"></a> `mode`                       | `"postMessage"` |
| <a id="port"></a> `port`                         | `MessagePort`   |
| <a id="preschedulerport"></a> `preschedulerPort` | `MessagePort`   |
| <a id="sourceid"></a> `sourceId`                 | `number`        |

***

### OscChannelSABTransferable

Transferable config for SAB mode OscChannel.

#### Properties

| Property                                           | Type                         |
| -------------------------------------------------- | ---------------------------- |
| <a id="blocking-1"></a> `blocking`                 | `boolean`                    |
| <a id="bufferconstants-1"></a> `bufferConstants`   | `Record`<`string`, `number`> |
| <a id="bypasslookaheads-1"></a> `bypassLookaheadS` | `number`                     |
| <a id="controlindices"></a> `controlIndices`       | `Record`<`string`, `number`> |
| <a id="mode-3"></a> `mode`                         | `"sab"`                      |
| <a id="preschedulerport-1"></a> `preschedulerPort` | `MessagePort`                |
| <a id="ringbufferbase-1"></a> `ringBufferBase`     | `number`                     |
| <a id="sharedbuffer-1"></a> `sharedBuffer`         | `SharedArrayBuffer`          |
| <a id="sourceid-1"></a> `sourceId`                 | `number`                     |

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

| Property                         | Type                | Description                                           |
| -------------------------------- | ------------------- | ----------------------------------------------------- |
| <a id="defname"></a> `defName`   | `string`            | SynthDef name (synths only, empty string for groups). |
| <a id="headid"></a> `headId`     | [`NodeID`](#nodeid) | First child node ID (groups only, -1 if empty).       |
| <a id="id"></a> `id`             | [`NodeID`](#nodeid) | Unique node ID.                                       |
| <a id="isgroup"></a> `isGroup`   | `boolean`           | true if group, false if synth.                        |
| <a id="nextid"></a> `nextId`     | [`NodeID`](#nodeid) | Next sibling node ID (-1 if none).                    |
| <a id="parentid"></a> `parentId` | [`NodeID`](#nodeid) | Parent node ID (-1 for root).                         |
| <a id="previd"></a> `prevId`     | [`NodeID`](#nodeid) | Previous sibling node ID (-1 if none).                |

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

### SendOSCOptions

Options for [SuperSonic.sendOSC](#sendosc).

#### Properties

| Property                            | Type     | Description                                                                 |
| ----------------------------------- | -------- | --------------------------------------------------------------------------- |
| <a id="runtag"></a> `runTag?`       | `string` | Run tag for cancellation via [SuperSonic.cancelTag](#canceltag).            |
| <a id="sessionid"></a> `sessionId?` | `string` | Session ID for cancellation via [SuperSonic.cancelSession](#cancelsession). |

***

### Snapshot

Diagnostic snapshot returned by [SuperSonic.getSnapshot](#getsnapshot).

Captures metrics with descriptions, the current node tree, and JS heap
memory info. Useful for bug reports and debugging timing issues.

#### Properties

| Property                           | Type                                                                 | Description                                                |
| ---------------------------------- | -------------------------------------------------------------------- | ---------------------------------------------------------- |
| <a id="memory"></a> `memory`       | `object`                                                             | JS heap memory info (Chrome only, null in other browsers). |
| `memory.jsHeapSizeLimit`           | `number`                                                             | -                                                          |
| `memory.totalJSHeapSize`           | `number`                                                             | -                                                          |
| `memory.usedJSHeapSize`            | `number`                                                             | -                                                          |
| <a id="metrics-1"></a> `metrics`   | `Record`<`string`, { `description?`: `string`; `value`: `number`; }> | All metrics with their current values and descriptions.    |
| <a id="nodetree"></a> `nodeTree`   | [`RawTree`](#rawtree)                                                | Current node tree in flat format.                          |
| <a id="timestamp"></a> `timestamp` | `string`                                                             | ISO 8601 timestamp when the snapshot was taken.            |

### SuperSonicInfo

Engine info returned by [SuperSonic.getInfo](#getinfo).

#### Properties

| Property                                     | Type      | Description                                                  |
| -------------------------------------------- | --------- | ------------------------------------------------------------ |
| <a id="boottimems"></a> `bootTimeMs`         | `number`  | Time taken to boot in ms, or null if not yet booted.         |
| <a id="bufferpoolsize"></a> `bufferPoolSize` | `number`  | Audio sample buffer pool size in bytes.                      |
| <a id="capabilities"></a> `capabilities`     | `object`  | Browser capability detection results.                        |
| `capabilities.atomics`                       | `boolean` | -                                                            |
| `capabilities.audioWorklet`                  | `boolean` | -                                                            |
| `capabilities.crossOriginIsolated`           | `boolean` | -                                                            |
| `capabilities.sharedArrayBuffer`             | `boolean` | -                                                            |
| `capabilities.webWorker`                     | `boolean` | -                                                            |
| <a id="numbuffers-1"></a> `numBuffers`       | `number`  | Max audio buffers configured.                                |
| <a id="samplerate-3"></a> `sampleRate`       | `number`  | AudioContext sample rate (e.g. 48000).                       |
| <a id="totalmemory"></a> `totalMemory`       | `number`  | Total WebAssembly memory in bytes.                           |
| <a id="version-1"></a> `version`             | `string`  | scsynth WASM version string, or null if not yet initialised. |
| <a id="wasmheapsize"></a> `wasmHeapSize`     | `number`  | WASM heap size available for scsynth allocations.            |

***

### SuperSonicMetrics

Complete metrics snapshot returned by [SuperSonic.getMetrics](#getmetrics-1).

All values are numbers. Counter metrics are cumulative; gauge metrics
reflect current state. Use [SuperSonic.getMetricsSchema](#getmetricsschema) for
descriptions, units, and UI layout metadata.

#### Properties

| Property                                                                 | Type     | Description                                                                                   |
| ------------------------------------------------------------------------ | -------- | --------------------------------------------------------------------------------------------- |
| <a id="audiocontextstate"></a> `audioContextState`                       | `number` | AudioContext state as enum index: 0=unknown, 1=running, 2=suspended, 3=closed, 4=interrupted. |
| <a id="bufferpoolallocations"></a> `bufferPoolAllocations`               | `number` | Total buffer pool allocations.                                                                |
| <a id="bufferpoolavailablebytes"></a> `bufferPoolAvailableBytes`         | `number` | Buffer pool bytes available.                                                                  |
| <a id="bufferpoolusedbytes"></a> `bufferPoolUsedBytes`                   | `number` | Buffer pool bytes currently in use.                                                           |
| <a id="bypassimmediate"></a> `bypassImmediate`                           | `number` | Bundles with timetag 0 or 1 that bypassed prescheduler.                                       |
| <a id="bypasslate"></a> `bypassLate`                                     | `number` | Late bundles that bypassed prescheduler.                                                      |
| <a id="bypassnearfuture"></a> `bypassNearFuture`                         | `number` | Bundles within lookahead threshold that bypassed prescheduler.                                |
| <a id="bypassnonbundle"></a> `bypassNonBundle`                           | `number` | Plain OSC messages (not bundles) that bypassed prescheduler.                                  |
| <a id="clockoffsetms"></a> `clockOffsetMs`                               | `number` | Clock offset for multi-system sync (ms, signed).                                              |
| <a id="debugbuffercapacity"></a> `debugBufferCapacity`                   | `number` | DEBUG ring buffer capacity (bytes).                                                           |
| <a id="debugbufferpeakbytes"></a> `debugBufferPeakBytes`                 | `number` | Peak bytes used in DEBUG ring buffer.                                                         |
| <a id="debugbufferusedbytes"></a> `debugBufferUsedBytes`                 | `number` | Bytes used in DEBUG ring buffer.                                                              |
| <a id="debugbytesreceived"></a> `debugBytesReceived`                     | `number` | Debug bytes received from scsynth.                                                            |
| <a id="debugmessagesreceived"></a> `debugMessagesReceived`               | `number` | Debug messages received from scsynth.                                                         |
| <a id="driftoffsetms"></a> `driftOffsetMs`                               | `number` | Clock drift between AudioContext and wall clock (ms, signed).                                 |
| <a id="inbuffercapacity"></a> `inBufferCapacity`                         | `number` | IN ring buffer capacity (bytes).                                                              |
| <a id="inbufferpeakbytes"></a> `inBufferPeakBytes`                       | `number` | Peak bytes used in IN ring buffer.                                                            |
| <a id="inbufferusedbytes"></a> `inBufferUsedBytes`                       | `number` | Bytes used in IN ring buffer (JS → scsynth).                                                  |
| <a id="loadedsynthdefs-1"></a> `loadedSynthDefs`                         | `number` | Number of loaded synthdefs.                                                                   |
| <a id="mode-4"></a> `mode`                                               | `number` | Transport mode as enum index: 0=sab, 1=postMessage.                                           |
| <a id="oscinbytesreceived"></a> `oscInBytesReceived`                     | `number` | Total bytes received from scsynth.                                                            |
| <a id="oscincorrupted"></a> `oscInCorrupted`                             | `number` | Corrupted messages detected from scsynth.                                                     |
| <a id="oscinmessagesdropped"></a> `oscInMessagesDropped`                 | `number` | Replies lost in transit from scsynth to JS.                                                   |
| <a id="oscinmessagesreceived"></a> `oscInMessagesReceived`               | `number` | OSC replies received from scsynth.                                                            |
| <a id="oscoutbytessent"></a> `oscOutBytesSent`                           | `number` | Total bytes sent from JS to scsynth.                                                          |
| <a id="oscoutmessagessent"></a> `oscOutMessagesSent`                     | `number` | OSC messages sent from JS to scsynth.                                                         |
| <a id="outbuffercapacity"></a> `outBufferCapacity`                       | `number` | OUT ring buffer capacity (bytes).                                                             |
| <a id="outbufferpeakbytes"></a> `outBufferPeakBytes`                     | `number` | Peak bytes used in OUT ring buffer.                                                           |
| <a id="outbufferusedbytes"></a> `outBufferUsedBytes`                     | `number` | Bytes used in OUT ring buffer (scsynth → JS).                                                 |
| <a id="preschedulerbundlesscheduled"></a> `preschedulerBundlesScheduled` | `number` | Bundles added to prescheduler.                                                                |
| <a id="preschedulerbypassed"></a> `preschedulerBypassed`                 | `number` | Messages sent directly, bypassing prescheduler (aggregate).                                   |
| <a id="preschedulercapacity"></a> `preschedulerCapacity`                 | `number` | Maximum pending events in JS prescheduler.                                                    |
| <a id="preschedulerdispatched"></a> `preschedulerDispatched`             | `number` | Events sent from prescheduler to worklet.                                                     |
| <a id="preschedulereventscancelled"></a> `preschedulerEventsCancelled`   | `number` | Bundles cancelled before dispatch.                                                            |
| <a id="preschedulerlates"></a> `preschedulerLates`                       | `number` | Bundles dispatched after their scheduled time.                                                |
| <a id="preschedulermaxlatems"></a> `preschedulerMaxLateMs`               | `number` | Maximum lateness at prescheduler (ms).                                                        |
| <a id="preschedulermessagesretried"></a> `preschedulerMessagesRetried`   | `number` | Total messages that needed retry.                                                             |
| <a id="preschedulerminheadroomms"></a> `preschedulerMinHeadroomMs`       | `number` | Smallest time gap between dispatch and execution (ms). 0xFFFFFFFF = no data yet.              |
| <a id="preschedulerpending"></a> `preschedulerPending`                   | `number` | Events waiting in JS prescheduler queue.                                                      |
| <a id="preschedulerpendingpeak"></a> `preschedulerPendingPeak`           | `number` | Peak pending events.                                                                          |
| <a id="preschedulerretriesfailed"></a> `preschedulerRetriesFailed`       | `number` | Ring buffer write retries that failed.                                                        |
| <a id="preschedulerretriessucceeded"></a> `preschedulerRetriesSucceeded` | `number` | Ring buffer write retries that succeeded.                                                     |
| <a id="preschedulerretryqueuepeak"></a> `preschedulerRetryQueuePeak`     | `number` | Peak retry queue size.                                                                        |
| <a id="preschedulerretryqueuesize"></a> `preschedulerRetryQueueSize`     | `number` | Current retry queue size.                                                                     |
| <a id="preschedulertotaldispatches"></a> `preschedulerTotalDispatches`   | `number` | Total dispatch cycles.                                                                        |
| <a id="ringbufferdirectwritefails"></a> `ringBufferDirectWriteFails`     | `number` | SAB mode only: optimistic direct writes that fell back to prescheduler.                       |
| <a id="scsynthmessagesdropped"></a> `scsynthMessagesDropped`             | `number` | Messages dropped by scsynth (scheduler queue full).                                           |
| <a id="scsynthmessagesprocessed"></a> `scsynthMessagesProcessed`         | `number` | OSC messages processed by scsynth.                                                            |
| <a id="scsynthprocesscount"></a> `scsynthProcessCount`                   | `number` | Audio process() calls (cumulative).                                                           |
| <a id="scsynthschedulercapacity"></a> `scsynthSchedulerCapacity`         | `number` | Maximum scsynth scheduler queue size (compile-time constant).                                 |
| <a id="scsynthschedulerdepth"></a> `scsynthSchedulerDepth`               | `number` | Current scsynth scheduler queue depth.                                                        |
| <a id="scsynthschedulerdropped"></a> `scsynthSchedulerDropped`           | `number` | Messages dropped from scsynth scheduler queue.                                                |
| <a id="scsynthschedulerlastlatems"></a> `scsynthSchedulerLastLateMs`     | `number` | Most recent late magnitude in scsynth scheduler (ms).                                         |
| <a id="scsynthschedulerlastlatetick"></a> `scsynthSchedulerLastLateTick` | `number` | Process count when last scsynth late occurred.                                                |
| <a id="scsynthschedulerlates"></a> `scsynthSchedulerLates`               | `number` | Bundles executed after their scheduled time.                                                  |
| <a id="scsynthschedulermaxlatems"></a> `scsynthSchedulerMaxLateMs`       | `number` | Maximum lateness observed in scsynth scheduler (ms).                                          |
| <a id="scsynthschedulerpeakdepth"></a> `scsynthSchedulerPeakDepth`       | `number` | Peak scsynth scheduler queue depth (high water mark).                                         |
| <a id="scsynthsequencegaps"></a> `scsynthSequenceGaps`                   | `number` | Messages lost in transit from JS to scsynth.                                                  |
| <a id="scsynthwasmerrors"></a> `scsynthWasmErrors`                       | `number` | WASM execution errors in audio worklet.                                                       |

### Tree

Hierarchical node tree returned by [SuperSonic.getTree](#gettree).

#### Example

```ts
const tree = sonic.getTree();
console.log(tree.root.children); // top-level groups and synths
console.log(tree.nodeCount);     // total nodes in the tree
```

***

#### Properties

| Property                                   | Type                    | Description                                                          |
| ------------------------------------------ | ----------------------- | -------------------------------------------------------------------- |
| <a id="droppedcount-1"></a> `droppedCount` | `number`                | Nodes that exceeded mirror capacity (tree may be incomplete if > 0). |
| <a id="nodecount-1"></a> `nodeCount`       | `number`                | Total number of nodes.                                               |
| <a id="root"></a> `root`                   | [`TreeNode`](#treenode) | Root group (always id 0).                                            |
| <a id="version-2"></a> `version`           | `number`                | Increments on any tree change — useful for detecting updates.        |

***

### TreeNode

A node in the hierarchical synth tree.

Groups contain children; synths are leaves.

#### Properties

| Property                         | Type                       | Description                                           |
| -------------------------------- | -------------------------- | ----------------------------------------------------- |
| <a id="children"></a> `children` | [`TreeNode`](#treenode)\[] | Child nodes (groups only, empty array for synths).    |
| <a id="defname-1"></a> `defName` | `string`                   | SynthDef name (synths only, empty string for groups). |
| <a id="id-1"></a> `id`           | [`NodeID`](#nodeid)        | Unique node ID.                                       |
| <a id="type-1"></a> `type`       | `"group"` \| `"synth"`     | `'group'` for groups, `'synth'` for synth nodes.      |

## Type Aliases

### AddAction

> **AddAction** = `0` | `1` | `2` | `3` | `4`

Node add action: 0=head, 1=tail, 2=before, 3=after, 4=replace

***

### BlockedCommand

> **BlockedCommand** = `"/d_load"` | `"/d_loadDir"` | `"/b_read"` | `"/b_readChannel"` | `"/b_write"` | `"/b_close"` | `"/clearSched"` | `"/error"` | `"/quit"`

Commands blocked at runtime — typed as compile-time errors

***

### NodeID

> **NodeID** = `number` | [`UUID`](#uuid)

A node identifier — either a classic i32 or a v7 UUID.

UUIDs are rewritten to i32s at the AudioWorklet boundary and back again
on the way out, so concurrent clients can create and track synths without
coordinating over a shared integer numbering system.

***

### NTPTimeTag

> **NTPTimeTag** = `number` | \[`number`, `number`] | `1` | `null` | `undefined`

NTP timetag for bundle encoding.

* `1` or `null` or `undefined` → immediate execution
* `number` → NTP seconds since 1900
* `[seconds, fraction]` → raw NTP pair (both uint32)

### OscBundlePacket

> **OscBundlePacket** = [`OscMessage`](#oscmessage) | { `address`: `string`; `args?`: [`OscArg`](#oscarg)\[]; } | { `packets`: [`OscBundlePacket`](#oscbundlepacket)\[]; `timeTag`: [`NTPTimeTag`](#ntptimetag); }

A packet that can be included in an OSC bundle.

Accepts three formats:

#### Example

```ts
// Array format (preferred):
["/s_new", "beep", 1001, 0, 1]

// Object format (legacy):
{ address: "/s_new", args: ["beep", 1001, 0, 1] }

// Nested bundle:
{ timeTag: ntpTime, packets: [ ["/n_set", 1001, "freq", 880] ] }
```

***

### OscCategory

> **OscCategory** = `"nonBundle"` | `"immediate"` | `"nearFuture"` | `"late"` | `"farFuture"`

Classification category for OSC message routing.

* `'nonBundle'` — plain message (not a bundle), sent directly
* `'immediate'` — bundle with timetag 0 or 1, sent directly
* `'nearFuture'` — bundle within the bypass lookahead threshold, sent directly
* `'late'` — bundle past its scheduled time, sent directly
* `'farFuture'` — bundle beyond the lookahead threshold, routed to the prescheduler

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
// A decoded /n_go message received from scsynth:
["/n_go", 1001, 0, -1, -1, 0]

// Access parts:
const address = msg[0];  // "/n_go"
const args = msg.slice(1);  // [1001, 0, -1, -1, 0]
```

***

### SuperSonicEvent

> **SuperSonicEvent** = keyof [`SuperSonicEventMap`](#supersoniceventmap)

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

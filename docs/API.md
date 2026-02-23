# API Reference

> **TypeScript users:** Full type declarations are in [`supersonic.d.ts`](../supersonic.d.ts) at the project root. Your IDE will pick these up automatically for autocomplete and type checking.

## Quick Start

```javascript
import { SuperSonic } from "https://unpkg.com/supersonic-scsynth@latest";

const sonic = new SuperSonic({
  baseURL: "https://unpkg.com/supersonic-scsynth@latest/dist/",
  synthdefBaseURL: "https://unpkg.com/supersonic-scsynth-synthdefs@latest/synthdefs/",
});

// call init after a user interaction
// such as a button press
myButton.onclick = async () => {
  await sonic.init();
  await sonic.loadSynthDef("sonic-pi-beep");
  sonic.send("/s_new", "sonic-pi-beep", -1, 0, 0, "note", 60);
};
```

## Quick Reference

### Core

| Method                                               | Description                                               |
| ---------------------------------------------------- | --------------------------------------------------------- |
| [`init()`](#init)                                    | Initialise the audio engine                               |
| [`shutdown()`](#shutdown)                            | Shut down, preserving listeners (can call `init()` again) |
| [`destroy()`](#destroy)                              | Permanently destroy instance, clearing all listeners      |
| [`recover()`](#recover)                              | Smart recovery - tries resume, falls back to reload       |
| [`suspend()`](#suspend)                              | Suspend the AudioContext (worklet stays loaded)            |
| [`resume()`](#resume)                                | Resume after suspend, flushes stale messages               |
| [`reload()`](#reload)                                | Full reload - destroys memory, emits `setup` event        |
| [`reset()`](#reset)                                  | Full teardown and re-initialize (loses all state)         |
| [`send(address, ...args)`](#sendaddress-args)        | Send an OSC message                                       |
| [`sendOSC(data, options)`](#sendoscoscbytes-options) | Send pre-encoded OSC bytes                                |
| [`sync(syncId)`](#syncsyncid)                        | Wait for server to process all commands                   |
| [`purge()`](#purge)                            | Flush all pending OSC from prescheduler and WASM scheduler |
| [`cancelAll()`](#cancelallscheduled)        | Cancel all pending events in the JS prescheduler          |

### Asset Loading

| Method                                                           | Description                         |
| ---------------------------------------------------------------- | ----------------------------------- |
| [`loadSynthDef(nameOrPath)`](#loadsynthdefnameorpath)            | Load a synth definition             |
| [`loadSynthDefs(names)`](#loadsynthdefsnames)                    | Load multiple synthdefs in parallel |
| [`loadSample(bufnum, source)`](#loadsamplebufnum-source)         | Load a sample into a buffer         |
| [`sampleInfo(source)`](#sampleinfosource-startframe-numframes)   | Get sample metadata without loading  |

### Events

| Method                                                   | Description                                         |
| -------------------------------------------------------- | --------------------------------------------------- |
| [`on(event, callback)`](#onevent-callback)               | Subscribe to an event, returns unsubscribe function |
| [`off(event, callback)`](#offevent-callback)             | Unsubscribe from an event                           |
| [`once(event, callback)`](#onceevent-callback)           | Subscribe to an event once                          |
| [`removeAllListeners(event?)`](#removealllistenersevent) | Remove all listeners for an event (or all events)   |

### Event Types

| Event                      | Description                                                                                                   |
| -------------------------- | ------------------------------------------------------------------------------------------------------------- |
| `setup`                    | Fires after init/recover, before `ready`. Async handlers are awaited. Use for groups, FX chains, bus routing. |
| `ready`                    | Engine initialised and ready                                                                                  |
| `loading:start`            | Asset loading started (with `{ type, name }` - type is 'wasm', 'synthdef', or 'sample')                       |
| `loading:complete`         | Asset loading completed (with `{ type, name, size }` - size in bytes)                                         |
| `error`                    | Error occurred                                                                                                |
| `message`                  | OSC message received (parsed)                                                                                 |
| `message:raw`              | OSC message received (with raw bytes)                                                                         |
| `message:sent`             | OSC message sent                                                                                              |
| `debug`                    | Debug output from scsynth                                                                                     |
| `shutdown`                 | Engine shutting down (emitted by `shutdown()`, `reset()`, and `destroy()`)                                    |
| `destroy`                  | Engine being permanently destroyed (emitted by `destroy()` only)                                              |
| `audiocontext:statechange` | AudioContext state changed (with `{ state }` payload)                                                         |
| `audiocontext:suspended`   | AudioContext was suspended (browser tab backgrounded, etc.)                                                   |
| `audiocontext:resumed`     | AudioContext resumed running                                                                                  |
| `audiocontext:interrupted` | AudioContext was interrupted (iOS audio session, etc.)                                                        |
| `resumed`                  | Quick resume succeeded (emitted by `resume()`)                                                                |
| `reload:start`             | Full reload starting (emitted by `reload()`)                                                                  |
| `reload:complete`          | Full reload completed (with `{ success }` payload)                                                            |

### Node Tree

| Method                        | Description                                                   |
| ----------------------------- | ------------------------------------------------------------- |
| [`getTree()`](#gettree)       | Get hierarchical tree structure for visualization             |
| [`getRawTree()`](#getrawtree) | Get flat array with internal linkage pointers (for debugging) |

### Metrics

| Method                                              | Description                                  |
| --------------------------------------------------- | -------------------------------------------- |
| [`getMetrics()`](#getmetrics)                       | Get metrics snapshot as an object             |
| [`getMetricsArray()`](#getmetricsarray)             | Get metrics as a flat Uint32Array (zero-alloc)|
| [`SuperSonic.getMetricsSchema()`](#getmetricsschema)| Schema with offsets, layout, and sentinels    |

### Properties

| Property                                        | Description                                              |
| ----------------------------------------------- | -------------------------------------------------------- |
| [`initialized`](#initialized-read-only)         | Whether engine is initialised (read-only)                |
| [`initializing`](#initializing-read-only)       | Whether engine is currently initialising (read-only)     |
| [`audioContext`](#audiocontext-read-only)       | The Web Audio AudioContext (read-only)                   |
| [`node`](#node-read-only)                       | Audio node wrapper for Web Audio connections (read-only) |
| [`loadedSynthDefs`](#loadedsynthdefs-read-only) | Map of loaded synthdef names to binary data (read-only)  |
| [`bootStats`](#bootstats-read-only)             | Boot timing information (read-only)                      |

### Advanced

| Method                                                                    | Description                                    |
| ------------------------------------------------------------------------- | ---------------------------------------------- |
| [`getInfo()`](#getinfo)                                                   | Get static engine configuration                |
| [`SuperSonic.osc.encodeMessage()`](#supersonicoscencodemessage)           | Encode an OSC message to bytes                 |
| [`SuperSonic.osc.encodeBundle()`](#supersonicoscencodebundle)             | Encode an OSC bundle to bytes                  |
| [`SuperSonic.osc.decode()`](#supersonicoscdecodedata-options)             | Decode OSC bytes to a message                  |
| [`SuperSonic.osc.encodeSingleBundle()`](#supersonicoscencodesinglebundle) | Encode a bundle containing a single message    |
| [`SuperSonic.osc.readTimetag()`](#supersonicoscreadtimetag)               | Read NTP timetag from raw bundle bytes         |
| [`SuperSonic.osc.ntpNow()`](#supersonicoscntpnow)                        | Get current time as NTP seconds                |
| [`SuperSonic.osc.NTP_EPOCH_OFFSET`](#supersonicoscntp_epoch_offset)      | Seconds between Unix epoch (1970) and NTP (1900) |

## Creating an Instance

```javascript
import { SuperSonic } from "supersonic-scsynth";

// With explicit baseURL (required)
const supersonic = new SuperSonic({
  baseURL: "/supersonic/",
});
// Derives: workers/, wasm/, synthdefs/, samples/

// Or explicit paths
const supersonic = new SuperSonic({
  workerBaseURL: "/supersonic/workers/",
  wasmBaseURL: "/supersonic/wasm/",
  synthdefBaseURL: "/supersonic/synthdefs/",
  sampleBaseURL: "/supersonic/samples/",
});

// Mix: baseURL with overrides
const supersonic = new SuperSonic({
  baseURL: "/supersonic/",
  sampleBaseURL: "/cdn/samples/", // absolute override
});
```

### Constructor Options

| Option                 | Required | Description                                                                                                                               |
| ---------------------- | -------- | ----------------------------------------------------------------------------------------------------------------------------------------- |
| `baseURL`              | Yes*     | Base URL - derives `workers/`, `wasm/`, `synthdefs/`, `samples/` subdirectories. *Required unless `workerBaseURL` and `wasmBaseURL` are both provided. |
| `workerBaseURL`        | No       | Base URL for worker scripts (overrides baseURL)                                                                                           |
| `wasmBaseURL`          | No       | Base URL for WASM files (overrides baseURL)                                                                                               |
| `wasmUrl`              | No       | Full URL to the WASM file (overrides wasmBaseURL)                                                                                         |
| `workletUrl`           | No       | Full URL to the worklet script (overrides workerBaseURL)                                                                                  |
| `synthdefBaseURL`      | No       | Base URL for synthdef files (used by `loadSynthDef`)                                                                                      |
| `sampleBaseURL`        | No       | Base URL for sample files (used by `loadSample`)                                                                                          |
| `mode`                 | No       | Transport mode: `'postMessage'` (default) or `'sab'`. See [Communication Modes](MODES.md). |
| `audioContext`         | No       | Use an existing AudioContext instead of creating one                                                                                      |
| `audioContextOptions`  | No       | Options passed to `new AudioContext()` (see below)                                                                                        |
| `autoConnect`          | No       | Whether to auto-connect to `audioContext.destination` (default: true)                                                                     |
| `scsynthOptions`       | No       | Server options (see below)                                                                                                                |
| `snapshotIntervalMs`   | No       | Metrics snapshot interval for postMessage mode (default: 150)                                                                             |
| `preschedulerCapacity` | No       | Max pending events in JS prescheduler (default: 65536)                                                                                    |
| `bypassLookaheadMs`    | No       | Bundles within this many ms of now bypass the prescheduler and are sent directly (default: 500)                                           |
| `fetchMaxRetries`      | No       | Max retries for asset fetches (default: 3)                                                                                                |
| `fetchRetryDelay`      | No       | Base delay in ms between fetch retries (default: 1000)                                                                                    |
| `activityEvent`        | No       | Event emission truncation options (see below)                                                                                             |
| `debug`                | No       | Log all debug messages to console (scsynth, OSC in, OSC out)                                                                              |
| `debugScsynth`         | No       | Log scsynth debug messages to console                                                                                                     |
| `debugOscIn`           | No       | Log incoming OSC messages to console                                                                                                      |
| `debugOscOut`          | No       | Log outgoing OSC messages to console                                                                                                      |
| `activityConsoleLog`   | No       | Console output truncation options (see below)                                                                                             |

**Note:** You must provide either `baseURL` or both `workerBaseURL` and `wasmBaseURL`. For CDN usage, set `baseURL` to the CDN path (e.g., `https://unpkg.com/supersonic-scsynth@latest/dist/`). For self-hosted usage, set it to your local path (e.g., `/supersonic/` or `./`).

### Activity Event Options (`activityEvent`)

Control truncation of event emission for custom log UIs. The `maxLineLength` is the default; specific overrides take precedence when set.

```javascript
const supersonic = new SuperSonic({
  baseURL: "/supersonic/",
  activityEvent: {
    maxLineLength: 200,            // Default for all (default: 200)
    scsynthMaxLineLength: 500,     // Override for scsynth messages
    oscInMaxLineLength: 100,       // Override for incoming OSC
    oscOutMaxLineLength: 100,      // Override for outgoing OSC
  },
});
```

| Option                 | Description                                                        |
| ---------------------- | ------------------------------------------------------------------ |
| `maxLineLength`        | Default max chars for event emission (default: 200)                |
| `scsynthMaxLineLength` | Override for scsynth debug events (falls back to `maxLineLength`)  |
| `oscInMaxLineLength`   | Override for incoming OSC args (falls back to `maxLineLength`)     |
| `oscOutMaxLineLength`  | Override for outgoing OSC args (falls back to `maxLineLength`)     |

### Activity Console Log Options (`activityConsoleLog`)

Control truncation of console debug output. The `maxLineLength` is the default; specific overrides take precedence when set.

```javascript
const supersonic = new SuperSonic({
  baseURL: "/supersonic/",
  debug: true,
  activityConsoleLog: {
    maxLineLength: 200,            // Default for all (default: 200)
    scsynthMaxLineLength: 500,     // Override for scsynth messages
    oscInMaxLineLength: 100,       // Override for incoming OSC
    oscOutMaxLineLength: 100,      // Override for outgoing OSC
  },
});
```

| Option                 | Description                                                              |
| ---------------------- | ------------------------------------------------------------------------ |
| `maxLineLength`        | Default max chars for all console output (default: 200)                  |
| `scsynthMaxLineLength` | Override for scsynth messages (falls back to `maxLineLength`)            |
| `oscInMaxLineLength`   | Override for incoming OSC args (falls back to `maxLineLength`)           |
| `oscOutMaxLineLength`  | Override for outgoing OSC args (falls back to `maxLineLength`)           |

### AudioContext Options (`audioContextOptions`)

Options passed to the [AudioContext constructor](https://developer.mozilla.org/en-US/docs/Web/API/AudioContext/AudioContext):

```javascript
const supersonic = new SuperSonic({
  audioContextOptions: {
    sampleRate: 44100,            // Desired sample rate (default: 48000)
    latencyHint: "playback",      // "interactive" (default), "balanced", "playback", or seconds
  },
});
```

| Option        | Type             | Description                                                                 |
| ------------- | ---------------- | --------------------------------------------------------------------------- |
| `sampleRate`  | number           | Desired sample rate in Hz (default: 48000)                                  |
| `latencyHint` | string \| number | `"interactive"` (default), `"balanced"`, `"playback"`, or seconds as number |

**Note:** The actual sample rate depends on hardware support. Use `getInfo().sampleRate` after init to check the actual rate.

### Server Options (`scsynthOptions`)

Override scsynth server defaults:

```javascript
const supersonic = new SuperSonic({
  scsynthOptions: {
    numBuffers: 4096,
    numOutputBusChannels: 4,
  },
});
```

| Option                  | Default | Range       | Description                                                                                                          |
|-------------------------|---------|-------------|----------------------------------------------------------------------------------------------------------------------|
| `numBuffers`            | 1024    | 1-65535     | Maximum audio buffers (SndBuf slots). Each slot has ~104 bytes overhead; actual audio data is stored separately.      |
| `maxNodes`              | 1024    | 1+          | Maximum synthesis nodes (synths + groups).                                                                            |
| `maxGraphDefs`          | 1024    | 1+          | Maximum loaded SynthDef definitions.                                                                                  |
| `maxWireBufs`           | 64      | 1+          | Wire buffers for internal UGen connections. Each uses `bufLength * 8` bytes.                                          |
| `numAudioBusChannels`   | 128     | 1+          | Audio bus channels for real-time routing between synths.                                                               |
| `numInputBusChannels`   | 2       | 0+          | Input bus channels from hardware. Actual channels used is `min(configured, hardware)`.                                |
| `numOutputBusChannels`  | 2       | 1-128       | Output bus channels to hardware. Use values > 2 for surround or multi-output audio interfaces. When > 2 and `autoConnect` is true, sets `destination.channelInterpretation` to `'discrete'` to prevent automatic mixing. |
| `numControlBusChannels` | 4096    | 1+          | Control bus channels for control-rate data sharing between synths.                                                     |
| `bufLength`             | 128     | 128 (fixed) | Audio buffer length in samples. Fixed by the WebAudio API — cannot be changed.                                        |
| `realTimeMemorySize`    | 8192    | 1+          | Real-time memory pool in KB for synthesis-time allocations (UGen memory, etc.).                                        |
| `numRGens`              | 64      | 1+          | Random number generators. Each synth can use its own RNG for reproducible randomness.                                 |
| `preferredSampleRate`   | 0       | 0, 8000-384000 | Preferred sample rate. `0` uses the AudioContext default (typically 48000).                                         |
| `verbosity`             | 0       | 0-4         | Debug verbosity. 0 = quiet, 1 = errors, 2 = warnings, 3 = info, 4 = debug.                                           |

## Core Methods

### `init()`

Initialise the audio engine. Call this after a user interaction (e.g., button click) due to browser autoplay policies.

```javascript
await supersonic.init();
```

All configuration is passed to the [constructor](#constructor-options). Calling `init()` multiple times is safe - it returns immediately if already initialised, or returns the existing promise if initialisation is in progress.

### `loadSynthDef(nameOrPath)`

Load a synthdef. Pass a name to use `synthdefBaseURL`, or provide a full path.

**Returns:** `Promise<{name: string, size: number}>` - The synthdef name and size in bytes.

```javascript
// By name (uses synthdefBaseURL)
const info = await supersonic.loadSynthDef("sonic-pi-beep");
console.log(`Loaded ${info.name} (${info.size} bytes)`);

// By full path
await supersonic.loadSynthDef("./custom/my-synth.scsyndef");
```

### `loadSynthDefs(names)`

Load multiple synthdefs in parallel.

**Returns:** `Promise<Object>` - A map of synthdef name to result object. Each result contains either `{success: true}` or `{success: false, error: string}`.

```javascript
const results = await supersonic.loadSynthDefs(["sonic-pi-beep", "sonic-pi-prophet"]);

// Check results
for (const [name, result] of Object.entries(results)) {
  if (result.success) {
    console.log(`Loaded ${name}`);
  } else {
    console.error(`Failed to load ${name}: ${result.error}`);
  }
}
```

### `loadSample(bufnum, source, startFrame, numFrames)`

Load a sample into a buffer. Accepts multiple source types:

- **String** - filename (uses `sampleBaseURL`) or full path/URL
- **File/Blob** - browser File object from `<input type="file">`
- **ArrayBuffer/TypedArray** - raw audio data

**Parameters:**

- `bufnum` - Buffer number (integer)
- `source` - Sample source (string, File, Blob, ArrayBuffer, or TypedArray)
- `startFrame` - Optional starting frame offset (integer, default: 0)
- `numFrames` - Optional number of frames to load (integer, default: 0 = all frames)

```javascript
// By name (uses sampleBaseURL)
await supersonic.loadSample(0, "loop_amen.flac");

// By full path/URL
await supersonic.loadSample(0, "./custom/my-sample.wav");

// From user-selected file
const file = document.querySelector('input[type="file"]').files[0];
await supersonic.loadSample(0, file);

// From ArrayBuffer (e.g., fetched manually)
const response = await fetch("./audio/sample.wav");
const arrayBuffer = await response.arrayBuffer();
await supersonic.loadSample(0, arrayBuffer);

// Load partial sample (frames 1000-2000)
await supersonic.loadSample(0, "long-sample.flac", 1000, 1000);
```

**Returns:** `Promise<{bufnum, hash, source, numFrames, numChannels, sampleRate, duration}>` — Buffer slot number and decoded audio metadata. The hash is a SHA-256 hex string — deterministic for the same audio content regardless of how it was loaded.

```javascript
const result = await supersonic.loadSample(0, "kick.wav");
console.log(result.hash);       // "a3f2b1c4..." (64-char hex string)
console.log(result.duration);   // 0.92
console.log(result.numChannels); // 2
```

### `sampleInfo(source, startFrame, numFrames)`

Get sample metadata without allocating a buffer. Fetches, decodes, and analyses the audio, returning the same info that would appear in the `loadSample` result. No buffer slot is consumed and no OSC is sent to scsynth.

Use this to inspect audio content or check for duplicates before loading.

**Parameters:**

- `source` - Sample source (string, File, Blob, ArrayBuffer, or TypedArray)
- `startFrame` - Optional starting frame offset (default: 0)
- `numFrames` - Optional number of frames (default: 0 = all)

**Returns:** `Promise<{hash, source, numFrames, numChannels, sampleRate, duration}>`

```javascript
// Inspect before loading
const info = await supersonic.sampleInfo("kick.wav");
console.log(info.duration, info.numChannels, info.sampleRate);

// Check for duplicates
const loaded = supersonic.getLoadedBuffers();
if (loaded.some(b => b.hash === info.hash)) {
  console.log("Already loaded, skipping");
} else {
  await supersonic.loadSample(nextBufnum, "kick.wav");
}
```

### `send(address, ...args)`

Send an OSC message. Types are auto-detected from JavaScript values. Use `{ type, value }` wrappers to force a specific OSC type (e.g. `{ type: 'float', value: 440 }` to send a whole number as float32). See [`encodeMessage()`](#supersonicoscencodemessage) for the full type reference.

Most commands are sent synchronously. Buffer allocation commands (`/b_alloc`, `/b_allocRead`, `/b_allocReadChannel`, `/b_allocFile`) are queued and processed in the background because they involve network fetches and audio decoding. Use `sync()` after buffer commands to ensure they complete before using the buffer.

```javascript
supersonic.send("/s_new", "sonic-pi-beep", -1, 0, 0, "note", 60, "amp", 0.5);
supersonic.send("/n_free", 1000);

// Buffer commands are processed in the background
supersonic.send("/b_alloc", 0, 44100, 1);
await supersonic.sync(); // waits for /b_alloc to complete
```

### `sendOSC(oscBytes, options)`

Send pre-encoded OSC bytes. Useful if you're building OSC messages yourself. Sends bytes as-is without rewriting — use `send()` for buffer allocation commands so they get rewritten to `/b_allocPtr`.

```javascript
const oscData = new Uint8Array([...]); // Your OSC bytes
supersonic.sendOSC(oscData);
```

### `sync(syncId)`

Send a `/sync` command and wait for the `/synced` response. Use this to ensure all previous asynchronous commands (like `/d_recv` for synthdefs) have been processed by the server.

**Parameters:**

- `syncId` - Optional integer identifier (default: random). The server echoes this back in the `/synced` response.

**Returns:** `Promise<void>` - Resolves when the server responds, or rejects after 10 seconds.

```javascript
// Load synthdefs then wait for them to be ready
await supersonic.loadSynthDefs(["sonic-pi-beep", "sonic-pi-prophet"]);
await supersonic.sync(); // Now safe to use the synthdefs

// Use a specific sync ID for tracking
await supersonic.sync(42);
```

### `purge()`

Flush all pending OSC messages from both the JS prescheduler and the WASM BundleScheduler. Returns a promise that resolves when both sides have confirmed the flush is complete.

Unlike `cancelAll()` which only clears the JS prescheduler, this also clears bundles that have already been consumed from the ring buffer and are sitting in the WASM scheduler's priority queue. Uses a postMessage flag (not the ring buffer) to avoid the race condition where stale scheduled bundles would fire before a clear command could be read.

Called internally by `resume()` to prevent stale messages from a previous session interfering with new work.

**Returns:** `Promise<void>`

```javascript
// Clear everything before starting a new run
await supersonic.purge();

// Safe to send new events — pipeline is confirmed empty
supersonic.send("/s_new", "sonic-pi-beep", -1, 0, 0, "note", 60);
```

### `cancelAll()`

Cancel all pending events in the JS prescheduler. This clears future-timestamped bundles that haven't yet been sent to the worklet. Does **not** clear bundles already in the WASM scheduler — use [`purge()`](#purge) for that.

```javascript
supersonic.cancelAll();
```

### `shutdown()`

Shut down the engine, releasing all resources but preserving event listeners. After shutdown, you can call `init()` again to restart. Emits the `shutdown` event before teardown begins.

**Returns:** `Promise<void>`

```javascript
await supersonic.shutdown();
// Engine is stopped, but listeners are preserved

// Later, restart the engine
await supersonic.init();
```

### `destroy()`

Permanently destroy the instance, releasing all resources AND clearing all event listeners. After destroy, the instance cannot be reused - create a new SuperSonic instance instead. Emits `destroy` event, then `shutdown` event, then clears all listeners.

**Returns:** `Promise<void>`

```javascript
await supersonic.destroy();
// Instance is now unusable, no memory leaks
```

### `recover()`

Smart recovery - tries `resume()` first, falls back to `reload()` if the worklet was killed. Use this when you don't know which recovery method is needed.

**Returns:** `Promise<boolean>` - true if audio is running after recovery

```javascript
// Handle visibility change (e.g., user switches back to tab)
document.addEventListener("visibilitychange", async () => {
  if (!document.hidden) {
    await supersonic.recover();
  }
});
```

### `suspend()`

Suspend the AudioContext. The worklet remains loaded but audio processing stops. Use this to reduce CPU usage when audio is not needed (e.g., nothing is playing).

The `audiocontext:suspended` event is emitted automatically by the AudioContext state change listener.

**Returns:** `Promise<void>`

```javascript
// Suspend when idle
await supersonic.suspend();
// CPU drops to near-zero, worklet and memory preserved

// Later, resume
await supersonic.resume();
```

### `resume()`

Resume after a suspend. Calls [`purge()`](#purge) internally to clear any stale scheduled messages from before the suspend, then resumes the AudioContext and resyncs timing. Memory and node tree are preserved. Does **not** emit `setup` event.

Use when you know the worklet is still running (e.g., tab was briefly backgrounded, or after a manual `suspend()`).

**Returns:** `Promise<boolean>` - true if worklet is running after resume

**Events:** Emits `resumed` on success.

```javascript
// Try quick resume first
if (await supersonic.resume()) {
  console.log("Quick resume worked, nodes preserved");
} else {
  console.log("Worklet was killed, need full reload");
  await supersonic.reload();
}
```

### `reload()`

Full reload - destroys and recreates the worklet/WASM, then restores synthdefs and buffers from cache. Emits `setup` event so you can rebuild groups, FX chains, and bus routing.

Use when the worklet was killed (e.g., long background, browser reclaimed memory).

**Returns:** `Promise<boolean>` - true if reload succeeded

**Events:** Emits `reload:start`, then `setup`, then `reload:complete`.

```javascript
// Force full reload (e.g., after known memory issue)
await supersonic.reload();
// 'setup' event handlers have run, groups/FX rebuilt
```

### `reset()`

Full teardown and re-initialize. Use this when you need a completely fresh state. Event listeners are preserved across reset, but all other state (synthdefs, buffers) is lost.

**Returns:** `Promise<void>`

```javascript
// Need a completely fresh start
await supersonic.reset();
// Engine is now re-initialized and ready to use
```

## Events

Subscribe to events with `on()`, which returns an unsubscribe function for easy cleanup.

### `on(event, callback)`

```javascript
// Subscribe
const unsubscribe = supersonic.on("message", (msg) => {
  console.log("Received:", msg[0]);  // address
});

// Later, unsubscribe
unsubscribe();
```

Multiple listeners can subscribe to the same event - each receives all events independently.

```javascript
// Both listeners receive all messages
supersonic.on("message", (msg) => console.log("Listener A:", msg[0]));
supersonic.on("message", (msg) => console.log("Listener B:", msg[0]));
```

### `off(event, callback)`

Unsubscribe using the original callback reference. Alternative to using the unsubscribe function.

```javascript
const handler = (msg) => console.log(msg);
supersonic.on("message", handler);

// Later
supersonic.off("message", handler);
```

### `once(event, callback)`

Subscribe to an event once. The listener auto-unsubscribes after the first event. Returns an unsubscribe function (matching `on()`).

```javascript
supersonic.once("ready", ({ bootStats }) => {
  console.log("Engine booted in", bootStats.initDuration.toFixed(2), "ms");
});

// Or cancel before it fires:
const unsub = supersonic.once("message", handler);
unsub(); // never fires
```

### Event: `setup`

Emitted after init/recover completes, before `ready`. Async handlers are awaited. Use this for any persistent audio infrastructure that needs to exist on both initial boot and after recovery:

- **Groups** - Node tree organization
- **FX chains** - Reverb, filters, compressors
- **Bus routing** - Synths that read/write to audio buses
- **Persistent synths** - Always-on nodes like analyzers or mixers

```javascript
supersonic.on("setup", async () => {
  // Create group structure
  supersonic.send("/g_new", 100, 0, 0); // synths group
  supersonic.send("/g_new", 101, 1, 0); // fx group (after synths)

  // Create FX chain
  supersonic.send("/s_new", "sonic-pi-fx_reverb", 2000, 0, 101, "in_bus", 20, "out_bus", 0, "mix", 0.3);

  await supersonic.sync();
});
```

**Why use `setup` instead of `ready`?**

In `postMessage` mode, `recover()` destroys and recreates the WASM memory, so all nodes are lost. The `setup` event lets you rebuild consistently on both initial boot and after recovery. In `sab` mode, memory persists across recovery so this is less critical - but using `setup` keeps your code portable.

### Event: `ready`

Emitted when the engine is initialised and ready to use.

```javascript
supersonic.on("ready", ({ capabilities, bootStats }) => {
  console.log("Capabilities:", capabilities);
  console.log("Boot time:", bootStats.initDuration.toFixed(2), "ms");
});
```

### Event: `error`

Emitted when an error occurs.

```javascript
supersonic.on("error", (error) => {
  console.error("SuperSonic error:", error.message);
});
```

### Event: `message`

Emitted when a parsed OSC message is received from scsynth.

```javascript
supersonic.on("message", (msg) => {
  // msg is [address, ...args]
  console.log("Address:", msg[0]);
  console.log("Args:", msg.slice(1));
});
```

### Event: `message:raw`

Emitted with raw OSC data including the original bytes and timing information. Useful for logging and latency analysis.

```javascript
supersonic.on("message:raw", ({ oscData, sequence, timestamp, scheduledTime }) => {
  const parsed = SuperSonic.osc.decode(oscData);
  const relativeTime = (timestamp - supersonic.initTime).toFixed(2);
  console.log(`[${sequence}] +${relativeTime}s`, parsed[0], parsed.slice(1));
  if (scheduledTime && timestamp > scheduledTime) {
    console.warn("Late by", (timestamp - scheduledTime).toFixed(4), "s");
  }
});
```

- `timestamp` — NTP seconds when the message was observed
- `scheduledTime` — NTP seconds from bundle timetag, or `null` if not a bundle

### Event: `message:sent`

Emitted when an OSC message is sent to scsynth. Receives an object with the raw OSC data, source channel ID (0 = main thread, 1+ = workers), sequence number, and timing information.

```javascript
supersonic.on("message:sent", ({ oscData, sourceId, sequence, timestamp, scheduledTime }) => {
  const decoded = SuperSonic.osc.decode(oscData);
  const relativeTime = (timestamp - supersonic.initTime).toFixed(2);
  console.log(`[${sequence}] +${relativeTime}s [src:${sourceId}]`, decoded[0]);
});
```

- `timestamp` — NTP seconds when the message was written to the ring buffer
- `scheduledTime` — NTP seconds from bundle timetag, or `null` if not a bundle

### Event: `debug`

Emitted with debug output from scsynth.

```javascript
supersonic.on("debug", (msg) => {
  console.log("[scsynth]", msg.text);
});
```

### Event: `shutdown`

Emitted when the engine is shutting down. Fired by `shutdown()`, `reset()`, and `destroy()`. Use this to clean up application state that depends on SuperSonic.

```javascript
supersonic.on("shutdown", () => {
  console.log("Engine shutting down, cleaning up...");
  // Reset application state flags, stop loops, etc.
});
```

### Event: `destroy`

Emitted when the engine is being permanently destroyed (only fired by `destroy()`, not by `shutdown()` or `reset()`). This is your last chance to clean up before all listeners are cleared.

```javascript
supersonic.on("destroy", () => {
  console.log("Engine being destroyed permanently");
  // Final cleanup before instance becomes unusable
});
```

### Event: `audiocontext:statechange`

Emitted when the AudioContext state changes. The payload contains the new state.

```javascript
supersonic.on("audiocontext:statechange", ({ state }) => {
  console.log("AudioContext state:", state);
  // state is one of: 'running', 'suspended', 'interrupted', 'closed'
});
```

### Event: `audiocontext:suspended`

Emitted when the AudioContext is suspended. This typically happens when the browser tab is backgrounded or the system suspends audio. Use this to show a "restart" UI to the user.

```javascript
supersonic.on("audiocontext:suspended", () => {
  console.log("Audio suspended - show restart button");
  showRestartUI();
});
```

### Event: `audiocontext:resumed`

Emitted when the AudioContext resumes running after being suspended.

```javascript
supersonic.on("audiocontext:resumed", () => {
  console.log("Audio resumed");
  hideRestartUI();
});
```

### Event: `audiocontext:interrupted`

Emitted when the AudioContext is interrupted by the system (common on iOS when another app takes audio focus). Similar to `suspended` but triggered externally.

```javascript
supersonic.on("audiocontext:interrupted", () => {
  console.log("Audio interrupted by system");
  showRestartUI();
});
```

### `removeAllListeners(event?)`

Remove all listeners for an event, or all listeners entirely. Useful for cleanup.

```javascript
// Remove all 'message' listeners
supersonic.removeAllListeners("message");

// Remove ALL listeners (use with caution)
supersonic.removeAllListeners();
```

## Properties

### `initialized` (read-only)

Whether the engine has been initialised.

```javascript
if (supersonic.initialized) {
  supersonic.send('/s_new', ...);
}
```

### `initializing` (read-only)

Whether the engine is currently initialising. Useful for showing loading states in your UI.

```javascript
if (supersonic.initializing) {
  console.log("Engine is booting...");
} else if (supersonic.initialized) {
  console.log("Engine is ready");
} else {
  console.log("Engine not started");
}
```

### `audioContext` (read-only)

The underlying Web Audio AudioContext.

```javascript
const ctx = supersonic.audioContext;
console.log("Sample rate:", ctx.sampleRate);
```

### `initTime` (read-only)

NTP time (seconds since 1900) when the AudioContext started. Use to compute relative times from event timestamps.

```javascript
supersonic.on("message:raw", ({ timestamp }) => {
  const relativeSeconds = timestamp - supersonic.initTime;
  console.log(`+${relativeSeconds.toFixed(2)}s`);
});
```

### `node` (read-only)

A wrapper around the AudioWorkletNode that provides a clean interface for Web Audio connections.

**Properties:**

| Property          | Description                                         |
| ----------------- | --------------------------------------------------- |
| `input`           | The AudioWorkletNode to connect external sources to |
| `context`         | The AudioContext (same as `audioContext`)           |
| `numberOfInputs`  | Number of input channels (from scsynth config)      |
| `numberOfOutputs` | Number of output channels (from scsynth config)     |
| `channelCount`    | Channel count of the worklet node                   |

**Methods:**

| Method                     | Description                                           |
| -------------------------- | ----------------------------------------------------- |
| `connect(destination)`     | Connect SuperSonic's output to another AudioNode      |
| `disconnect(destination?)` | Disconnect from a destination (or all if no argument) |

**Connecting external audio sources (e.g., microphone):**

```javascript
// Get mic stream
const stream = await navigator.mediaDevices.getUserMedia({ audio: true });
const micSource = audioContext.createMediaStreamSource(stream);

// Connect mic to SuperSonic input
micSource.connect(supersonic.node.input);

// Audio flows through scsynth's input buses (bus 2+ by default)
// Use In.ar(2) in a synthdef to read the mic signal
```

**Connecting SuperSonic output to other nodes:**

```javascript
// Create an analyser for visualization
const analyser = supersonic.audioContext.createAnalyser();

// Connect SuperSonic output to analyser (in addition to speakers)
supersonic.node.connect(analyser);

// Read frequency data
const data = new Uint8Array(analyser.frequencyBinCount);
analyser.getByteFrequencyData(data);
```

**Note:** The `input` property exposes the raw AudioWorkletNode for connecting external sources.

### `loadedSynthDefs` (read-only)

A `Map` of synthdef names to their binary data. A synthdef appears here after it's been sent to scsynth via `/d_recv` or `loadSynthDef()`. Removed when freed via `/d_free` or `/d_freeAll`. The binary data is cached so synthdefs can be restored after a `reload()`.

```javascript
if (supersonic.loadedSynthDefs.has("sonic-pi-beep")) {
  supersonic.send("/s_new", "sonic-pi-beep", -1, 0, 0);
}

// See all loaded synthdef names
console.log("Loaded:", [...supersonic.loadedSynthDefs.keys()]);
```

### `bootStats` (read-only)

Timing information from engine initialisation.

```javascript
const stats = supersonic.bootStats;
console.log(`Engine booted in ${stats.initDuration.toFixed(2)}ms`);
```

**Properties:**

- `initStartTime` - When `init()` was called (`performance.now()` timestamp)
- `initDuration` - How long initialisation took (milliseconds)

## Node Tree API

The node tree gives you a live view of all running synths and groups - useful for building visualizations that update at 60fps without any OSC round-trip latency.

### `getTree()`

Returns a hierarchical snapshot of the scsynth node tree - all synths and groups currently running, organized as a nested tree structure.

```javascript
const tree = supersonic.getTree();
```

**Returns:**

```javascript
{
  version: 42,          // Increments on every change
  nodeCount: 5,         // Nodes in mirror
  droppedCount: 0,      // Nodes not mirrored due to overflow
  root: {
    id: 0,
    type: "group",      // "group" or "synth"
    defName: "",
    children: [
      {
        id: 100,
        type: "synth",
        defName: "sonic-pi-beep",
        children: []
      },
      {
        id: 101,
        type: "group",
        defName: "",
        children: [
          {
            id: 200,
            type: "synth",
            defName: "sonic-pi-fx_reverb",
            children: []
          }
        ]
      }
    ]
  }
}
```

**Polling for changes:**

Use `version` to skip re-renders when nothing changed:

```javascript
let lastVersion = 0;

function animate() {
  const tree = supersonic.getTree();
  if (tree.version !== lastVersion) {
    lastVersion = tree.version;
    updateVisualization(tree.root);
  }
  requestAnimationFrame(animate);
}
animate();
```

**Example: List all running synths**

```javascript
function collectSynths(node) {
  const synths = [];
  if (node.type === 'synth') synths.push(node);
  for (const child of node.children) {
    synths.push(...collectSynths(child));
  }
  return synths;
}

const tree = supersonic.getTree();
for (const synth of collectSynths(tree.root)) {
  console.log(`Synth ${synth.id}: ${synth.defName}`);
}
```

**`getTree()` vs `getRawTree()` vs `/g_queryTree`**

|                | `getTree()`                      | `getRawTree()`                 | `/g_queryTree`             |
| -------------- | -------------------------------- | ------------------------------ | -------------------------- |
| Latency        | Instant (reads shared memory)    | Instant (reads shared memory)  | ~40ms round-trip           |
| Format         | Hierarchical (nested children)   | Flat array with link pointers  | Nested in message args     |
| Control values | Not included                     | Not included                   | Optional (flag=1)          |
| Use case       | Tree visualization, UI rendering | Debugging, low-level access    | One-off queries, debugging |

`getTree()` returns node structure only - not control values. For control values, use `/g_queryTree` with flag=1 or `/n_get` for specific nodes. See [scsynth Command Reference](SCSYNTH_COMMAND_REFERENCE.md).

**Mirror capacity and overflow**

The node tree mirror has a capacity of 1024 nodes by default. If the actual scsynth node tree grows beyond this limit, excess nodes won't be visible to JavaScript but audio continues working normally. The `droppedCount` field indicates how many nodes were not mirrored due to this overflow - if it's greater than zero, you're seeing a partial view of the tree.

## Metrics API

For monitoring performance and debugging. See [Metrics](METRICS.md) for the full list of available metrics, or use the [`<supersonic-metrics>` web component](METRICS_COMPONENT.md) for a ready-made UI.

### `getMetrics()`

Get a metrics snapshot as a JavaScript object. This is a cheap local memory read in both SAB and postMessage modes - no IPC or copying occurs. Safe to call from `requestAnimationFrame` or high-frequency timers.

```javascript
const metrics = supersonic.getMetrics();
console.log("Messages processed:", metrics.scsynthMessagesProcessed);
```

### `getMetricsArray()`

Get metrics as a flat `Uint32Array`. Returns the same array reference every call — values are updated in-place, making this zero-allocation. Use `getMetricsSchema().metrics` for the offset of each metric.

```javascript
const arr = supersonic.getMetricsArray();
const schema = SuperSonic.getMetricsSchema();
console.log("Processed:", arr[schema.metrics.scsynthMessagesProcessed.offset]);
```

This is what the `<supersonic-metrics>` web component uses internally for its delta-diffed rendering loop.

### `SuperSonic.getMetricsSchema()` (static) {#getmetricsschema}

Returns the schema describing all metrics, their array offsets, UI layout, and sentinel values.

```javascript
const schema = SuperSonic.getMetricsSchema();
// schema.metrics   — { key: { offset, type, unit, description }, ... }
// schema.layout    — { panels: [...] } for rendering
// schema.sentinels — { HEADROOM_UNSET: 0xFFFFFFFF }
```

See [Metrics Component](METRICS_COMPONENT.md) for full schema documentation.

## Advanced

### `getInfo()`

Returns static configuration from boot time - things that don't change after initialisation.

```javascript
const info = supersonic.getInfo();
console.log("Sample rate:", info.sampleRate);
console.log("Boot time:", info.bootTimeMs, "ms");
```

Returns an object containing:

- `sampleRate` - Audio sample rate (e.g., 48000)
- `numBuffers` - Maximum number of audio buffers
- `totalMemory` - Total memory allocated (bytes)
- `wasmHeapSize` - WASM heap size (bytes)
- `bufferPoolSize` - Buffer pool size (bytes)
- `bootTimeMs` - Engine initialisation time (ms)
- `capabilities` - Browser capabilities object
- `version` - Engine version string (may be null if not yet received)

### `SuperSonic.osc.encodeMessage(address, args)`

Encode an OSC message into binary format. Types are inferred automatically from JavaScript values. Strings are encoded as UTF-8.

```javascript
const bytes = SuperSonic.osc.encodeMessage("/s_new", [
  "sonic-pi-beep",  // string
  -1,               // integer
  0,                // integer
  0,                // integer
]);
// bytes is a Uint8Array
```

**Auto-detected types:**

| JavaScript value | OSC type |
|---|---|
| Integer number (e.g. `42`) | int32 |
| Non-integer number (e.g. `0.5`) | float32 |
| String | string (UTF-8) |
| `true` / `false` | bool |
| `Uint8Array` / `ArrayBuffer` | blob |

**Tagged type wrappers** — use `{ type, value }` objects to force a specific OSC type:

| Tagged wrapper | OSC type | Use case |
|---|---|---|
| `{ type: 'float', value: 440 }` | float32 | Force float for whole numbers |
| `{ type: 'int', value: 42 }` | int32 | Explicit integer |
| `{ type: 'string', value: 'hello' }` | string | Explicit string |
| `{ type: 'blob', value: bytes }` | blob | Explicit blob |
| `{ type: 'bool', value: true }` | bool | Explicit boolean |
| `{ type: 'double', value: 3.14159 }` | float64 | Double precision |
| `{ type: 'int64', value: 9007199254740992n }` | int64 | 64-bit integer |
| `{ type: 'timetag', value: ntpTime }` | timetag | NTP timestamp as argument |

```javascript
// Force 440 as float32 (without the wrapper, whole numbers encode as int32)
SuperSonic.osc.encodeMessage("/n_set", [1001, "freq", { type: 'float', value: 440 }]);
```

### `SuperSonic.osc.encodeBundle(timeTag, packets)`

Encode an OSC bundle with a timestamp and multiple messages.

**TimeTag formats:**

| Value | Meaning |
|---|---|
| `1`, `null`, `undefined` | Execute immediately |
| `[seconds, fraction]` | NTP uint32 pair — preserves full 64-bit precision |
| Number (e.g. `3913056000.5`) | NTP float — seconds since 1900 (fractional part encoded) |

A `[seconds, fraction]` array must have exactly 2 elements (throws `Error` otherwise). Non-number / non-array values throw `TypeError`. Numbers between 1 and `NTP_EPOCH_OFFSET` trigger a `console.warn` because they look like Unix timestamps.

Each packet is an array: `[address, ...args]`.

```javascript
// Immediate
SuperSonic.osc.encodeBundle(1, [["/notify", 1]]);

// NTP float
const ntpTime = SuperSonic.osc.ntpNow();
SuperSonic.osc.encodeBundle(ntpTime, [
  ["/s_new", "sonic-pi-beep", 1001, 0, 0],
]);

// NTP uint32 pair (full precision, no float loss)
SuperSonic.osc.encodeBundle([3913056000, 2147483648], [
  ["/s_new", "sonic-pi-beep", 1001, 0, 0],
  ["/n_set", 1001, "amp", 0.5],
]);
```

### `SuperSonic.osc.decode(data)`

Decode binary OSC data. Messages decode to `[address, ...args]` arrays. Bundles decode to `{ timeTag, packets }` objects where each packet is itself decoded.

**Parameters:**

- `data` - `Uint8Array` or `ArrayBuffer` containing OSC data

```javascript
// Messages
const msg = SuperSonic.osc.decode(oscBytes);
// msg = ["/s_new", "sonic-pi-beep", 1001, 0, 0]
console.log(msg[0], msg.slice(1));  // address, args

// Bundles
const bundle = SuperSonic.osc.decode(bundleBytes);
// bundle = { timeTag: 3913056000.5, packets: [["/s_new", ...], ["/n_set", ...]] }
```

### `SuperSonic.osc.encodeSingleBundle(timeTag, address, args)`

Encode a bundle containing a single message. This is an optimised path that avoids wrapping the message in an array.

**Parameters:**

- `timeTag` - TimeTag in any accepted format (see [`encodeBundle`](#supersonicoscencodebundletimetag-packets))
- `address` - OSC address string (e.g. `"/s_new"`)
- `args` - Array of arguments

```javascript
const bytes = SuperSonic.osc.encodeSingleBundle(
  [sec, frac],
  "/s_new",
  ["sonic-pi-beep", 1001, 0, 0]
);
```

### `SuperSonic.osc.readTimetag(bundleData)`

Read the NTP timetag from raw bundle bytes without decoding the entire packet. Returns `{ ntpSeconds, ntpFraction }` or `null` if data is too short.

**Parameters:**

- `bundleData` - `Uint8Array` of at least 16 bytes (the `#bundle\0` header + 8-byte timetag)

```javascript
const bytes = SuperSonic.osc.encodeBundle([sec, frac], packets);
const { ntpSeconds, ntpFraction } = SuperSonic.osc.readTimetag(bytes);
```

### `SuperSonic.osc.ntpNow()`

Get the current time as NTP seconds (seconds since 1900-01-01). Derived from `performance.timeOrigin + performance.now()`.

```javascript
const ntpTime = SuperSonic.osc.ntpNow();
SuperSonic.osc.encodeBundle(ntpTime + 0.5, [/* ... */]);
```

### `SuperSonic.osc.NTP_EPOCH_OFFSET`

Constant: seconds between the Unix epoch (1970) and the NTP epoch (1900). Value: `2208988800`.

```javascript
const unixSeconds = Date.now() / 1000;
const ntpSeconds = unixSeconds + SuperSonic.osc.NTP_EPOCH_OFFSET;
```

### `createOscChannel(options?)`

Create an `OscChannel` for sending OSC from a Web Worker or AudioWorkletProcessor directly to the AudioWorklet, bypassing the main thread. Useful for high-frequency control or offloading work.

**Options:**

| Option     | Type    | Description |
|------------|---------|-------------|
| `sourceId` | number  | Numeric source ID. `0` = main thread (default), `1+` = workers. Auto-assigned from an incrementing counter if omitted. |
| `blocking` | boolean | SAB mode only. Whether the channel can use `Atomics.wait()` for guaranteed ring buffer delivery. Default: `true` for worker channels (`sourceId !== 0`), `false` for main thread. Set to `false` when using inside an `AudioWorkletProcessor`. In postMessage mode this option has no effect. |

```javascript
const channel = supersonic.createOscChannel();
myWorker.postMessage({ channel: channel.transferable }, channel.transferList);
```

In a Web Worker:
```javascript
import { OscChannel } from 'supersonic-scsynth';
const channel = OscChannel.fromTransferable(event.data.channel);
channel.send(oscBytes);  // Send directly to AudioWorklet
```

For AudioWorklet use, import from the AudioWorklet-safe entry point (`supersonic-scsynth/osc-channel`) which avoids `TextDecoder`, `Worker`, and DOM APIs. See [Workers Guide](WORKERS.md#using-oscchannel-in-an-audioworklet) for details.

For detailed usage including multiple workers, see [Workers Guide](WORKERS.md).

## OSC Commands

SuperSonic speaks the SuperCollider Server protocol. You control the audio engine by sending OSC messages via `send()`.

For the full list of supported commands, see the [scsynth Command Reference](SCSYNTH_COMMAND_REFERENCE.md).

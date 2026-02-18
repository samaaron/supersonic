# API Reference

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
| [`reset(config?)`](#resetconfig)                     | Full teardown and re-initialize (loses all state)         |
| [`send(address, ...args)`](#sendaddress-args)        | Send an OSC message                                       |
| [`sendOSC(data, options)`](#sendoscoscbytes-options) | Send pre-encoded OSC bytes                                |
| [`sync(syncId)`](#syncsyncid)                        | Wait for server to process all commands                   |
| [`purge()`](#flushall)                            | Flush all pending OSC from prescheduler and WASM scheduler |
| [`cancelAll()`](#cancelallscheduled)        | Cancel all pending events in the JS prescheduler          |

### Asset Loading

| Method                                                           | Description                         |
| ---------------------------------------------------------------- | ----------------------------------- |
| [`loadSynthDef(nameOrPath)`](#loadsynthdefnameorpath)            | Load a synth definition             |
| [`loadSynthDefs(names)`](#loadsynthdefsnames)                    | Load multiple synthdefs in parallel |
| [`loadSample(bufnum, source)`](#loadsamplebufnum-source)         | Load a sample into a buffer         |

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
| `metrics`                  | Periodic metrics update                                                                                       |
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
| [`loadedSynthDefs`](#loadedsynthdefs-read-only) | Set of loaded synthdef names (read-only)                 |
| [`bootStats`](#bootstats-read-only)             | Boot timing information (read-only)                      |

### Advanced

| Method                                                                    | Description                                    |
| ------------------------------------------------------------------------- | ---------------------------------------------- |
| [`getInfo()`](#getinfo)                                                   | Get static engine configuration                |
| [`SuperSonic.osc.encode()`](#supersonicoscencodemessage)                  | Encode an OSC message to bytes                 |
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

### `send(address, ...args)`

Send an OSC message. Types are auto-detected from JavaScript values.

```javascript
supersonic.send("/s_new", "sonic-pi-beep", -1, 0, 0, "note", 60, "amp", 0.5);
supersonic.send("/n_free", 1000);
supersonic.send("/b_allocRead", 0, "bd_haus.flac");
```

### `sendOSC(oscBytes, options)`

Send pre-encoded OSC bytes. Useful if you're building OSC messages yourself.

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

Cancel all pending events in the JS prescheduler. This clears future-timestamped bundles that haven't yet been sent to the worklet. Does **not** clear bundles already in the WASM scheduler — use [`purge()`](#flushall) for that.

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

Resume after a suspend. Calls [`purge()`](#flushall) internally to clear any stale scheduled messages from before the suspend, then resumes the AudioContext and resyncs timing. Memory and node tree are preserved. Does **not** emit `setup` event.

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

### `reset(config?)`

Full teardown and re-initialize. Use this when you need a completely fresh state. Event listeners are preserved across reset, but all other state (synthdefs, buffers) is lost.

**Parameters:**
| Name | Type | Description |
|------|------|-------------|
| `config` | `Object` | Optional configuration overrides for init() |

**Returns:** `Promise<void>`

```javascript
// Browser suspended audio, need to recover
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

Subscribe to an event once. The listener auto-unsubscribes after the first event.

```javascript
supersonic.once("ready", (info) => {
  console.log("Engine booted in", info.bootTimeMs, "ms");
});
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
  await supersonic.send("/g_new", 100, 0, 0); // synths group
  await supersonic.send("/g_new", 101, 1, 0); // fx group (after synths)

  // Create FX chain
  await supersonic.send("/s_new", "sonic-pi-fx_reverb", 2000, 0, 101, "in_bus", 20, "out_bus", 0, "mix", 0.3);

  await supersonic.sync();
});
```

**Why use `setup` instead of `ready`?**

In `postMessage` mode, `recover()` destroys and recreates the WASM memory, so all nodes are lost. The `setup` event lets you rebuild consistently on both initial boot and after recovery. In `sab` mode, memory persists across recovery so this is less critical - but using `setup` keeps your code portable.

### Event: `ready`

Emitted when the engine is initialised and ready to use.

```javascript
supersonic.on("ready", (info) => {
  console.log("Sample rate:", info.sampleRate);
  console.log("Boot time:", info.bootTimeMs, "ms");
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

Emitted with raw OSC data including the original bytes. Useful for logging.

```javascript
supersonic.on("message:raw", (data) => {
  console.log("OSC bytes:", data.oscData);
  const parsed = SuperSonic.osc.decode(data.oscData);
  console.log("Parsed:", parsed[0], parsed.slice(1));
});
```

### Event: `message:sent`

Emitted when an OSC message is sent to scsynth.

```javascript
supersonic.on("message:sent", (oscBytes) => {
  const decoded = SuperSonic.osc.decode(oscBytes);
  console.log("Sent:", decoded[0]);
});
```

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

A `Set` containing the names of all confirmed loaded synthdefs. A synthdef appears here only after scsynth has confirmed it's ready - not just when the load request was sent. Removed when freed via `/d_free` or `/d_freeAll`.

```javascript
if (supersonic.loadedSynthDefs.has("sonic-pi-beep")) {
  supersonic.send("/s_new", "sonic-pi-beep", -1, 0, 0);
}

// See all loaded synthdefs
console.log("Loaded:", [...supersonic.loadedSynthDefs]);
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

Returns a snapshot of the scsynth node tree - all synths and groups currently running.

```javascript
const tree = supersonic.getTree();
```

**Returns:**

```javascript
{
  version: 42,          // Increments on every change
  nodeCount: 5,         // Nodes in mirror
  droppedCount: 0,      // Nodes not mirrored due to overflow
  nodes: [
    {
      id: 0,                    // Node ID
      parentId: -1,             // Parent group (-1 for root)
      isGroup: true,            // true = group, false = synth
      defName: "group",         // "group" or synthdef name
      headId: 100,              // First child (-1 if empty/synth)
      prevId: -1,               // Previous sibling (-1 if first)
      nextId: -1                // Next sibling (-1 if last)
    },
    {
      id: 100,
      parentId: 0,
      isGroup: false,
      defName: "sonic-pi-beep",
      headId: -1,
      prevId: -1,
      nextId: 101
    },
    // ...
  ]
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
    updateVisualization(tree.nodes);
  }
  requestAnimationFrame(animate);
}
animate();
```

**Example: List all running synths**

```javascript
const { nodes } = supersonic.getTree();
const synths = nodes.filter((n) => !n.isGroup);

for (const synth of synths) {
  console.log(`Synth ${synth.id}: ${synth.defName}`);
}
```

**Example: Build a nested tree**

```javascript
function buildNestedTree(nodes) {
  const byId = new Map(nodes.map((n) => [n.id, { ...n, children: [] }]));

  for (const node of byId.values()) {
    if (node.parentId !== -1) {
      byId.get(node.parentId)?.children.push(node);
    }
  }

  return byId.get(0); // root group
}
```

**Example: Get children in execution order**

Nodes are linked via `headId`/`nextId`. This is the order scsynth executes them:

```javascript
function getChildrenInOrder(nodes, groupId) {
  const group = nodes.find((n) => n.id === groupId);
  if (!group || group.headId === -1) return [];

  const children = [];
  let nodeId = group.headId;

  while (nodeId !== -1) {
    const node = nodes.find((n) => n.id === nodeId);
    if (!node) break;
    children.push(node);
    nodeId = node.nextId;
  }

  return children;
}
```

**`getTree()` vs `/g_queryTree`**

|                | `getTree()`                   | `/g_queryTree`             |
| -------------- | ----------------------------- | -------------------------- |
| Latency        | Instant (reads shared memory) | ~40ms round-trip           |
| Format         | Flat array with links         | Nested in message args     |
| Control values | Not included                  | Optional (flag=1)          |
| Use case       | 60fps visualization           | One-off queries, debugging |

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

Encode an OSC message into binary format. Types are inferred automatically from JavaScript values.

```javascript
const bytes = SuperSonic.osc.encodeMessage("/s_new", [
  "sonic-pi-beep",  // string
  -1,               // integer
  0,                // integer
  0,                // integer
]);
// bytes is a Uint8Array
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

# API Reference

## Quick Reference

### Core

| Method | Description |
|--------|-------------|
| [`init(config)`](#initconfig) | Initialise the audio engine |
| [`shutdown()`](#shutdown) | Shut down, preserving listeners (can call `init()` again) |
| [`destroy()`](#destroy) | Permanently destroy instance, clearing all listeners |
| [`reset(config?)`](#resetconfig) | Shutdown and re-initialize (convenience method) |
| [`send(address, ...args)`](#sendaddress-args) | Send an OSC message |
| [`sendOSC(data, options)`](#sendoscoscbytes-options) | Send pre-encoded OSC bytes |
| [`sync(syncId)`](#syncsyncid) | Wait for server to process all commands |

### Asset Loading

| Method | Description |
|--------|-------------|
| [`loadSynthDef(nameOrPath)`](#loadsynthdefnameorpath) | Load a synth definition |
| [`loadSynthDefs(names)`](#loadsynthdefsnames) | Load multiple synthdefs in parallel |
| [`loadSample(bufnum, nameOrPath)`](#loadsamplebufnum-nameorpath) | Load a sample into a buffer |

### Events

| Method | Description |
|--------|-------------|
| [`on(event, callback)`](#onevent-callback) | Subscribe to an event, returns unsubscribe function |
| [`off(event, callback)`](#offevent-callback) | Unsubscribe from an event |
| [`once(event, callback)`](#onceevent-callback) | Subscribe to an event once |
| [`removeAllListeners(event?)`](#removealllistenersevent) | Remove all listeners for an event (or all events) |

### Event Types

| Event | Description |
|-------|-------------|
| `ready` | Engine initialised and ready |
| `error` | Error occurred |
| `message` | OSC message received (parsed) |
| `message:raw` | OSC message received (with raw bytes) |
| `message:sent` | OSC message sent |
| `debug` | Debug output from scsynth |
| `metrics` | Periodic metrics update |
| `shutdown` | Engine shutting down (emitted by `shutdown()`, `reset()`, and `destroy()`) |
| `destroy` | Engine being permanently destroyed (emitted by `destroy()` only) |
| `audiocontext:statechange` | AudioContext state changed (with `{ state }` payload) |
| `audiocontext:suspended` | AudioContext was suspended (browser tab backgrounded, etc.) |
| `audiocontext:resumed` | AudioContext resumed running |
| `audiocontext:interrupted` | AudioContext was interrupted (iOS audio session, etc.) |

### Metrics

| Method | Description |
|--------|-------------|
| [`getMetrics()`](#getmetrics) | Get metrics snapshot on demand |
| [`setMetricsInterval(ms)`](#setmetricsintervalms) | Change polling interval |
| [`stopMetricsPolling()`](#stopmetricspolling) | Stop the metrics timer |

### Properties

| Property | Description |
|----------|-------------|
| [`initialized`](#initialized-read-only) | Whether engine is initialised (read-only) |
| [`initializing`](#initializing-read-only) | Whether engine is currently initialising (read-only) |
| [`audioContext`](#audiocontext-read-only) | The Web Audio AudioContext (read-only) |
| `workletNode` | The AudioWorkletNode (read-only) |
| [`loadedSynthDefs`](#loadedsynthdefs-read-only) | Set of loaded synthdef names (read-only) |
| [`bootStats`](#bootstats-read-only) | Boot timing information (read-only) |

### Advanced

| Method | Description |
|--------|-------------|
| [`getInfo()`](#getinfo) | Get static engine configuration |
| [`SuperSonic.osc.encode()`](#supersonicoscencodemessage) | Encode an OSC message to bytes |
| [`SuperSonic.osc.decode()`](#supersonicoscdecodedata-options) | Decode OSC bytes to a message |

## Creating an Instance

```javascript
import { SuperSonic } from "supersonic-scsynth";

const baseURL = "/supersonic"; // Configure for your setup
const supersonic = new SuperSonic({
  workerBaseURL:   `${baseURL}/workers/`,   // Required
  wasmBaseURL:     `${baseURL}/wasm/`,      // Required
  synthdefBaseURL: `${baseURL}/synthdefs/`, // Optional
  sampleBaseURL:   `${baseURL}/samples/`,   // Optional
  scsynthOptions:  { numBuffers: 4096 }     // Optional
});
```

### Constructor Options

| Option | Required | Description |
|--------|----------|-------------|
| `workerBaseURL` | Yes | Base URL for worker scripts |
| `wasmBaseURL` | Yes | Base URL for WASM files |
| `synthdefBaseURL` | No | Base URL for synthdef files (used by `loadSynthDef`) |
| `sampleBaseURL` | No | Base URL for sample files (used by `loadSample`) |
| `scsynthOptions` | No | Server options (see below) |
| `preschedulerCapacity` | No | Max pending events in JS prescheduler (default: 65536) |
| `debugMaxLineLength` | No | Truncate debug messages longer than this (default: 0 = no truncation) |

### Server Options (`scsynthOptions`)

Override scsynth server defaults:

```javascript
const supersonic = new SuperSonic({
  // ... required options ...
  scsynthOptions: {
    numBuffers: 4096,  // Max audio buffers (default: 1024)
  }
});
```

## Core Methods

### `init(config)`

Initialise the audio engine. Call this before anything else.

```javascript
await supersonic.init();
```

**Optional config overrides:**

```javascript
await supersonic.init({
  development: true,  // Use cache-busted WASM (for dev)
  audioContextOptions: {
    sampleRate: 44100,      // Request specific sample rate
    latencyHint: "playback" // "interactive" (default), "balanced", or "playback"
  }
});
```

| Option | Description |
|--------|-------------|
| `development` | Enable development mode with cache-busted WASM files |
| `wasmUrl` | Override the WASM file URL |
| `workletUrl` | Override the worklet script URL |
| `audioContextOptions` | Options passed to the AudioContext constructor |

Calling `init()` multiple times is safe - it returns immediately if already initialised, or returns the existing promise if initialisation is in progress.

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

### `loadSample(bufnum, nameOrPath, startFrame, numFrames)`

Load a sample into a buffer. Pass a filename to use `sampleBaseURL`, or provide a full path.

**Parameters:**
- `bufnum` - Buffer number (integer)
- `nameOrPath` - Sample filename or full path/URL (string)
- `startFrame` - Optional starting frame offset (integer, default: 0)
- `numFrames` - Optional number of frames to load (integer, default: 0 = all frames)

```javascript
// By name (uses sampleBaseURL)
await supersonic.loadSample(0, "loop_amen.flac");

// By full path
await supersonic.loadSample(0, "./custom/my-sample.wav");

// Load partial sample (frames 1000-2000)
await supersonic.loadSample(0, "long-sample.flac", 1000, 1000);
```

### `send(address, ...args)`

Send an OSC message. Types are auto-detected from JavaScript values.

```javascript
supersonic.send('/s_new', 'sonic-pi-beep', -1, 0, 0, 'note', 60, 'amp', 0.5);
supersonic.send('/n_free', 1000);
supersonic.send('/b_allocRead', 0, 'bd_haus.flac');
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
await supersonic.sync();  // Now safe to use the synthdefs

// Use a specific sync ID for tracking
await supersonic.sync(42);
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

### `reset(config?)`

Convenience method that calls `shutdown()` then `init()`. Use this to recover from browser audio suspension or other broken states. Event listeners are preserved across reset.

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

SuperSonic uses an event emitter pattern. Subscribe with `on()`, which returns an unsubscribe function.

### `on(event, callback)`

Subscribe to an event. Returns an unsubscribe function for easy cleanup.

```javascript
// Subscribe
const unsubscribe = supersonic.on('message', (msg) => {
  console.log('Received:', msg.address);
});

// Later, unsubscribe
unsubscribe();
```

Multiple listeners can subscribe to the same event - each receives all events independently.

```javascript
// Both listeners receive all messages
supersonic.on('message', (msg) => console.log('Listener A:', msg.address));
supersonic.on('message', (msg) => console.log('Listener B:', msg.address));
```

### `off(event, callback)`

Unsubscribe using the original callback reference. Alternative to using the unsubscribe function.

```javascript
const handler = (msg) => console.log(msg);
supersonic.on('message', handler);

// Later
supersonic.off('message', handler);
```

### `once(event, callback)`

Subscribe to an event once. The listener auto-unsubscribes after the first event.

```javascript
supersonic.once('ready', (info) => {
  console.log('Engine booted in', info.bootTimeMs, 'ms');
});
```

### Event: `ready`

Emitted when the engine is initialised and ready to use.

```javascript
supersonic.on('ready', (info) => {
  console.log('Sample rate:', info.sampleRate);
  console.log('Boot time:', info.bootTimeMs, 'ms');
});
```

### Event: `error`

Emitted when an error occurs.

```javascript
supersonic.on('error', (error) => {
  console.error('SuperSonic error:', error.message);
});
```

### Event: `message`

Emitted when a parsed OSC message is received from scsynth.

```javascript
supersonic.on('message', (msg) => {
  console.log('Address:', msg.address);
  console.log('Args:', msg.args);
});
```

### Event: `message:raw`

Emitted with raw OSC data including the original bytes. Useful for logging.

```javascript
supersonic.on('message:raw', (data) => {
  console.log('OSC bytes:', data.oscData);
  console.log('Parsed:', data.address, data.args);
});
```

### Event: `message:sent`

Emitted when an OSC message is sent to scsynth.

```javascript
supersonic.on('message:sent', (oscBytes) => {
  const decoded = SuperSonic.osc.decode(oscBytes);
  console.log('Sent:', decoded.address);
});
```

### Event: `debug`

Emitted with debug output from scsynth.

```javascript
supersonic.on('debug', (msg) => {
  console.log('[scsynth]', msg.text);
});
```

### Event: `metrics`

Emitted periodically with performance metrics. See [Metrics](METRICS.md) for details.

```javascript
supersonic.on('metrics', (metrics) => {
  console.log('Messages sent:', metrics.mainMessagesSent);
  console.log('Scheduler depth:', metrics.workletSchedulerDepth);
});
```

### Event: `shutdown`

Emitted when the engine is shutting down. Fired by `shutdown()`, `reset()`, and `destroy()`. Use this to clean up application state that depends on SuperSonic.

```javascript
supersonic.on('shutdown', () => {
  console.log('Engine shutting down, cleaning up...');
  // Reset application state flags, stop loops, etc.
});
```

### Event: `destroy`

Emitted when the engine is being permanently destroyed (only fired by `destroy()`, not by `shutdown()` or `reset()`). This is your last chance to clean up before all listeners are cleared.

```javascript
supersonic.on('destroy', () => {
  console.log('Engine being destroyed permanently');
  // Final cleanup before instance becomes unusable
});
```

### Event: `audiocontext:statechange`

Emitted when the AudioContext state changes. The payload contains the new state.

```javascript
supersonic.on('audiocontext:statechange', ({ state }) => {
  console.log('AudioContext state:', state);
  // state is one of: 'running', 'suspended', 'interrupted', 'closed'
});
```

### Event: `audiocontext:suspended`

Emitted when the AudioContext is suspended. This typically happens when the browser tab is backgrounded or the system suspends audio. Use this to show a "restart" UI to the user.

```javascript
supersonic.on('audiocontext:suspended', () => {
  console.log('Audio suspended - show restart button');
  showRestartUI();
});
```

### Event: `audiocontext:resumed`

Emitted when the AudioContext resumes running after being suspended.

```javascript
supersonic.on('audiocontext:resumed', () => {
  console.log('Audio resumed');
  hideRestartUI();
});
```

### Event: `audiocontext:interrupted`

Emitted when the AudioContext is interrupted by the system (common on iOS when another app takes audio focus). Similar to `suspended` but triggered externally.

```javascript
supersonic.on('audiocontext:interrupted', () => {
  console.log('Audio interrupted by system');
  showRestartUI();
});
```

### `removeAllListeners(event?)`

Remove all listeners for an event, or all listeners entirely. Useful for cleanup.

```javascript
// Remove all 'message' listeners
supersonic.removeAllListeners('message');

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
  console.log('Engine is booting...');
} else if (supersonic.initialized) {
  console.log('Engine is ready');
} else {
  console.log('Engine not started');
}
```

### `audioContext` (read-only)

The underlying Web Audio AudioContext.

```javascript
const ctx = supersonic.audioContext;
console.log('Sample rate:', ctx.sampleRate);
```

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

## Metrics API

For monitoring performance and debugging. See [Metrics](METRICS.md) for the full list of available metrics.

### `getMetrics()`

Get a metrics snapshot on demand.

```javascript
const metrics = supersonic.getMetrics();
console.log('Messages processed:', metrics.workletMessagesProcessed);
```

### `setMetricsInterval(ms)`

Change the polling interval for `onMetricsUpdate`. Default is 100ms (10Hz).

```javascript
supersonic.setMetricsInterval(500);  // Update every 500ms
```

### `stopMetricsPolling()`

Stop the metrics polling timer.

```javascript
supersonic.stopMetricsPolling();
```

## Advanced

### `getInfo()`

Returns static configuration from boot time - things that don't change after initialisation.

```javascript
const info = supersonic.getInfo();
console.log('Sample rate:', info.sampleRate);
console.log('Boot time:', info.bootTimeMs, 'ms');
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

### `SuperSonic.osc.encode(message)`

Encode an OSC message object into binary format. Useful for building OSC messages manually.

```javascript
const message = {
  address: "/s_new",
  args: [
    { type: "s", value: "sonic-pi-beep" },
    { type: "i", value: -1 },
    { type: "i", value: 0 },
    { type: "i", value: 0 }
  ]
};

const bytes = SuperSonic.osc.encode(message);
// bytes is a Uint8Array
```

### `SuperSonic.osc.decode(data, options)`

Decode binary OSC data into a message object. Useful for debugging or logging.

**Parameters:**
- `data` - `Uint8Array` or `ArrayBuffer` containing OSC data
- `options` - Optional. `{ metadata: true }` to include type info in args

```javascript
// Basic decode
const msg = SuperSonic.osc.decode(oscBytes);
console.log(msg.address, msg.args);

// With type metadata
const msg = SuperSonic.osc.decode(oscBytes, { metadata: true });
// msg.args will be [{ type: "s", value: "..." }, ...]
```

## Common OSC Commands

SuperSonic speaks the SuperCollider Server protocol. Here are the commands you'll use most often:

### Synths

```javascript
// Create a synth (node ID -1 = auto-assign)
supersonic.send('/s_new', 'synth-name', nodeId, addAction, target, ...params);

// Set synth parameters
supersonic.send('/n_set', nodeId, 'param', value, 'param2', value2);

// Free a synth
supersonic.send('/n_free', nodeId);
```

### Buffers (Samples)

```javascript
// Load a sample into a buffer
supersonic.send('/b_allocRead', bufferNum, 'filename.flac');

// Free a buffer
supersonic.send('/b_free', bufferNum);
```

### Server

```javascript
// Enable notifications (receive messages back from server)
supersonic.send('/notify', 1);

// Query server status
supersonic.send('/status');
```

For the complete OSC command reference, see the [SuperCollider Server Command Reference](https://doc.sccode.org/Reference/Server-Command-Reference.html).

### Unsupported Commands

These SuperCollider commands are **not supported** in SuperSonic because there's no filesystem in the browser/WASM environment:

| Command | Alternative |
|---------|-------------|
| `/d_load` | Use `loadSynthDef()` or send `/d_recv` with synthdef bytes |
| `/d_loadDir` | Use `loadSynthDefs()` to load multiple synthdefs |
| `/b_read` | Use `loadSample()` to load audio into a buffer |
| `/b_readChannel` | Use `loadSample()` to load audio into a buffer |
| `/b_write` | Not available (cannot write files in browser) |
| `/b_close` | Not available (no disk streaming in browser) |

Attempting to send these commands will throw an error with guidance on the alternative.

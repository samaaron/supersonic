# API Reference

The complete SuperSonic API.

## Quick Reference

### Core

| Method | Description |
|--------|-------------|
| [`init()`](#init) | Initialise the audio engine |
| `destroy()` | Shut down and clean up resources |
| [`send(address, ...args)`](#sendaddress-args) | Send an OSC message |
| [`sendOSC(data, options)`](#sendoscoscbytes-options) | Send pre-encoded OSC bytes |
| `sync(syncId)` | Wait for server to process all commands |

### Asset Loading

| Method | Description |
|--------|-------------|
| [`loadSynthDef(nameOrPath)`](#loadsynthdefnameorpath) | Load a synth definition |
| [`loadSynthDefs(names)`](#loadsynthdefsnames) | Load multiple synthdefs in parallel |
| [`loadSample(bufnum, nameOrPath)`](#loadsamplebufnum-nameorpath) | Load a sample into a buffer |

### Metrics

| Method | Description |
|--------|-------------|
| [`onMetricsUpdate`](#onmetricsupdatemetrics) | Callback for periodic metrics updates |
| [`getMetrics()`](#getmetrics) | Get metrics snapshot on demand |
| [`setMetricsInterval(ms)`](#setmetricsintervalms) | Change polling interval |
| [`stopMetricsPolling()`](#stopmetricspolling) | Stop the metrics timer |

### Callbacks

| Property | Description |
|----------|-------------|
| [`onInitialized`](#oninitialized) | Called when engine is ready |
| [`onError`](#onerrorerror) | Called on errors |
| [`onMessage`](#onmessagemsg) | Called when OSC message received |
| [`onOSC`](#onoscdata) | Called with raw OSC bytes received |
| [`onMessageSent`](#onmessagesentdata) | Called when OSC message sent |
| [`onDebugMessage`](#ondebugmessagemsg) | Called with debug output |

### Properties

| Property | Description |
|----------|-------------|
| [`initialized`](#initialized-read-only) | Whether engine is initialised (read-only) |
| [`audioContext`](#audiocontext-read-only) | The Web Audio AudioContext (read-only) |
| `workletNode` | The AudioWorkletNode (read-only) |

### Advanced

| Method | Description |
|--------|-------------|
| [`getInfo()`](#getinfo) | Get static engine configuration |

## Creating an Instance

```javascript
import { SuperSonic } from "supersonic-scsynth";

const baseURL = "/supersonic"; // Configure for your setup
const sonic = new SuperSonic({
  workerBaseURL:   `${baseURL}/workers/`,   // Required
  wasmBaseURL:     `${baseURL}/wasm/`,      // Required
  synthdefBaseURL: `${baseURL}/synthdefs/`, // Optional
  sampleBaseURL:   `${baseURL}/samples/`    // Optional
});
```

## Core Methods

### `init()`

Initialise the audio engine. Must be called before anything else.

```javascript
await sonic.init();
```

### `loadSynthDef(nameOrPath)`

Load a synth definition. Pass a name to use `synthdefBaseURL`, or a full path.

```javascript
// By name (uses synthdefBaseURL)
await sonic.loadSynthDef("sonic-pi-beep");

// By full path
await sonic.loadSynthDef("./custom/my-synth.scsyndef");
```

### `loadSynthDefs(names)`

Load multiple synth definitions by name in parallel.

```javascript
await sonic.loadSynthDefs(["sonic-pi-beep", "sonic-pi-prophet"]);
```

### `loadSample(bufnum, nameOrPath)`

Load a sample into a buffer. Pass a filename to use `sampleBaseURL`, or a full path.

```javascript
// By name (uses sampleBaseURL)
await sonic.loadSample(0, "loop_amen.flac");

// By full path
await sonic.loadSample(0, "./custom/my-sample.wav");
```

### `send(address, ...args)`

Send an OSC message. Types are auto-detected from JavaScript values.

```javascript
sonic.send('/s_new', 'sonic-pi-beep', -1, 0, 0, 'note', 60, 'amp', 0.5);
sonic.send('/n_free', 1000);
sonic.send('/b_allocRead', 0, 'bd_haus.flac');
```

### `sendOSC(oscBytes, options)`

Send pre-encoded OSC bytes. Useful if you're building OSC messages yourself.

```javascript
const oscData = new Uint8Array([...]); // Your OSC bytes
sonic.sendOSC(oscData);
```

## Callbacks

Set these to receive events from the engine.

### `onInitialized`

Called when the engine is ready.

```javascript
sonic.onInitialized = () => {
  console.log('Engine ready');
};
```

### `onError(error)`

Called when something goes wrong.

```javascript
sonic.onError = (error) => {
  console.error('SuperSonic error:', error);
};
```

### `onMessage(msg)`

Called when an OSC message is received from the engine.

```javascript
sonic.onMessage = (msg) => {
  console.log('Received:', msg.address, msg.args);
};
```

### `onOSC(data)`

Called with raw OSC bytes received from the engine.

```javascript
sonic.onOSC = (data) => {
  // data is a Uint8Array
};
```

### `onMessageSent(data)`

Called when an OSC message is sent to the engine.

```javascript
sonic.onMessageSent = (data) => {
  // data is a Uint8Array
};
```

### `onDebugMessage(msg)`

Called with debug output from the engine.

```javascript
sonic.onDebugMessage = (msg) => {
  console.log('[scsynth]', msg);
};
```

### `onMetricsUpdate(metrics)`

Called periodically with performance metrics. See [Metrics](METRICS.md) for details.

```javascript
sonic.onMetricsUpdate = (metrics) => {
  console.log('Process count:', metrics.workletProcessCount);
};
```

## Properties

### `initialized` (read-only)

Whether the engine has been initialised.

```javascript
if (sonic.initialized) {
  sonic.send('/s_new', ...);
}
```

### `audioContext` (read-only)

The underlying Web Audio AudioContext.

```javascript
const ctx = sonic.audioContext;
console.log('Sample rate:', ctx.sampleRate);
```

## Metrics API

For monitoring performance and debugging. See [Metrics](METRICS.md) for the full list of available metrics.

### `getMetrics()`

Get a metrics snapshot on demand.

```javascript
const metrics = sonic.getMetrics();
console.log('Messages processed:', metrics.workletMessagesProcessed);
```

### `setMetricsInterval(ms)`

Change the polling interval for `onMetricsUpdate`. Default is 100ms (10Hz).

```javascript
sonic.setMetricsInterval(500);  // Update every 500ms
```

### `stopMetricsPolling()`

Stop the metrics polling timer.

```javascript
sonic.stopMetricsPolling();
```

## Advanced

### `getInfo()`

Static config from boot time.

```javascript
const info = sonic.getInfo();
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

## Common OSC Commands

SuperSonic uses the SuperCollider Server protocol. Common commands:

### Synths

```javascript
// Create a synth (node ID -1 = auto-assign)
sonic.send('/s_new', 'synth-name', nodeId, addAction, target, ...params);

// Set synth parameters
sonic.send('/n_set', nodeId, 'param', value, 'param2', value2);

// Free a synth
sonic.send('/n_free', nodeId);
```

### Buffers (Samples)

```javascript
// Load a sample into a buffer
sonic.send('/b_allocRead', bufferNum, 'filename.flac');

// Free a buffer
sonic.send('/b_free', bufferNum);
```

### Server

```javascript
// Enable notifications (receive messages back from server)
sonic.send('/notify', 1);

// Query server status
sonic.send('/status');
```

For the complete OSC command reference, see the [SuperCollider Server Command Reference](https://doc.sccode.org/Reference/Server-Command-Reference.html).

# Guide

Practical patterns and worked examples for SuperSonic. For the full method and property reference, see [API.md](API.md).

## Table of Contents

- [Lifecycle & Recovery](#lifecycle--recovery)
- [Audio Routing](#audio-routing)
- [Node Tree](#node-tree)
- [Samples](#samples)
- [Events & Debugging](#events--debugging)
- [Constructor Sub-Options](#constructor-sub-options)
- [OSC Encoding](#osc-encoding)

---

## Lifecycle & Recovery

### Setup vs Ready

The `setup` event fires after init/recover completes, before `ready`. Async handlers are awaited. Use it for any persistent audio infrastructure that needs to exist on both initial boot and after recovery:

- **Groups** — node tree organization
- **FX chains** — reverb, filters, compressors
- **Bus routing** — synths that read/write to audio buses
- **Persistent synths** — always-on nodes like analyzers or mixers

```javascript
supersonic.on("setup", async () => {
  // Create group structure
  supersonic.send("/g_new", 100, 0, 0); // synths group
  supersonic.send("/g_new", 101, 1, 0); // fx group (after synths)

  // Create FX chain
  supersonic.send("/s_new", "sonic-pi-fx_reverb", 2000, 0, 101,
    "in_bus", 20, "out_bus", 0, "mix", 0.3);

  await supersonic.sync();
});
```

**Why `setup` instead of `ready`?** When `recover()` falls through to a full `reload()`, WASM memory is destroyed and recreated, so all nodes are lost. The `setup` event lets you rebuild consistently on both initial boot and after recovery, regardless of transport mode. See [Communication Modes](MODES.md) for more on SAB vs postMessage.

### Tab Visibility & Recovery

Use `visibilitychange` to recover when the user switches back to your tab:

```javascript
document.addEventListener("visibilitychange", async () => {
  if (!document.hidden) {
    await supersonic.recover();
  }
});
```

### Resume vs Reload

`resume()` is fast but only works if the worklet is still alive — it calls `purge()` to flush stale scheduled messages, resumes the AudioContext, and resyncs timing. `reload()` is a full restart. When you don't know which is needed, use `recover()` — or handle it manually:

```javascript
if (await supersonic.resume()) {
  console.log("Quick resume worked, nodes preserved");
} else {
  console.log("Worklet was killed, need full reload");
  await supersonic.reload();
}
```

---

## Audio Routing

### Connecting Microphone Input

```javascript
const stream = await navigator.mediaDevices.getUserMedia({ audio: true });
const micSource = audioContext.createMediaStreamSource(stream);

// Connect mic to SuperSonic input
micSource.connect(supersonic.node.input);

// Audio flows through scsynth's input buses (bus 2+ by default)
// Use In.ar(2) in a synthdef to read the mic signal
```

### Connecting to an Analyser

```javascript
const analyser = supersonic.audioContext.createAnalyser();

// Connect SuperSonic output to analyser (in addition to speakers)
supersonic.node.connect(analyser);

// Read frequency data
const data = new Uint8Array(analyser.frequencyBinCount);
analyser.getByteFrequencyData(data);
```

---

## Node Tree

### Polling for Visualization

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

### Tree Traversal

Recursive helper to collect all running synths:

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

### `getTree()` vs `getRawTree()` vs `/g_queryTree`

|                | `getTree()`                      | `getRawTree()`                 | `/g_queryTree`             |
| -------------- | -------------------------------- | ------------------------------ | -------------------------- |
| Latency        | Instant (reads shared memory)    | Instant (reads shared memory)  | ~40ms round-trip           |
| Format         | Hierarchical (nested children)   | Flat array with link pointers  | Nested in message args     |
| Control values | Not included                     | Not included                   | Optional (flag=1)          |
| Use case       | Tree visualization, UI rendering | Debugging, low-level access    | One-off queries, debugging |

`getTree()` returns node structure only — not control values. For control values, use `/g_queryTree` with flag=1 or `/n_get` for specific nodes. See [scsynth Command Reference](SCSYNTH_COMMAND_REFERENCE.md).

---

## Samples

### Loading from Different Sources

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

### Deduplication with `sampleInfo()`

Use `sampleInfo()` to get a SHA-256 content hash without allocating a buffer, then check before loading:

```javascript
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

---

## Events & Debugging

### Latency Analysis with `message:raw`

The `message:raw` event includes timing information for measuring delivery latency:

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

### Multi-Worker Source Tracking with `message:sent`

When using multiple OscChannels (see [Workers Guide](WORKERS.md)), `sourceId` identifies which channel sent each message:

```javascript
supersonic.on("message:sent", ({ oscData, sourceId, sequence, timestamp, scheduledTime }) => {
  const decoded = SuperSonic.osc.decode(oscData);
  const relativeTime = (timestamp - supersonic.initTime).toFixed(2);
  console.log(`[${sequence}] +${relativeTime}s [src:${sourceId}]`, decoded[0]);
});
```

- `sourceId` — `0` = main thread, `1+` = worker channels
- `timestamp` — NTP seconds when the message was written to the ring buffer
- `scheduledTime` — NTP seconds from bundle timetag, or `null` if not a bundle

---

## Constructor Sub-Options

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

---

## OSC Encoding

### Full-Precision NTP Timestamps

When scheduling bundles, a plain JavaScript number loses sub-microsecond precision because IEEE 754 float64 only has 52 mantissa bits — not enough for NTP's full 64-bit range. Use a `[seconds, fraction]` uint32 pair for lossless timestamps:

```javascript
// Float — easy but loses precision at large NTP values
const ntpTime = SuperSonic.osc.ntpNow();
SuperSonic.osc.encodeBundle(ntpTime + 0.5, [
  ["/s_new", "sonic-pi-beep", 1001, 0, 0],
]);

// Uint32 pair — full 64-bit precision, no float loss
SuperSonic.osc.encodeBundle([3913056000, 2147483648], [
  ["/s_new", "sonic-pi-beep", 1001, 0, 0],
  ["/n_set", 1001, "amp", 0.5],
]);
```

**TimeTag formats accepted by `encodeBundle()`:**

| Value | Meaning |
|---|---|
| `1`, `null`, `undefined` | Execute immediately |
| `[seconds, fraction]` | NTP uint32 pair — preserves full 64-bit precision |
| Number (e.g. `3913056000.5`) | NTP float — seconds since 1900 (fractional part encoded) |

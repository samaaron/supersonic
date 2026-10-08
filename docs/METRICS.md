# Metrics

Real-time performance metrics for monitoring what's happening inside SuperSonic.

On every host the engine writes most of these into a metrics region of shared memory. On the web, `getMetrics()` reads that region and adds metrics of the client's own. Natively, a client attached to the server's shared memory reads the same region (`clockwork_client_metrics` in `clockwork/src/clockwork_client.h`), plus a region of native-only stats. Each section below says which hosts it applies to.

On the web, in both [communication modes](MODES.md), calling `getMetrics()` is a cheap local memory read with no IPC overhead.

## Quick Start

```javascript
// Poll metrics from your UI update loop
setInterval(() => {
  const metrics = supersonic.getMetrics();
  console.log('Processed:', metrics.engineMessagesProcessed);
  console.log('Dropped:', metrics.engineMessagesDropped);
}, 100);

// Or use requestAnimationFrame for smoother UI updates
function updateUI() {
  const metrics = supersonic.getMetrics();
  // Update your UI here
  requestAnimationFrame(updateUI);
}
requestAnimationFrame(updateUI);
```

## API

### `getMetrics()`

Web only. Get a metrics snapshot on demand, as an object. This is a local memory read with no IPC, safe to call from `requestAnimationFrame` or high-frequency timers. For a read that allocates nothing, use `getMetricsArray()` (see [Metrics Component](METRICS_COMPONENT.md)).

```javascript
const metrics = supersonic.getMetrics();
```

## Available Metrics

The engine's metrics are written by the same C++ on every host. In postMessage mode the worklet posts a copy of them on a heartbeat, so they can be up to one heartbeat old.

### Engine Metrics

All hosts.

| Property | Description |
|----------|-------------|
| `engineProcessCount` | Audio process() calls (cumulative) |
| `engineMessagesProcessed` | Messages drained from the IN ring and dispatched |
| `engineMessagesDropped` | Messages dropped because a ring buffer was full |

### Scheduler Metrics

All hosts. clockwork's scheduler holds timed bundles and hands them to scsynth at the exact sample. There is no scheduling in JavaScript: every bundle goes straight onto the IN ring, however far ahead it is timed.

| Property | Description |
|----------|-------------|
| `engineSchedulerDepth` | Current scheduler queue depth |
| `engineSchedulerPeakDepth` | Peak scheduler queue depth (high water mark) |
| `engineSchedulerCapacity` | Maximum scheduler queue size (compile-time constant; 2048 in SuperSonic's web build). Web client only |
| `engineSchedulerDropped` | Events dropped because the scheduler queue overflowed |
| `engineSequenceGaps` | Sequence gaps seen on the rings: messages lost in transit to or from the engine |
| `engineSchedulerLates` | Bundles executed after their scheduled time |
| `engineSchedulerMaxLateMs` | Worst lateness seen (ms) |
| `engineSchedulerLastLateMs` | Most recent lateness (ms) |
| `engineSchedulerLastLateTick` | `engineProcessCount` when the last late bundle ran |

### OSC Input Metrics

Replies coming back from the engine.

| Property | Description |
|----------|-------------|
| `oscInMessagesReceived` | OSC replies received from the engine |
| `oscInMessagesDropped` | Replies lost (sequence gaps). Web client only |
| `oscInBytesReceived` | Total bytes received |

### Debug Metrics

All hosts. Debug lines from the engine.

| Property | Description |
|----------|-------------|
| `debugMessagesReceived` | Debug messages received |
| `debugBytesReceived` | Total debug bytes received |

### OSC Out Metrics

Messages sent to the engine. On the web the client counts them; natively the engine counts what it takes in.

| Property | Description |
|----------|-------------|
| `oscOutMessagesSent` | OSC messages sent to the engine |
| `oscOutBytesSent` | Total bytes sent |

### Buffer Usage

Ring buffer fill levels. The engine measures them during each block and writes them to the metrics region, on every host, so they are available in both SAB and postMessage modes. The `capacity` and `percentage` fields are the web client's, worked out from the ring sizes.

| Property | Description |
|----------|-------------|
| `inBufferUsed.bytes` | Bytes currently in input buffer |
| `inBufferUsed.percentage` | Input buffer percentage used |
| `inBufferUsed.peakBytes` | Peak bytes used (high water mark) |
| `inBufferUsed.peakPercentage` | Peak percentage used |
| `inBufferUsed.capacity` | Total input buffer capacity in bytes |
| `outBufferUsed.bytes` | Bytes currently in output buffer |
| `outBufferUsed.percentage` | Output buffer percentage used |
| `outBufferUsed.peakBytes` | Peak bytes used (high water mark) |
| `outBufferUsed.peakPercentage` | Peak percentage used |
| `outBufferUsed.capacity` | Total output buffer capacity in bytes |
| `nrtOutBufferUsed.bytes` | Bytes currently in the NRT-out buffer |
| `nrtOutBufferUsed.percentage` | NRT-out buffer percentage used |
| `nrtOutBufferUsed.peakBytes` | Peak bytes used (high water mark) |
| `nrtOutBufferUsed.peakPercentage` | Peak percentage used |
| `nrtOutBufferUsed.capacity` | Total NRT-out buffer capacity in bytes |

The NRT-out ring carries replies written off the audio thread. Natively that is the engine's control thread; on the web nothing writes it, so it stays empty. In the schema and `getMetricsArray()`, these are the flat keys `inBufferUsedBytes`, `inBufferPeakBytes` and `inBufferCapacity`, and likewise for `outBuffer` and `nrtOutBuffer`.

### Clock

All hosts. The engine's session clock, read once a block.

| Property | Description |
|----------|-------------|
| `clockTempoMbpm` | Tempo, in milli-BPM |
| `clockBeatCenti` | Beat position × 100 |
| `clockPhaseCenti` | Phase within the quantum × 100 |
| `clockPlaying` | Transport playing (0/1) |

### Link

Native only: always 0 on the web, which has no Ableton Link.

| Property | Description |
|----------|-------------|
| `linkPeers` | Connected Link peers on the network |
| `linkTempoMbpm` | Shared Link session tempo, in milli-BPM |
| `linkBeatCenti` | Link beat position × 100 |
| `linkPhaseCenti` | Phase within the Link quantum × 100 |
| `linkPlaying` | Link transport playing (0/1) |
| `linkAudioInChannels` | Active received Link Audio channels |
| `linkAudioStreamRate` | Received Link Audio stream sample rate (Hz) |
| `linkAudioUnderruns` | Receiver queue underruns |
| `linkAudioBufferedMs` | Received Link Audio queued in the receiver (ms) |
| `linkAudioDriftPpm` | Read-rate deviation from the sender's clock (ppm) |
| `linkAudioPublish` | Link Audio publishing enabled (0/1) |
| `linkAudioSinks` | Active Link Audio output sinks |

### System Info

All hosts. Written once when the engine starts.

| Property | Description |
|----------|-------------|
| `clockworkCommit` | The clockwork commit the engine was built from (its first 8 hex digits; 0 when unknown) |
| `audioSampleRate` | Output sample rate (Hz) |
| `audioBlockSize` | Audio block size in frames |
| `audioOutputChannels` | Output bus channels |
| `audioInputChannels` | Input bus channels |

### Timing

Web client only.

| Property | Description |
|----------|-------------|
| `driftOffsetMs` | Clock drift between AudioContext and performance.now() |
| `clockOffsetMs` | Clock offset for syncing with another system (`setClockOffset()`) |
| `ntpStartTime` | NTP time when the AudioContext's clock was zero (`getMetrics()` only) |

### Client State

Web client only.

| Property | Description |
|----------|-------------|
| `audioContextState` | AudioContext state ("running", "suspended", etc.) |
| `mode` | Transport mode: `'sab'` or `'postMessage'` |
| `audioHealthPct` | Fraction of expected audio frames delivered (100 = no issues) |
| `hasPlaybackStats` | Whether the browser has the `playbackStats` API (Chrome) |
| `glitchCount` | Audio underrun events (Chrome only) |
| `glitchDurationMs` | Total silence from audio underruns (Chrome only) |
| `averageLatencyUs` | Average audio output latency (Chrome only) |
| `maxLatencyUs` | Maximum audio output latency (Chrome only) |
| `totalFramesDurationMs` | Total audio rendered duration (Chrome only) |

### Buffers and SynthDefs

Web client only. SuperSonic's own metrics, about the sample buffer pool and the synthdefs the client holds.

| Property | Description |
|----------|-------------|
| `bufferPoolUsedBytes` | Bytes used in the buffer pool |
| `bufferPoolAvailableBytes` | Bytes available in the buffer pool |
| `bufferPoolAllocations` | Buffers currently allocated |
| `bufferPoolTotalCapacity` | Committed capacity across all pool segments |
| `bufferPoolMaxCapacity` | The most the pool may grow to |
| `bufferPoolGrowthCount` | Times the pool has grown |
| `bufferPoolPoolCount` | Pool segments; 1 means it has never grown |
| `loadedSynthDefs` | Synthdefs this client holds, and will replay after a reload |

### Error Metrics

Error counters for diagnosing issues.

| Property | Description |
|----------|-------------|
| `engineWasmErrors` | WASM execution errors in audio worklet. Web only |
| `oscInCorrupted` | Ring buffer message corruption detected |
| `ringBufferDirectWriteFails` | Writes to the IN ring that found it full, so the message was dropped. Web, SAB mode only |

### Native Stats

Native only. These live in a region of their own (`CLOCKWORK_REGION_NATIVE_STATS`), which `getMetrics()` does not read.

| Property | Description |
|----------|-------------|
| `cpuAvgCenti` | Average DSP load: the share of each audio callback's time budget the render used, smoothed, × 100 |
| `cpuPeakCenti` | Peak DSP load, decaying, × 100 |
| `cbOverruns` | Audio callbacks that overran their time budget |
| `nrtMaxPassUs` | Longest the control thread has spent on one batch of commands since boot (µs) |
| `nrtInFlightUs` | How long the control thread has been in the batch it is handling now (µs); anything but 0 means commands are waiting |
| `nrtRecentWorstUs` | Longest batch in the last minute (µs) |

## Node Tree Mirror

Beyond numeric metrics, SuperSonic mirrors the entire scsynth node tree into shared memory, on every host. This gives you a live view of every synth and group currently running - updated in real-time with zero OSC round-trip latency. On the web, `getTree()` reads it.

The mirror lives in the guest window, a region clockwork lays out but never interprets, so the arena table also carries **whose** window it is: SuperSonic declares a magic (`'SCNT'`, `0x53434E54`) and a layout version (`1`) from `dsp_describe()`, and clockwork copies them into the table's window entry when the guest binds (`CLOCKWORK_GEOM_WINDOW_MAGIC`, `CLOCKWORK_GEOM_WINDOW_VERSION`). A reader checks them before parsing (`nodeTreeWindowMatches` in `js/lib/node_tree_parser.js`, `NODE_TREE_WINDOW_MAGIC`/`NODE_TREE_WINDOW_VERSION` in `dsp/scsynth/node_tree.h`); zeros mean the guest has not bound yet.

```javascript
const tree = supersonic.getTree();
// {
//   version: 42,        // Increments on every change
//   nodeCount: 5,       // Total nodes
//   droppedCount: 0,    // Overflow (capacity exceeded)
//   root: { ... }       // Hierarchical TreeNode (always id 0)
// }
```

Use `version` to skip re-renders when nothing changed - perfect for 60fps visualizations.

For the full API including node structure, tree traversal examples, and comparison with `/g_queryTree`, see [API Reference](API.md) and [Guide](GUIDE.md).

## Metrics Web Component

For a ready-made metrics UI on the web, use the `<clockwork-metrics>` web component. It renders all metrics panels from the schema with zero manual DOM work:

```html
<link rel="stylesheet" href="dist/metrics-dark.css" />
<script type="module" src="dist/metrics_component.js"></script>

<clockwork-metrics id="metrics"></clockwork-metrics>
```

```javascript
// After boot:
document.getElementById("metrics").connect(sonic, { refreshRate: 10 });
```

See [Metrics Component](METRICS_COMPONENT.md) for full documentation including theming, layout control, and the zero-allocation `getMetricsArray()` API.

## Example: Simple Monitor

```javascript
setInterval(() => {
  const metrics = supersonic.getMetrics();

  // Check for problems
  if (metrics.engineMessagesDropped > 0) {
    console.warn('Messages being dropped!');
  }

  // Monitor buffer usage
  if (metrics.inBufferUsed?.percentage > 80) {
    console.warn('Input buffer getting full:', metrics.inBufferUsed.percentage + '%');
  }

  // Track throughput
  console.log(`Processed: ${metrics.engineMessagesProcessed}, Sent: ${metrics.oscOutMessagesSent}`);
}, 100);
```

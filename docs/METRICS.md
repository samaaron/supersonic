# Metrics

Real-time performance metrics for monitoring what's happening inside SuperSonic.

- **SAB mode** - reads directly from SharedArrayBuffer with zero overhead
- **PostMessage mode** - reads from a cached snapshot (worklet sends updates every 50ms by default)

## Quick Start

```javascript
// Poll metrics from your UI update loop
setInterval(() => {
  const metrics = supersonic.getMetrics();
  console.log('Processed:', metrics.scsynthMessagesProcessed);
  console.log('Dropped:', metrics.scsynthMessagesDropped);
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

Get a metrics snapshot on demand. This is a cheap local memory read - safe to call from `requestAnimationFrame` or high-frequency timers without causing any IPC or copying.

```javascript
const metrics = supersonic.getMetrics();
```

## Available Metrics

### scsynth Metrics

From the WASM scsynth engine running in the AudioWorklet.

| Property | Description |
|----------|-------------|
| `scsynthProcessCount` | Audio process() calls (cumulative) |
| `scsynthMessagesProcessed` | OSC messages processed by scsynth |
| `scsynthMessagesDropped` | Messages dropped (scheduler queue full) |

### scsynth Scheduler Metrics

(WASM, AudioWorklet) - Receives bundles from the prescheduler and executes them at the exact sample. This is where sample-accurate timing happens.

| Property | Description |
|----------|-------------|
| `scsynthSchedulerDepth` | Current scheduler queue depth |
| `scsynthSchedulerPeakDepth` | Peak scheduler queue depth (high water mark) |
| `scsynthSchedulerCapacity` | Maximum scheduler queue size (compile-time constant) |
| `scsynthSchedulerDropped` | Messages dropped from scheduler |
| `scsynthSequenceGaps` | Sequence gaps detected (indicates lost messages) |
| `scsynthSchedulerLates` | Bundles executed after their scheduled time |

### Prescheduler Metrics
(JavaScript, worker thread) - Holds timed OSC bundles and dispatches them to the AudioWorklet just before they're needed. This keeps the ring buffer from filling up with future events.

When you send a bundle scheduled for 2 seconds in the future, the prescheduler holds it, then dispatches it to scsynth ~50ms before execution time. Scsynth's scheduler then fires it at precisely the right sample.


| Property | Description |
|----------|-------------|
| `preschedulerPending` | Events waiting in queue |
| `preschedulerPendingPeak` | Peak queue depth |
| `preschedulerDispatched` | Bundles sent to worklet |
| `preschedulerRetriesSucceeded` | Writes that succeeded after retry |
| `preschedulerRetriesFailed` | Writes that failed after max retries |
| `preschedulerBundlesScheduled` | Total bundles scheduled |
| `preschedulerEventsCancelled` | Bundles cancelled before dispatch |
| `preschedulerTotalDispatches` | Dispatch cycles executed |
| `preschedulerMessagesRetried` | Total retry attempts |
| `preschedulerRetryQueueSize` | Current retry queue size |
| `preschedulerRetryQueuePeak` | Peak retry queue size |
| `preschedulerBypassed` | Messages that bypassed prescheduler (aggregate total) |
| `bypassNonBundle` | Plain OSC messages (not bundles) that bypassed prescheduler |
| `bypassImmediate` | Bundles with timetag 0 or 1 that bypassed prescheduler |
| `bypassNearFuture` | Bundles within lookahead window (default 500ms) that bypassed prescheduler |
| `bypassLate` | Bundles past their scheduled time that bypassed prescheduler |
| `preschedulerCapacity` | Maximum pending events allowed |
| `preschedulerMinHeadroomMs` | Smallest time gap between JS prescheduler dispatch and scsynth scheduler execution |
| `preschedulerLates` | Bundles dispatched after their scheduled execution time |

### OSC Input Metrics

Messages coming back from scsynth.

| Property | Description |
|----------|-------------|
| `oscInMessagesReceived` | OSC replies received from scsynth |
| `oscInMessagesDropped` | Replies lost (sequence gaps or corruption) |
| `oscInBytesReceived` | Total bytes received |

### Debug Metrics

Debug output from scsynth.

| Property | Description |
|----------|-------------|
| `debugMessagesReceived` | Debug messages received |
| `debugBytesReceived` | Total debug bytes received |

### OSC Out Metrics

Messages sent to scsynth.

| Property | Description |
|----------|-------------|
| `oscOutMessagesSent` | OSC messages sent to scsynth |
| `oscOutBytesSent` | Total bytes sent |

### Buffer Usage

Ring buffer fill levels. WASM calculates buffer usage during each process() call and writes values to the metrics region, making this available in both SAB and postMessage modes.

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
| `debugBufferUsed.bytes` | Bytes currently in debug buffer |
| `debugBufferUsed.percentage` | Debug buffer percentage used |
| `debugBufferUsed.peakBytes` | Peak bytes used (high water mark) |
| `debugBufferUsed.peakPercentage` | Peak percentage used |
| `debugBufferUsed.capacity` | Total debug buffer capacity in bytes |

### Timing

| Property | Description |
|----------|-------------|
| `driftOffsetMs` | Clock drift between AudioContext and performance.now() |

### Engine State

Main thread state.

| Property | Description |
|----------|-------------|
| `audioContextState` | AudioContext state ("running", "suspended", etc.) |
| `bufferPoolUsedBytes` | Bytes used in the buffer pool |
| `bufferPoolAvailableBytes` | Bytes available in the buffer pool |
| `bufferPoolAllocations` | Total buffer allocations made |
| `loadedSynthDefs` | Number of synthdefs currently loaded |

### Error Metrics

Error counters for diagnosing issues.

| Property | Description |
|----------|-------------|
| `scsynthWasmErrors` | WASM execution errors in audio worklet |
| `oscInCorrupted` | Ring buffer message corruption detected |

## Node Tree Mirror

Beyond numeric metrics, SuperSonic mirrors the entire scsynth node tree to JavaScript via the same shared memory mechanism. This gives you a live view of every synth and group currently running - updated in real-time with zero OSC round-trip latency.

```javascript
const tree = supersonic.getTree();
// {
//   version: 42,        // Increments on every change
//   nodeCount: 5,       // Total nodes
//   droppedCount: 0,    // Overflow (capacity exceeded)
//   nodes: [...]        // All synths and groups
// }
```

Use `version` to skip re-renders when nothing changed - perfect for 60fps visualizations.

For the full API including node structure, tree traversal examples, and comparison with `/g_queryTree`, see [Node Tree API](API.md#node-tree-api).

## Example: Simple Monitor

```javascript
setInterval(() => {
  const metrics = supersonic.getMetrics();

  // Check for problems
  if (metrics.scsynthMessagesDropped > 0) {
    console.warn('Messages being dropped!');
  }

  // Monitor buffer usage
  if (metrics.inBufferUsed?.percentage > 80) {
    console.warn('Input buffer getting full:', metrics.inBufferUsed.percentage + '%');
  }

  // Track throughput
  console.log(`Processed: ${metrics.scsynthMessagesProcessed}, Sent: ${metrics.oscOutMessagesSent}`);
}, 100);
```

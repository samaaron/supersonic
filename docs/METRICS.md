# Metrics

Performance metrics for monitoring SuperSonic's internal state.

- **SAB mode**: Reads directly from SharedArrayBuffer with zero overhead
- **PostMessage mode**: Reads from a cached snapshot (worklet sends updates every 25ms by default)

## Quick Start

```javascript
// Poll metrics from your UI update loop
setInterval(() => {
  const metrics = supersonic.getMetrics();
  console.log('Processed:', metrics.workletMessagesProcessed);
  console.log('Dropped:', metrics.workletMessagesDropped);
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

### Worklet Metrics

These come from the audio worklet running the WASM engine.

| Property | Description |
|----------|-------------|
| `workletProcessCount` | Audio process() calls (cumulative) |
| `workletMessagesProcessed` | OSC messages processed by scsynth |
| `workletMessagesDropped` | Messages dropped (scheduler queue full) |
| `workletSchedulerDepth` | Current scheduler queue depth |
| `workletSchedulerMax` | Peak scheduler queue depth |
| `workletSchedulerDropped` | Messages dropped from scheduler |
| `workletSequenceGaps` | Sequence gaps detected (indicates lost messages) |

### Prescheduler Metrics

The prescheduler handles timed OSC bundles before they reach the audio worklet.

| Property | Description |
|----------|-------------|
| `preschedulerPending` | Events waiting in queue |
| `preschedulerPeak` | Peak queue depth |
| `preschedulerSent` | Bundles sent to worklet |
| `preschedulerRetriesSucceeded` | Writes that succeeded after retry |
| `preschedulerRetriesFailed` | Writes that failed after max retries |
| `preschedulerBundlesScheduled` | Total bundles scheduled |
| `preschedulerEventsCancelled` | Bundles cancelled before dispatch |
| `preschedulerTotalDispatches` | Dispatch cycles executed |
| `preschedulerMessagesRetried` | Total retry attempts |
| `preschedulerRetryQueueSize` | Current retry queue size |
| `preschedulerRetryQueueMax` | Peak retry queue size |
| `preschedulerBypassed` | Messages that bypassed prescheduler (direct writes) |

### OSC Input Metrics

Messages coming back from the engine.

| Property | Description |
|----------|-------------|
| `oscInMessagesReceived` | OSC replies received from scsynth |
| `oscInMessagesDropped` | Replies lost (sequence gaps or corruption) |
| `oscInBytesReceived` | Total bytes received |

### Debug Metrics

Debug output from the engine.

| Property | Description |
|----------|-------------|
| `debugMessagesReceived` | Debug messages received |
| `debugBytesReceived` | Total debug bytes received |

### Main Thread Metrics

Messages sent to the engine.

| Property | Description |
|----------|-------------|
| `mainMessagesSent` | OSC messages sent to scsynth |
| `mainBytesSent` | Total bytes sent |

### Buffer Usage (SAB mode only)

Ring buffer fill levels. These metrics require atomic reads from shared memory and are only available in SAB mode. In postMessage mode, these properties will be `undefined`.

| Property | Description |
|----------|-------------|
| `inBufferUsed.bytes` | Bytes in input buffer |
| `inBufferUsed.percentage` | Input buffer percentage used |
| `outBufferUsed.bytes` | Bytes in output buffer |
| `outBufferUsed.percentage` | Output buffer percentage used |
| `debugBufferUsed.bytes` | Bytes in debug buffer |
| `debugBufferUsed.percentage` | Debug buffer percentage used |

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

## Example: Simple Monitor

```javascript
setInterval(() => {
  const metrics = supersonic.getMetrics();

  // Check for problems
  if (metrics.workletMessagesDropped > 0) {
    console.warn('Messages being dropped!');
  }

  // Monitor buffer usage (SAB mode only)
  if (metrics.inBufferUsed?.percentage > 80) {
    console.warn('Input buffer getting full:', metrics.inBufferUsed.percentage + '%');
  }

  // Track throughput
  console.log(`Processed: ${metrics.workletMessagesProcessed}, Sent: ${metrics.mainMessagesSent}`);
}, 100);
```

## Performance

**SAB mode**: `getMetrics()` takes less than 0.1ms - it's just reading from shared memory.

**PostMessage mode**: `getMetrics()` reads from a cached snapshot that the worklet sends every 25ms (configurable via `snapshotIntervalMs`). The snapshot includes both metrics and node tree data in a single transfer.

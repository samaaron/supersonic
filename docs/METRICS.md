# Metrics

Performance metrics for monitoring SuperSonic's internal state.

- **SAB mode**: Reads directly from SharedArrayBuffer with zero overhead
- **PostMessage mode**: Reads from a cached snapshot (worklet sends updates every 50ms by default)

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

These come from the WASM scsynth engine running in the audio worklet.

| Property | Description |
|----------|-------------|
| `scsynthProcessCount` | Audio process() calls (cumulative) |
| `scsynthMessagesProcessed` | OSC messages processed by scsynth |
| `scsynthMessagesDropped` | Messages dropped (scheduler queue full) |
| `scsynthSchedulerDepth` | Current scheduler queue depth |
| `scsynthSchedulerPeakDepth` | Peak scheduler queue depth (high water mark) |
| `scsynthSchedulerCapacity` | Maximum scheduler queue size (compile-time constant) |
| `scsynthSchedulerDropped` | Messages dropped from scheduler |
| `scsynthSequenceGaps` | Sequence gaps detected (indicates lost messages) |
| `scsynthSchedulerLates` | Bundles executed after their scheduled time |

### Prescheduler Metrics

The prescheduler handles timed OSC bundles before they reach the audio worklet.

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
| `preschedulerBypassed` | Messages that bypassed prescheduler (direct writes) |
| `preschedulerCapacity` | Maximum pending events allowed |
| `preschedulerMinHeadroomMs` | Smallest time gap between JS prescheduler dispatch and scsynth scheduler execution |
| `preschedulerLates` | Bundles dispatched after their scheduled execution time |

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
| `inBufferUsedBytes` | Raw bytes in input buffer |
| `outBufferUsedBytes` | Raw bytes in output buffer |
| `debugBufferUsedBytes` | Raw bytes in debug buffer |
| `inBufferUsed.bytes` | Bytes in input buffer |
| `inBufferUsed.percentage` | Input buffer percentage used |
| `inBufferUsed.capacity` | Total input buffer capacity in bytes |
| `outBufferUsed.bytes` | Bytes in output buffer |
| `outBufferUsed.percentage` | Output buffer percentage used |
| `outBufferUsed.capacity` | Total output buffer capacity in bytes |
| `debugBufferUsed.bytes` | Bytes in debug buffer |
| `debugBufferUsed.percentage` | Debug buffer percentage used |
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

## Performance

**SAB mode**: `getMetrics()` takes less than 0.1ms - it's just reading from shared memory.

**PostMessage mode**: `getMetrics()` reads from a cached snapshot that the worklet sends every 50ms (configurable via `snapshotIntervalMs`). The snapshot includes both metrics and node tree data in a single transfer.

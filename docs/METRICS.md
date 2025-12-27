# Metrics

Performance metrics for monitoring SuperSonic's internal state.

- **SAB mode**: Reads directly from SharedArrayBuffer with zero overhead
- **PostMessage mode**: Reads from a cached snapshot (worklet sends updates every 25ms by default)

## Quick Start

```javascript
// Subscribe to periodic updates (default: every 100ms)
supersonic.on('metrics', (metrics) => {
  console.log('Processed:', metrics.workletMessagesProcessed);
  console.log('Dropped:', metrics.workletMessagesDropped);
});

// Or get a snapshot on demand
const metrics = supersonic.getMetrics();
```

## API

### `on('metrics', callback)`

Subscribe to periodic metrics updates. The timer runs from boot at 100ms intervals.

```javascript
const unsubscribe = supersonic.on('metrics', (metrics) => {
  // Called every 100ms by default
});

// Stop receiving updates
unsubscribe();
```

### `setMetricsInterval(ms)`

Change the polling interval and restart the timer.

```javascript
supersonic.setMetricsInterval(500);  // Update every 500ms (2Hz)
supersonic.setMetricsInterval(50);   // Update every 50ms (20Hz)
```

### `stopMetricsPolling()`

Stop the metrics timer entirely.

```javascript
supersonic.stopMetricsPolling();
```

### `getMetrics()`

Get a metrics snapshot on demand. Always available, regardless of polling state.

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

### Buffer Usage

Ring buffer fill levels.

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
supersonic.on('metrics', (metrics) => {
  // Check for problems
  if (metrics.workletMessagesDropped > 0) {
    console.warn('Messages being dropped!');
  }

  // Monitor buffer usage
  if (metrics.inBufferUsed.percentage > 80) {
    console.warn('Input buffer getting full:', metrics.inBufferUsed.percentage + '%');
  }

  // Track throughput
  console.log(`Processed: ${metrics.workletMessagesProcessed}, Sent: ${metrics.mainMessagesSent}`);
});
```

## Performance

**SAB mode**: `getMetrics()` takes less than 0.1ms - it's just reading from shared memory.

**PostMessage mode**: `getMetrics()` reads from a cached snapshot that the worklet sends every 25ms (configurable via `snapshotIntervalMs`). The snapshot includes both metrics and node tree data in a single transfer.

The default 100ms polling interval for the `'metrics'` event adds negligible CPU load in either mode.

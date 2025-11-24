# Metrics

Real-time performance metrics, read directly from shared memory.

## Quick Start

```javascript
// Receive periodic updates (default: every 100ms)
sonic.onMetricsUpdate = (metrics) => {
  console.log('Processed:', metrics.workletMessagesProcessed);
  console.log('Dropped:', metrics.workletMessagesDropped);
};

// Or get a snapshot on demand
const metrics = sonic.getMetrics();
```

## API

### `onMetricsUpdate`

Set a callback to receive periodic metrics updates. The timer runs from boot at 100ms intervals but does nothing until you set a callback.

```javascript
sonic.onMetricsUpdate = (metrics) => {
  // Called every 100ms by default
};

// Stop receiving updates
sonic.onMetricsUpdate = null;
```

### `setMetricsInterval(ms)`

Change the polling interval and restart the timer.

```javascript
sonic.setMetricsInterval(500);  // Update every 500ms (2Hz)
sonic.setMetricsInterval(50);   // Update every 50ms (20Hz)
```

### `stopMetricsPolling()`

Stop the metrics timer entirely.

```javascript
sonic.stopMetricsPolling();
```

### `getMetrics()`

Get a metrics snapshot on demand. Always available, regardless of polling state.

```javascript
const metrics = sonic.getMetrics();
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
sonic.onMetricsUpdate = (metrics) => {
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
};
```

## Performance Notes

Metrics are read directly from SharedArrayBuffer with no message passing overhead. A typical `getMetrics()` call takes less than 0.1ms. The default 100ms polling interval adds negligible CPU load.

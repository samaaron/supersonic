// SPDX-License-Identifier: MIT OR GPL-3.0-or-later
// Copyright (c) 2025 Sam Aaron

import * as MetricsOffsets from './metrics_offsets.js';
import { calculateAllControlIndices } from './control_offsets.js';

/**
 * Reads and aggregates metrics from SharedArrayBuffer or snapshot buffers
 */
export class MetricsReader {
  #sharedBuffer;
  #ringBufferBase;
  #bufferConstants;
  #mode;

  // Cached views (SAB mode only)
  #atomicView;
  #metricsView;
  #controlIndices;

  // Cached snapshot buffer (postMessage mode)
  #cachedSnapshotBuffer = null;

  // Merged array: slots 0-(SAB_METRICS_COUNT-1) from SAB/snapshot, SAB_METRICS_COUNT+ from context
  #mergedArray = new Uint32Array(MetricsOffsets.MERGED_ARRAY_SIZE);
  #mergedDV = new DataView(this.#mergedArray.buffer);

  /**
   * @param {Object} options
   * @param {string} options.mode - 'sab' or 'postMessage'
   * @param {SharedArrayBuffer} [options.sharedBuffer] - Required for SAB mode
   * @param {number} [options.ringBufferBase] - Required for SAB mode
   * @param {Object} [options.bufferConstants] - Buffer layout constants
   */
  constructor(options = {}) {
    this.#mode = options.mode || 'sab';
    this.#sharedBuffer = options.sharedBuffer || null;
    this.#ringBufferBase = options.ringBufferBase || 0;
    this.#bufferConstants = options.bufferConstants || null;
  }

  /**
   * Initialize shared views (SAB mode only)
   * Call after receiving bufferConstants from worklet
   */
  initSharedViews(sharedBuffer, ringBufferBase, bufferConstants) {
    this.#sharedBuffer = sharedBuffer;
    this.#ringBufferBase = ringBufferBase;
    this.#bufferConstants = bufferConstants;

    if (this.#mode === 'sab' && sharedBuffer && bufferConstants) {
      // Atomic view for reading control pointers
      this.#atomicView = new Int32Array(sharedBuffer);
      this.#controlIndices = calculateAllControlIndices(ringBufferBase, bufferConstants.CONTROL_START);

      // Cache metrics view for efficient reads
      const metricsBase = ringBufferBase + bufferConstants.METRICS_START;
      this.#metricsView = new Uint32Array(
        sharedBuffer,
        metricsBase,
        bufferConstants.METRICS_SIZE / 4
      );
    }
  }

  /**
   * Update cached snapshot buffer (postMessage mode)
   * @param {ArrayBuffer} buffer - Snapshot from worklet
   */
  updateSnapshot(buffer) {
    this.#cachedSnapshotBuffer = buffer;
  }

  /**
   * Get the cached snapshot buffer
   * @returns {ArrayBuffer|null}
   */
  getSnapshotBuffer() {
    return this.#cachedSnapshotBuffer;
  }

  /**
   * Get the metrics view (for direct access)
   * @returns {Uint32Array|null}
   */
  getMetricsView() {
    return this.#metricsView;
  }

  /**
   * Add to a metric in SharedArrayBuffer
   * @param {string} metric - Metric name
   * @param {number} [amount=1] - Amount to add
   */
  addMetric(metric, amount = 1) {
    if (!this.#metricsView) {
      return;
    }

    const offsets = {
      oscOutMessagesSent: MetricsOffsets.OSC_OUT_MESSAGES_SENT,
      oscOutBytesSent: MetricsOffsets.OSC_OUT_BYTES_SENT,
      preschedulerBypassed: MetricsOffsets.PRESCHEDULER_BYPASSED,
      bypassNonBundle: MetricsOffsets.BYPASS_NON_BUNDLE,
      bypassImmediate: MetricsOffsets.BYPASS_IMMEDIATE,
      bypassNearFuture: MetricsOffsets.BYPASS_NEAR_FUTURE,
      bypassLate: MetricsOffsets.BYPASS_LATE,
    };

    const offset = offsets[metric];
    if (offset !== undefined) {
      Atomics.add(this.#metricsView, offset, amount);
    }
  }

  /**
   * Overlay prescheduler metrics into the snapshot buffer
   * @param {Uint32Array} preschedulerMetrics - Metrics from prescheduler
   */
  overlayPreschedulerMetrics(preschedulerMetrics) {
    if (!this.#cachedSnapshotBuffer || !preschedulerMetrics) return;

    const metricsView = new Uint32Array(this.#cachedSnapshotBuffer, 0, MetricsOffsets.SAB_METRICS_COUNT);

    // Copy prescheduler metrics (offsets 9-21), excluding BYPASSED (22) and MAX_LATE_MS (23).
    // The worklet is source of truth for PRESCHEDULER_BYPASSED.
    const start = MetricsOffsets.PRESCHEDULER_START;
    const count = MetricsOffsets.PRESCHEDULER_COUNT - 2;
    metricsView.set(preschedulerMetrics.subarray(start, start + count), start);

    // Copy PRESCHEDULER_MAX_LATE_MS (tracked by prescheduler worker)
    metricsView[MetricsOffsets.PRESCHEDULER_MAX_LATE_MS] =
      preschedulerMetrics[MetricsOffsets.PRESCHEDULER_MAX_LATE_MS];
  }

  /**
   * Gather all metrics as a named object.
   * Uses updateMergedArray() as the single read path, then builds named properties.
   * @param {Object} context - Context with additional metric sources
   * @returns {Object} Combined metrics
   */
  gatherMetrics(context = {}) {
    // Single read path: populate the flat merged array from SAB/snapshot + context
    this.updateMergedArray(context);
    const m = this.#mergedArray;

    const metrics = {
      // scsynth metrics
      scsynthProcessCount: m[MetricsOffsets.SCSYNTH_PROCESS_COUNT],
      scsynthMessagesProcessed: m[MetricsOffsets.SCSYNTH_MESSAGES_PROCESSED],
      scsynthMessagesDropped: m[MetricsOffsets.SCSYNTH_MESSAGES_DROPPED],
      scsynthSchedulerDepth: m[MetricsOffsets.SCSYNTH_SCHEDULER_DEPTH],
      scsynthSchedulerPeakDepth: m[MetricsOffsets.SCSYNTH_SCHEDULER_PEAK_DEPTH],
      scsynthSchedulerDropped: m[MetricsOffsets.SCSYNTH_SCHEDULER_DROPPED],
      scsynthSequenceGaps: m[MetricsOffsets.SCSYNTH_SEQUENCE_GAPS],
      scsynthSchedulerLates: m[MetricsOffsets.SCSYNTH_SCHEDULER_LATES],
      scsynthSchedulerMaxLateMs: m[MetricsOffsets.SCSYNTH_SCHEDULER_MAX_LATE_MS],
      scsynthSchedulerLastLateMs: m[MetricsOffsets.SCSYNTH_SCHEDULER_LAST_LATE_MS],
      scsynthSchedulerLastLateTick: m[MetricsOffsets.SCSYNTH_SCHEDULER_LAST_LATE_TICK],

      // Prescheduler metrics
      preschedulerPending: m[MetricsOffsets.PRESCHEDULER_PENDING],
      preschedulerPendingPeak: m[MetricsOffsets.PRESCHEDULER_PENDING_PEAK],
      preschedulerDispatched: m[MetricsOffsets.PRESCHEDULER_DISPATCHED],
      preschedulerRetriesSucceeded: m[MetricsOffsets.PRESCHEDULER_RETRIES_SUCCEEDED],
      preschedulerRetriesFailed: m[MetricsOffsets.PRESCHEDULER_RETRIES_FAILED],
      preschedulerBundlesScheduled: m[MetricsOffsets.PRESCHEDULER_BUNDLES_SCHEDULED],
      preschedulerEventsCancelled: m[MetricsOffsets.PRESCHEDULER_EVENTS_CANCELLED],
      preschedulerTotalDispatches: m[MetricsOffsets.PRESCHEDULER_TOTAL_DISPATCHES],
      preschedulerMessagesRetried: m[MetricsOffsets.PRESCHEDULER_MESSAGES_RETRIED],
      preschedulerRetryQueueSize: m[MetricsOffsets.PRESCHEDULER_RETRY_QUEUE_SIZE],
      preschedulerRetryQueuePeak: m[MetricsOffsets.PRESCHEDULER_RETRY_QUEUE_PEAK],
      preschedulerBypassed: m[MetricsOffsets.PRESCHEDULER_BYPASSED],
      preschedulerMinHeadroomMs: m[MetricsOffsets.PRESCHEDULER_MIN_HEADROOM_MS],
      preschedulerLates: m[MetricsOffsets.PRESCHEDULER_LATES],
      preschedulerMaxLateMs: m[MetricsOffsets.PRESCHEDULER_MAX_LATE_MS],

      // OSC In/Out metrics
      oscInMessagesReceived: m[MetricsOffsets.OSC_IN_MESSAGES_RECEIVED],
      oscInMessagesDropped: m[MetricsOffsets.OSC_IN_DROPPED_MESSAGES],
      oscInBytesReceived: m[MetricsOffsets.OSC_IN_BYTES_RECEIVED],
      debugMessagesReceived: m[MetricsOffsets.DEBUG_MESSAGES_RECEIVED],
      debugBytesReceived: m[MetricsOffsets.DEBUG_BYTES_RECEIVED],
      oscOutMessagesSent: m[MetricsOffsets.OSC_OUT_MESSAGES_SENT],
      oscOutBytesSent: m[MetricsOffsets.OSC_OUT_BYTES_SENT],

      // Error metrics
      scsynthWasmErrors: m[MetricsOffsets.SCSYNTH_WASM_ERRORS],
      oscInCorrupted: m[MetricsOffsets.OSC_IN_CORRUPTED],
      ringBufferDirectWriteFails: m[MetricsOffsets.RING_BUFFER_DIRECT_WRITE_FAILS],

      // Bypass categories
      bypassNonBundle: m[MetricsOffsets.BYPASS_NON_BUNDLE],
      bypassImmediate: m[MetricsOffsets.BYPASS_IMMEDIATE],
      bypassNearFuture: m[MetricsOffsets.BYPASS_NEAR_FUTURE],
      bypassLate: m[MetricsOffsets.BYPASS_LATE],

      // Mode
      mode: this.#mode,
    };

    // Build buffer usage objects from raw metrics
    const bc = this.#bufferConstants;
    if (m[MetricsOffsets.IN_BUFFER_USED_BYTES] !== undefined && bc) {
      metrics.inBufferUsed = {
        bytes: m[MetricsOffsets.IN_BUFFER_USED_BYTES],
        percentage: (m[MetricsOffsets.IN_BUFFER_USED_BYTES] / bc.IN_BUFFER_SIZE) * 100,
        peakBytes: m[MetricsOffsets.IN_BUFFER_PEAK_BYTES],
        peakPercentage: (m[MetricsOffsets.IN_BUFFER_PEAK_BYTES] / bc.IN_BUFFER_SIZE) * 100,
        capacity: bc.IN_BUFFER_SIZE,
      };
      metrics.outBufferUsed = {
        bytes: m[MetricsOffsets.OUT_BUFFER_USED_BYTES],
        percentage: (m[MetricsOffsets.OUT_BUFFER_USED_BYTES] / bc.OUT_BUFFER_SIZE) * 100,
        peakBytes: m[MetricsOffsets.OUT_BUFFER_PEAK_BYTES],
        peakPercentage: (m[MetricsOffsets.OUT_BUFFER_PEAK_BYTES] / bc.OUT_BUFFER_SIZE) * 100,
        capacity: bc.OUT_BUFFER_SIZE,
      };
      metrics.debugBufferUsed = {
        bytes: m[MetricsOffsets.DEBUG_BUFFER_USED_BYTES],
        percentage: (m[MetricsOffsets.DEBUG_BUFFER_USED_BYTES] / bc.DEBUG_BUFFER_SIZE) * 100,
        peakBytes: m[MetricsOffsets.DEBUG_BUFFER_PEAK_BYTES],
        peakPercentage: (m[MetricsOffsets.DEBUG_BUFFER_PEAK_BYTES] / bc.DEBUG_BUFFER_SIZE) * 100,
        capacity: bc.DEBUG_BUFFER_SIZE,
      };
    }

    if (bc?.scheduler_slot_count !== undefined) {
      metrics.scsynthSchedulerCapacity = bc.scheduler_slot_count;
    }

    // Context-provided metrics
    if (context.driftOffsetMs !== undefined) metrics.driftOffsetMs = context.driftOffsetMs;
    if (context.ntpStartTime !== undefined) metrics.ntpStartTime = context.ntpStartTime;
    if (context.clockOffsetMs !== undefined) metrics.clockOffsetMs = context.clockOffsetMs;
    if (context.audioContextState) metrics.audioContextState = context.audioContextState;
    if (context.bufferPoolStats) {
      metrics.bufferPoolUsedBytes = context.bufferPoolStats.used.size;
      metrics.bufferPoolAvailableBytes = context.bufferPoolStats.available;
      metrics.bufferPoolAllocations = context.bufferPoolStats.used.count;
    }
    if (context.loadedSynthDefsCount !== undefined) metrics.loadedSynthDefs = context.loadedSynthDefsCount;
    if (context.preschedulerCapacity !== undefined) metrics.preschedulerCapacity = context.preschedulerCapacity;

    // Audio diagnostics from merged array context slots
    metrics.audioHealthPct = context.audioHealthPct ?? 100;
    metrics.hasPlaybackStats = !!context.playbackStats;
    if (context.playbackStats) {
      metrics.glitchCount = context.playbackStats.fallbackFramesEvents ?? 0;
      metrics.glitchDurationMs = Math.round((context.playbackStats.fallbackFramesDuration ?? 0) * 1000);
      metrics.averageLatencyUs = Math.round((context.playbackStats.averageLatency ?? 0) * 1_000_000);
      metrics.maxLatencyUs = Math.round((context.playbackStats.maximumLatency ?? 0) * 1_000_000);
      metrics.totalFramesDurationMs = Math.round((context.playbackStats.totalFramesDuration ?? 0) * 1000);
    } else {
      metrics.glitchCount = 0;
      metrics.glitchDurationMs = 0;
      metrics.averageLatencyUs = 0;
      metrics.maxLatencyUs = 0;
      metrics.totalFramesDurationMs = 0;
    }

    if (this.#mode === 'postMessage' && context.transportMetrics) {
      Object.assign(metrics, context.transportMetrics);
    }

    return metrics;
  }

  /**
   * Update the merged array with current metrics from SAB/snapshot + context.
   * Zero-allocation: writes into the pre-allocated Uint32Array.
   * @param {Object} context - Context with additional metric sources
   */
  updateMergedArray(context = {}) {
    const arr = this.#mergedArray;

    // Copy slots 0-45 from SAB or snapshot
    if (this.#mode === 'postMessage') {
      if (context.preschedulerMetrics) {
        this.overlayPreschedulerMetrics(context.preschedulerMetrics);
      }
      if (this.#cachedSnapshotBuffer) {
        const view = new Uint32Array(this.#cachedSnapshotBuffer, 0, MetricsOffsets.SAB_METRICS_COUNT);
        arr.set(view);
      }
      // PM mode: overlay transport metrics into the merged array
      if (context.transportMetrics) {
        if (context.transportMetrics.oscOutMessagesSent !== undefined) {
          arr[MetricsOffsets.OSC_OUT_MESSAGES_SENT] = context.transportMetrics.oscOutMessagesSent;
        }
        if (context.transportMetrics.oscOutBytesSent !== undefined) {
          arr[MetricsOffsets.OSC_OUT_BYTES_SENT] = context.transportMetrics.oscOutBytesSent;
        }
        if (context.transportMetrics.preschedulerBypassed !== undefined) {
          arr[MetricsOffsets.PRESCHEDULER_BYPASSED] = context.transportMetrics.preschedulerBypassed;
        }
        if (context.transportMetrics.bypassNonBundle !== undefined) {
          arr[MetricsOffsets.BYPASS_NON_BUNDLE] = context.transportMetrics.bypassNonBundle;
        }
        if (context.transportMetrics.bypassImmediate !== undefined) {
          arr[MetricsOffsets.BYPASS_IMMEDIATE] = context.transportMetrics.bypassImmediate;
        }
        if (context.transportMetrics.bypassNearFuture !== undefined) {
          arr[MetricsOffsets.BYPASS_NEAR_FUTURE] = context.transportMetrics.bypassNearFuture;
        }
        if (context.transportMetrics.bypassLate !== undefined) {
          arr[MetricsOffsets.BYPASS_LATE] = context.transportMetrics.bypassLate;
        }
      }
    } else if (this.#metricsView) {
      arr.set(this.#metricsView);
    }

    // Context slots 46+
    // driftOffsetMs and clockOffsetMs are signed — store as int32 via DataView
    const dv = this.#mergedDV;
    dv.setInt32(MetricsOffsets.CTX_DRIFT_OFFSET_MS * 4, context.driftOffsetMs ?? 0, true);
    dv.setInt32(MetricsOffsets.CTX_CLOCK_OFFSET_MS * 4, context.clockOffsetMs ?? 0, true);

    // audioContextState enum: unknown=0, running=1, suspended=2, closed=3, interrupted=4
    const stateStr = context.audioContextState || 'unknown';
    const stateEnum = { unknown: 0, running: 1, suspended: 2, closed: 3, interrupted: 4 };
    arr[MetricsOffsets.CTX_AUDIO_CONTEXT_STATE] = stateEnum[stateStr] ?? 0;

    // Buffer pool stats
    if (context.bufferPoolStats) {
      arr[MetricsOffsets.CTX_BUFFER_POOL_USED_BYTES] = context.bufferPoolStats.used?.size ?? 0;
      arr[MetricsOffsets.CTX_BUFFER_POOL_AVAILABLE_BYTES] = context.bufferPoolStats.available ?? 0;
      arr[MetricsOffsets.CTX_BUFFER_POOL_ALLOCATIONS] = context.bufferPoolStats.used?.count ?? 0;
    }

    arr[MetricsOffsets.CTX_LOADED_SYNTH_DEFS] = context.loadedSynthDefsCount ?? 0;

    // Static capacities from bufferConstants
    const bc = this.#bufferConstants;
    arr[MetricsOffsets.CTX_SCSYNTH_SCHEDULER_CAPACITY] = bc?.scheduler_slot_count ?? 0;
    arr[MetricsOffsets.CTX_PRESCHEDULER_CAPACITY] = context.preschedulerCapacity ?? 0;
    arr[MetricsOffsets.CTX_IN_BUFFER_CAPACITY] = bc?.IN_BUFFER_SIZE ?? 0;
    arr[MetricsOffsets.CTX_OUT_BUFFER_CAPACITY] = bc?.OUT_BUFFER_SIZE ?? 0;
    arr[MetricsOffsets.CTX_DEBUG_BUFFER_CAPACITY] = bc?.DEBUG_BUFFER_SIZE ?? 0;

    // Mode enum: 0=sab, 1=postMessage
    arr[MetricsOffsets.CTX_MODE] = this.#mode === 'sab' ? 0 : 1;

    // Audio diagnostics [59-66]
    arr[MetricsOffsets.CTX_HAS_PLAYBACK_STATS] = context.playbackStats ? 1 : 0;
    if (context.playbackStats) {
      arr[MetricsOffsets.CTX_GLITCH_COUNT] = context.playbackStats.fallbackFramesEvents ?? 0;
      arr[MetricsOffsets.CTX_GLITCH_DURATION_MS] = Math.round((context.playbackStats.fallbackFramesDuration ?? 0) * 1000);
      arr[MetricsOffsets.CTX_AVERAGE_LATENCY_US] = Math.round((context.playbackStats.averageLatency ?? 0) * 1_000_000);
      arr[MetricsOffsets.CTX_MAX_LATENCY_US] = Math.round((context.playbackStats.maximumLatency ?? 0) * 1_000_000);
      arr[MetricsOffsets.CTX_TOTAL_FRAMES_DURATION_MS] = Math.round((context.playbackStats.totalFramesDuration ?? 0) * 1000);
    }
    arr[MetricsOffsets.CTX_AUDIO_HEALTH_PCT] = context.audioHealthPct ?? 100;
  }

  /**
   * Get the merged array reference (same Uint32Array every call — zero allocation).
   * Call updateMergedArray() first to refresh values.
   * @returns {Uint32Array}
   */
  getMergedArray() {
    return this.#mergedArray;
  }

  /**
   * Get buffer constants
   * @returns {Object|null}
   */
  get bufferConstants() {
    return this.#bufferConstants;
  }

  /**
   * Get ring buffer base address
   * @returns {number}
   */
  get ringBufferBase() {
    return this.#ringBufferBase;
  }

  /**
   * Get shared buffer
   * @returns {SharedArrayBuffer|null}
   */
  get sharedBuffer() {
    return this.#sharedBuffer;
  }
}

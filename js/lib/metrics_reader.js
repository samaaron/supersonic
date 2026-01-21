// SPDX-License-Identifier: MIT OR GPL-3.0-or-later
// Copyright (c) 2025 Sam Aaron

import * as MetricsOffsets from './metrics_offsets.js';

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

  // Cached snapshot buffer (postMessage mode)
  #cachedSnapshotBuffer = null;

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
    };

    const offset = offsets[metric];
    if (offset !== undefined) {
      Atomics.add(this.#metricsView, offset, amount);
    }
  }

  /**
   * Parse metrics from a Uint32Array buffer
   * Layout defined in src/shared_memory.h and js/lib/metrics_offsets.js
   * @param {Uint32Array} m - Metrics buffer
   * @returns {Object} Metrics object
   */
  parseMetricsBuffer(m) {
    return {
      // scsynth metrics (written by WASM)
      scsynthProcessCount: m[MetricsOffsets.SCSYNTH_PROCESS_COUNT],
      scsynthMessagesProcessed: m[MetricsOffsets.SCSYNTH_MESSAGES_PROCESSED],
      scsynthMessagesDropped: m[MetricsOffsets.SCSYNTH_MESSAGES_DROPPED],
      scsynthSchedulerDepth: m[MetricsOffsets.SCSYNTH_SCHEDULER_DEPTH],
      scsynthSchedulerPeakDepth: m[MetricsOffsets.SCSYNTH_SCHEDULER_PEAK_DEPTH],
      scsynthSchedulerDropped: m[MetricsOffsets.SCSYNTH_SCHEDULER_DROPPED],
      scsynthSequenceGaps: m[MetricsOffsets.SCSYNTH_SEQUENCE_GAPS],
      scsynthSchedulerLates: m[MetricsOffsets.SCSYNTH_SCHEDULER_LATES],
      // scsynthSchedulerCapacity is added in gatherMetrics() from bufferConstants

      // Prescheduler metrics (written by osc_out_prescheduler_worker.js)
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

      // OSC In metrics (written by osc_in_worker.js)
      oscInMessagesReceived: m[MetricsOffsets.OSC_IN_MESSAGES_RECEIVED],
      oscInMessagesDropped: m[MetricsOffsets.OSC_IN_DROPPED_MESSAGES],
      oscInBytesReceived: m[MetricsOffsets.OSC_IN_BYTES_RECEIVED],

      // Debug metrics (written by debug_worker.js)
      debugMessagesReceived: m[MetricsOffsets.DEBUG_MESSAGES_RECEIVED],
      debugBytesReceived: m[MetricsOffsets.DEBUG_BYTES_RECEIVED],

      // OSC Out metrics (written by supersonic.js main thread)
      oscOutMessagesSent: m[MetricsOffsets.OSC_OUT_MESSAGES_SENT],
      oscOutBytesSent: m[MetricsOffsets.OSC_OUT_BYTES_SENT],

      // Preschedule timing metrics (written by osc_out_prescheduler_worker.js)
      preschedulerMinHeadroomMs: m[MetricsOffsets.PRESCHEDULER_MIN_HEADROOM_MS],
      preschedulerLates: m[MetricsOffsets.PRESCHEDULER_LATES],

      // Error metrics
      scsynthWasmErrors: m[MetricsOffsets.SCSYNTH_WASM_ERRORS],
      oscInCorrupted: m[MetricsOffsets.OSC_IN_CORRUPTED],

      // Ring buffer usage (written by WASM during process())
      inBufferUsedBytes: m[MetricsOffsets.IN_BUFFER_USED_BYTES],
      outBufferUsedBytes: m[MetricsOffsets.OUT_BUFFER_USED_BYTES],
      debugBufferUsedBytes: m[MetricsOffsets.DEBUG_BUFFER_USED_BYTES],
    };
  }

  /**
   * Get metrics from SharedArrayBuffer (SAB mode)
   * @returns {Object|null}
   */
  getSABMetrics() {
    if (!this.#metricsView) {
      return null;
    }
    return this.parseMetricsBuffer(this.#metricsView);
  }

  /**
   * Get buffer usage statistics from SAB head/tail pointers
   * @returns {Object|null}
   */
  getBufferUsage() {
    if (!this.#atomicView || !this.#bufferConstants || !this.#ringBufferBase) {
      return null;
    }

    const controlBase = this.#ringBufferBase + this.#bufferConstants.CONTROL_START;
    const bc = this.#bufferConstants;

    // Read head/tail pointers
    const view = this.#atomicView;
    const inHead = Atomics.load(view, (controlBase + 0) / 4);
    const inTail = Atomics.load(view, (controlBase + 4) / 4);
    const outHead = Atomics.load(view, (controlBase + 8) / 4);
    const outTail = Atomics.load(view, (controlBase + 12) / 4);
    const debugHead = Atomics.load(view, (controlBase + 16) / 4);
    const debugTail = Atomics.load(view, (controlBase + 20) / 4);

    // Calculate bytes used (accounting for wrap-around)
    const inUsed = (inHead - inTail + bc.IN_BUFFER_SIZE) % bc.IN_BUFFER_SIZE;
    const outUsed = (outHead - outTail + bc.OUT_BUFFER_SIZE) % bc.OUT_BUFFER_SIZE;
    const debugUsed = (debugHead - debugTail + bc.DEBUG_BUFFER_SIZE) % bc.DEBUG_BUFFER_SIZE;

    return {
      inBufferUsed: {
        bytes: inUsed,
        percentage: (inUsed / bc.IN_BUFFER_SIZE) * 100,
        capacity: bc.IN_BUFFER_SIZE,
      },
      outBufferUsed: {
        bytes: outUsed,
        percentage: (outUsed / bc.OUT_BUFFER_SIZE) * 100,
        capacity: bc.OUT_BUFFER_SIZE,
      },
      debugBufferUsed: {
        bytes: debugUsed,
        percentage: (debugUsed / bc.DEBUG_BUFFER_SIZE) * 100,
        capacity: bc.DEBUG_BUFFER_SIZE,
      },
    };
  }

  /**
   * Overlay prescheduler metrics into the snapshot buffer
   * @param {Uint32Array} preschedulerMetrics - Metrics from prescheduler
   */
  overlayPreschedulerMetrics(preschedulerMetrics) {
    if (!this.#cachedSnapshotBuffer || !preschedulerMetrics) return;

    // Get a view into the metrics portion of the snapshot buffer
    const metricsView = new Uint32Array(this.#cachedSnapshotBuffer, 0, 32);

    // Single memcpy of ALL contiguous prescheduler metrics (offsets 8-21)
    const start = MetricsOffsets.PRESCHEDULER_START;
    const count = MetricsOffsets.PRESCHEDULER_COUNT;
    metricsView.set(preschedulerMetrics.subarray(start, start + count), start);
  }

  /**
   * Gather all metrics from appropriate source based on mode
   * @param {Object} context - Context with additional metric sources
   * @returns {Object} Combined metrics
   */
  gatherMetrics(context = {}) {
    let metrics;

    if (this.#mode === 'postMessage') {
      // Overlay prescheduler metrics if available
      if (context.preschedulerMetrics) {
        this.overlayPreschedulerMetrics(context.preschedulerMetrics);
      }

      // Read metrics from snapshot buffer
      if (this.#cachedSnapshotBuffer) {
        const metricsView = new Uint32Array(this.#cachedSnapshotBuffer, 0, 36);
        metrics = this.parseMetricsBuffer(metricsView);
      } else {
        metrics = {};
      }
    } else {
      // SAB mode: read directly from SharedArrayBuffer
      metrics = this.getSABMetrics() || {};
    }

    // Build buffer usage objects from raw metrics (works in both modes)
    // WASM calculates and writes these during process()
    if (metrics.inBufferUsedBytes !== undefined && this.#bufferConstants) {
      const bc = this.#bufferConstants;
      metrics.inBufferUsed = {
        bytes: metrics.inBufferUsedBytes,
        percentage: (metrics.inBufferUsedBytes / bc.IN_BUFFER_SIZE) * 100,
        capacity: bc.IN_BUFFER_SIZE,
      };
      metrics.outBufferUsed = {
        bytes: metrics.outBufferUsedBytes,
        percentage: (metrics.outBufferUsedBytes / bc.OUT_BUFFER_SIZE) * 100,
        capacity: bc.OUT_BUFFER_SIZE,
      };
      metrics.debugBufferUsed = {
        bytes: metrics.debugBufferUsedBytes,
        percentage: (metrics.debugBufferUsedBytes / bc.DEBUG_BUFFER_SIZE) * 100,
        capacity: bc.DEBUG_BUFFER_SIZE,
      };

      // Remove raw byte values (now captured in derived objects above)
      delete metrics.inBufferUsedBytes;
      delete metrics.outBufferUsedBytes;
      delete metrics.debugBufferUsedBytes;
    }

    // Add mode so clients know what metrics are available
    metrics.mode = this.#mode;

    // Add scheduler capacity from buffer constants (compile-time value)
    // Note: worklet exports as lowercase 'scheduler_slot_count'
    if (this.#bufferConstants?.scheduler_slot_count !== undefined) {
      metrics.scsynthSchedulerCapacity = this.#bufferConstants.scheduler_slot_count;
    }

    // Add context-provided metrics
    if (context.driftOffsetMs !== undefined) {
      metrics.driftOffsetMs = context.driftOffsetMs;
    }
    if (context.audioContextState) {
      metrics.audioContextState = context.audioContextState;
    }
    if (context.bufferPoolStats) {
      metrics.bufferPoolUsedBytes = context.bufferPoolStats.used.size;
      metrics.bufferPoolAvailableBytes = context.bufferPoolStats.available;
      metrics.bufferPoolAllocations = context.bufferPoolStats.used.count;
    }
    if (context.loadedSynthDefsCount !== undefined) {
      metrics.loadedSynthDefs = context.loadedSynthDefsCount;
    }
    if (context.preschedulerCapacity !== undefined) {
      metrics.preschedulerCapacity = context.preschedulerCapacity;
    }

    // In postMessage mode, merge transport metrics (sent/received counters)
    // These aren't in the snapshot buffer since they're tracked on the main thread
    // Transport now uses canonical metric names, so we can merge directly
    if (this.#mode === 'postMessage' && context.transportMetrics) {
      Object.assign(metrics, context.transportMetrics);
    }

    return metrics;
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

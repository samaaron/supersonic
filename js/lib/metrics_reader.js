/*
    SuperSonic - SuperCollider AudioWorklet WebAssembly port
    Copyright (c) 2025 Sam Aaron

    Based on SuperCollider by James McCartney and community
    GPL v3 or later
*/

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
      mainMessagesSent: MetricsOffsets.MESSAGES_SENT,
      mainBytesSent: MetricsOffsets.BYTES_SENT,
      preschedulerBypassed: MetricsOffsets.DIRECT_WRITES,
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
      // Worklet metrics (written by WASM)
      workletProcessCount: m[MetricsOffsets.PROCESS_COUNT],
      workletMessagesProcessed: m[MetricsOffsets.MESSAGES_PROCESSED],
      workletMessagesDropped: m[MetricsOffsets.MESSAGES_DROPPED],
      workletSchedulerDepth: m[MetricsOffsets.SCHEDULER_QUEUE_DEPTH],
      workletSchedulerMax: m[MetricsOffsets.SCHEDULER_QUEUE_MAX],
      workletSchedulerDropped: m[MetricsOffsets.SCHEDULER_QUEUE_DROPPED],
      workletSequenceGaps: m[MetricsOffsets.SEQUENCE_GAPS],

      // PreScheduler metrics (written by osc_out_prescheduler_worker.js)
      preschedulerPending: m[MetricsOffsets.PRESCHEDULER_PENDING],
      preschedulerPeak: m[MetricsOffsets.PRESCHEDULER_PEAK],
      preschedulerSent: m[MetricsOffsets.PRESCHEDULER_SENT],
      preschedulerRetriesSucceeded: m[MetricsOffsets.RETRIES_SUCCEEDED],
      preschedulerRetriesFailed: m[MetricsOffsets.RETRIES_FAILED],
      preschedulerBundlesScheduled: m[MetricsOffsets.BUNDLES_SCHEDULED],
      preschedulerEventsCancelled: m[MetricsOffsets.EVENTS_CANCELLED],
      preschedulerTotalDispatches: m[MetricsOffsets.TOTAL_DISPATCHES],
      preschedulerMessagesRetried: m[MetricsOffsets.MESSAGES_RETRIED],
      preschedulerRetryQueueSize: m[MetricsOffsets.RETRY_QUEUE_SIZE],
      preschedulerRetryQueueMax: m[MetricsOffsets.RETRY_QUEUE_MAX],
      preschedulerBypassed: m[MetricsOffsets.DIRECT_WRITES],

      // OSC In metrics (written by osc_in_worker.js)
      oscInMessagesReceived: m[MetricsOffsets.OSC_IN_MESSAGES_RECEIVED],
      oscInMessagesDropped: m[MetricsOffsets.OSC_IN_DROPPED_MESSAGES],
      oscInBytesReceived: m[MetricsOffsets.OSC_IN_BYTES_RECEIVED],

      // Debug metrics (written by debug_worker.js)
      debugMessagesReceived: m[MetricsOffsets.DEBUG_MESSAGES_RECEIVED],
      debugBytesReceived: m[MetricsOffsets.DEBUG_BYTES_RECEIVED],

      // Main thread metrics (written by supersonic.js)
      mainMessagesSent: m[MetricsOffsets.MESSAGES_SENT],
      mainBytesSent: m[MetricsOffsets.BYTES_SENT],
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
      },
      outBufferUsed: {
        bytes: outUsed,
        percentage: (outUsed / bc.OUT_BUFFER_SIZE) * 100,
      },
      debugBufferUsed: {
        bytes: debugUsed,
        percentage: (debugUsed / bc.DEBUG_BUFFER_SIZE) * 100,
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

    // Single memcpy of contiguous prescheduler metrics (offsets 6-16)
    const start = MetricsOffsets.PRESCHEDULER_PENDING;  // 6
    const count = MetricsOffsets.RETRY_QUEUE_MAX - start + 1;  // 11
    metricsView.set(preschedulerMetrics.subarray(start, start + count), start);
  }

  /**
   * Gather all metrics from appropriate source based on mode
   * @param {Object} context - Context with additional metric sources
   * @returns {Object} Combined metrics
   */
  gatherMetrics(context = {}) {
    const startTime = performance.now();
    let metrics;

    if (this.#mode === 'postMessage') {
      // Overlay prescheduler metrics if available
      if (context.preschedulerMetrics) {
        this.overlayPreschedulerMetrics(context.preschedulerMetrics);
      }

      // Read metrics from snapshot buffer
      if (this.#cachedSnapshotBuffer) {
        const metricsView = new Uint32Array(this.#cachedSnapshotBuffer, 0, 32);
        metrics = this.parseMetricsBuffer(metricsView);
      } else {
        metrics = {};
      }
    } else {
      // SAB mode: read directly from SharedArrayBuffer
      metrics = this.getSABMetrics() || {};

      // Buffer usage (calculated from SAB head/tail pointers)
      const bufferUsage = this.getBufferUsage();
      if (bufferUsage) {
        Object.assign(metrics, bufferUsage);
      }
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

    const totalDuration = performance.now() - startTime;
    if (totalDuration > 1) {
      console.warn(`[MetricsReader] Slow metrics gathering: ${totalDuration.toFixed(2)}ms`);
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

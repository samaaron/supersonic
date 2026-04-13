// SPDX-License-Identifier: MIT OR GPL-3.0-or-later
// Copyright (c) 2025 Sam Aaron

// NTP epoch offset: seconds between 1900-01-01 (NTP epoch) and 1970-01-01 (Unix epoch)
const NTP_EPOCH_OFFSET = 2208988800;

// Drift offset update interval in milliseconds.
// At 100 ppm crystal drift, this keeps error within ~0.1ms.
const DRIFT_UPDATE_INTERVAL_MS = 1000;

// Delay before calculating initial drift at boot (ms).
// Allows enough contextTime to elapse for accurate measurement.
const INITIAL_DRIFT_DELAY_MS = 500;

/**
 * Calculate current NTP time from performance timestamp
 * @param {number} performanceTimeMs - performance.timeOrigin + timestamp.performanceTime
 * @returns {number} Current NTP time in seconds
 */
function calculateCurrentNTP(performanceTimeMs) {
  return performanceTimeMs / 1000 + NTP_EPOCH_OFFSET;
}

/**
 * Calculate NTP time when AudioContext started
 * @param {number} currentNTP - Current NTP time in seconds
 * @param {number} contextTime - Current AudioContext.currentTime
 * @returns {number} NTP time at AudioContext start
 */
function calculateNTPStartTime(currentNTP, contextTime) {
  return currentNTP - contextTime;
}

/**
 * Calculate drift between expected and actual AudioContext time
 * Positive = AudioContext running slow, Negative = running fast
 * @param {number} expectedContextTime
 * @param {number} actualContextTime
 * @returns {number} Drift in microseconds (rounded to integer)
 */
function calculateDriftUs(expectedContextTime, actualContextTime) {
  const driftSeconds = expectedContextTime - actualContextTime;
  return Math.round(driftSeconds * 1000000);
}

/**
 * Manages NTP timing synchronization between AudioContext and wall clock.
 * Measures drift periodically and applies correction for accurate bundle scheduling.
 */
export class NTPTiming {
  #mode;
  #audioContext;
  #workletPort;
  #bufferConstants;
  #ringBufferBase;

  // Cached SAB views (SAB mode only)
  #ntpStartView;
  #driftView;
  #clockOffsetView;

  // Local storage (postMessage mode, or fallback)
  #initialNTPStartTime;
  #localDriftMs = 0;
  #localClockOffsetMs = 0;

  // Drift update timer
  #driftOffsetTimer = null;

  /**
   * @param {Object} options
   * @param {string} options.mode - 'sab' or 'postMessage'
   * @param {AudioContext} options.audioContext
   * @param {MessagePort} [options.workletPort] - Required for postMessage mode
   */
  constructor(options = {}) {
    this.#mode = options.mode || 'sab';
    this.#audioContext = options.audioContext;
    this.#workletPort = options.workletPort || null;
  }

  /**
   * Write NTP start time to shared memory with a store-release fence.
   * Float64Array doesn't support Atomics, so we write via DataView and
   * fence with an Atomics.store on the adjacent drift Int32. The C++ side
   * reads drift with memory_order_acquire before reading the float64,
   * ensuring the double is fully visible.
   */
  #writeNtpStartTime(value) {
    this.#ntpStartView.setFloat64(0, value, true); // little-endian
    // Store-release fence: the C++ acquire-load on drift_offset will
    // see all prior stores including the float64 above.
    if (this.#driftView) {
      Atomics.store(this.#driftView, 0, Atomics.load(this.#driftView, 0));
    }
  }

  /**
   * Initialize shared views for SAB mode
   * @param {SharedArrayBuffer} sharedBuffer
   * @param {number} ringBufferBase
   * @param {Object} bufferConstants
   */
  initSharedViews(sharedBuffer, ringBufferBase, bufferConstants) {
    this.#ringBufferBase = ringBufferBase;
    this.#bufferConstants = bufferConstants;

    if (this.#mode === 'sab' && sharedBuffer && bufferConstants) {
      // Use DataView for NTP start time writes — Float64Array doesn't support Atomics,
      // so a plain assignment risks a torn read on the audio thread.
      // After writing, we fence via Atomics.store on the adjacent drift Int32.
      this.#ntpStartView = new DataView(
        sharedBuffer,
        ringBufferBase + bufferConstants.NTP_START_TIME_START,
        8
      );
      this.#driftView = new Int32Array(
        sharedBuffer,
        ringBufferBase + bufferConstants.DRIFT_OFFSET_START,
        1
      );
      this.#clockOffsetView = new Int32Array(
        sharedBuffer,
        ringBufferBase + bufferConstants.GLOBAL_OFFSET_START,
        1
      );
    }
  }

  /**
   * Set worklet port (for postMessage mode)
  /**
   * Update audio context reference (for recovery)
   * @param {AudioContext} audioContext
   */
  updateAudioContext(audioContext) {
    this.#audioContext = audioContext;
  }

  /**
   * Initialize NTP timing
   * Sets the NTP start time when AudioContext started, then calculates initial drift.
   * Blocks until audio is flowing and initial drift is measured.
   */
  async initialize() {
    if (!this.#audioContext) {
      return;
    }

    // Wait for audio to actually be flowing (contextTime > 0)
    let timestamp;
    while (true) {
      timestamp = this.#audioContext.getOutputTimestamp();
      if (timestamp.contextTime > 0) {
        break;
      }
      await new Promise((resolve) => setTimeout(resolve, 50));
    }

    // Get current time from both domains using a synchronized pair.
    // getOutputTimestamp() returns performanceTime and contextTime from the same
    // audio render instant, ensuring consistency with updateDriftOffset() which
    // also uses getOutputTimestamp(). Using unsynchronized performance.now() +
    // audioContext.currentTime can produce a different time mapping on platforms
    // with WASAPI buffering (Windows/Edge), causing false drift on first measurement.
    timestamp = this.#audioContext.getOutputTimestamp();
    const perfTimeMs = performance.timeOrigin + timestamp.performanceTime;
    const currentNTP = calculateCurrentNTP(perfTimeMs);
    const contextTime = timestamp.contextTime;

    // NTP time at AudioContext start = current NTP - current AudioContext time
    const ntpStartTime = calculateNTPStartTime(currentNTP, contextTime);

    // Write to memory (SAB directly, or via postMessage)
    if (this.#mode === 'sab' && this.#ntpStartView) {
      this.#writeNtpStartTime(ntpStartTime);
    } else if (this.#workletPort) {
      this.#workletPort.postMessage({
        type: 'setNTPStartTime',
        ntpStartTime: ntpStartTime
      });
    }

    // Store for drift calculation
    this.#initialNTPStartTime = ntpStartTime;

    if (__DEV__) {
      console.log(
        `[Dbg-NTPTiming] Initialized: start=${ntpStartTime.toFixed(6)}s ` +
        `(NTP=${currentNTP.toFixed(3)}s, contextTime=${timestamp.contextTime.toFixed(3)}s)`
      );
    }

    // Wait for enough elapsed time to measure drift accurately
    await new Promise((resolve) => setTimeout(resolve, INITIAL_DRIFT_DELAY_MS));

    // Calculate and write initial drift before returning
    this.updateDriftOffset();
  }

  /**
   * Update drift offset (AudioContext → NTP drift correction)
   * CRITICAL: This REPLACES the drift value, does not accumulate
   */
  updateDriftOffset() {
    if (!this.#audioContext || this.#initialNTPStartTime === undefined) {
      return;
    }

    // Get synchronized snapshot of both time domains
    const timestamp = this.#audioContext.getOutputTimestamp();
    const perfTimeMs = performance.timeOrigin + timestamp.performanceTime;
    const currentNTP = calculateCurrentNTP(perfTimeMs);

    // Calculate where contextTime SHOULD be based on wall clock
    const expectedContextTime = currentNTP - this.#initialNTPStartTime;

    // Compare to actual contextTime to get drift
    const driftUs = calculateDriftUs(expectedContextTime, timestamp.contextTime);

    // Store locally
    this.#localDriftMs = Math.round(driftUs / 1000);

    // Write to memory (SAB directly, or via postMessage to worklet which writes to WASM memory)
    if (this.#mode === 'sab' && this.#driftView) {
      Atomics.store(this.#driftView, 0, driftUs);
    } else if (this.#workletPort) {
      this.#workletPort.postMessage({
        type: 'setDriftOffset',
        driftOffsetUs: driftUs
      });
    }

    if (__DEV__) {
      console.log(
        `[Dbg-NTPTiming] Drift: ${(driftUs / 1000).toFixed(1)}ms ` +
        `(expected=${expectedContextTime.toFixed(3)}s, actual=${timestamp.contextTime.toFixed(3)}s)`
      );
    }
  }

  /**
   * Resync NTP timing after recovering from suspend/interrupt.
   * Re-baselines the NTP start time and recalculates drift.
   */
  resync() {
    if (!this.#audioContext) {
      return;
    }

    const timestamp = this.#audioContext.getOutputTimestamp();
    if (!timestamp || timestamp.contextTime <= 0) {
      return;
    }

    // Recalculate NTP start time based on current state
    const perfTimeMs = performance.timeOrigin + timestamp.performanceTime;
    const currentNTP = calculateCurrentNTP(perfTimeMs);
    const ntpStartTime = calculateNTPStartTime(currentNTP, timestamp.contextTime);

    // Update both SAB/worklet and internal state
    if (this.#mode === 'sab' && this.#ntpStartView) {
      this.#writeNtpStartTime(ntpStartTime);
    } else if (this.#workletPort) {
      this.#workletPort.postMessage({
        type: 'setNTPStartTime',
        ntpStartTime: ntpStartTime
      });
    }
    this.#initialNTPStartTime = ntpStartTime;

    // Recalculate drift immediately
    this.updateDriftOffset();

    if (__DEV__) {
      console.log(`[Dbg-NTPTiming] Resynced: start=${ntpStartTime.toFixed(6)}s`);
    }
  }

  /**
   * Start periodic drift offset updates
   */
  startDriftTimer() {
    this.stopDriftTimer();

    this.#driftOffsetTimer = setInterval(() => {
      this.updateDriftOffset();
    }, DRIFT_UPDATE_INTERVAL_MS);

    if (__DEV__) {
      console.log(`[Dbg-NTPTiming] Started drift timer (every ${DRIFT_UPDATE_INTERVAL_MS}ms)`);
    }
  }

  /**
   * Stop periodic drift offset updates
   */
  stopDriftTimer() {
    if (this.#driftOffsetTimer) {
      clearInterval(this.#driftOffsetTimer);
      this.#driftOffsetTimer = null;
    }
  }

  /**
   * Get current drift offset in milliseconds
   * @returns {number}
   */
  getDriftOffset() {
    if (this.#driftView) {
      // SAB stores microseconds; return milliseconds for API consumers
      return Math.round(Atomics.load(this.#driftView, 0) / 1000);
    }
    return this.#localDriftMs;
  }

  /**
   * Get NTP start time
   * @returns {number}
   */
  getNTPStartTime() {
    if (this.#ntpStartView) {
      return this.#ntpStartView[0];
    }
    return this.#initialNTPStartTime ?? 0;
  }

  /**
   * Get clock offset in milliseconds (internal use)
   * @returns {number}
   */
  getClockOffset() {
    if (this.#clockOffsetView) {
      return Atomics.load(this.#clockOffsetView, 0);
    }
    return this.#localClockOffsetMs;
  }

  /**
   * Set clock offset for multi-system sync (e.g., Ableton Link, NTP server).
   * Positive values mean the shared/server clock is ahead of local time —
   * bundles with shared-clock timetags are shifted earlier to compensate.
   * @param {number} offsetS - Offset in seconds
   */
  setClockOffset(offsetS) {
    const offsetMs = Math.round(offsetS * 1000);
    this.#localClockOffsetMs = offsetMs;

    if (this.#mode === 'sab' && this.#clockOffsetView) {
      Atomics.store(this.#clockOffsetView, 0, offsetMs);
    } else if (this.#workletPort) {
      this.#workletPort.postMessage({
        type: 'setClockOffset',
        clockOffsetMs: offsetMs
      });
    }

    if (__DEV__) {
      console.log(`[Dbg-NTPTiming] Clock offset set: ${offsetMs}ms (${offsetS}s)`);
    }
  }

  /**
   * Reset timing state (for shutdown/recover)
   */
  reset() {
    this.stopDriftTimer();
    this.#initialNTPStartTime = undefined;
    this.#localDriftMs = 0;
    this.#localClockOffsetMs = 0;
    this.#ntpStartView = null;
    this.#driftView = null;
    this.#clockOffsetView = null;
  }
}

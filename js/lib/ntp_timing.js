// SPDX-License-Identifier: MIT OR GPL-3.0-or-later
// Copyright (c) 2025 Sam Aaron

import {
  calculateCurrentNTP,
  calculateNTPStartTime,
  calculateDriftMs,
} from './timing_utils.js';
import { DRIFT_UPDATE_INTERVAL_MS, INITIAL_DRIFT_DELAY_MS } from '../timing_constants.js';

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
  #globalOffsetView;

  // Local storage (postMessage mode, or fallback)
  #initialNTPStartTime;
  #localDriftMs = 0;
  #localGlobalOffsetMs = 0;

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
   * Initialize shared views for SAB mode
   * @param {SharedArrayBuffer} sharedBuffer
   * @param {number} ringBufferBase
   * @param {Object} bufferConstants
   */
  initSharedViews(sharedBuffer, ringBufferBase, bufferConstants) {
    this.#ringBufferBase = ringBufferBase;
    this.#bufferConstants = bufferConstants;

    if (this.#mode === 'sab' && sharedBuffer && bufferConstants) {
      this.#ntpStartView = new Float64Array(
        sharedBuffer,
        ringBufferBase + bufferConstants.NTP_START_TIME_START,
        1
      );
      this.#driftView = new Int32Array(
        sharedBuffer,
        ringBufferBase + bufferConstants.DRIFT_OFFSET_START,
        1
      );
      this.#globalOffsetView = new Int32Array(
        sharedBuffer,
        ringBufferBase + bufferConstants.GLOBAL_OFFSET_START,
        1
      );
    }
  }

  /**
   * Set worklet port (for postMessage mode)
   * @param {MessagePort} port
   */
  setWorkletPort(port) {
    this.#workletPort = port;
  }

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
      this.#ntpStartView[0] = ntpStartTime;
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
    const driftMs = calculateDriftMs(expectedContextTime, timestamp.contextTime);

    // Store locally
    this.#localDriftMs = driftMs;

    // Write to memory (SAB directly, or via postMessage to worklet which writes to WASM memory)
    if (this.#mode === 'sab' && this.#driftView) {
      Atomics.store(this.#driftView, 0, driftMs);
    } else if (this.#workletPort) {
      this.#workletPort.postMessage({
        type: 'setDriftOffset',
        driftOffsetMs: driftMs
      });
    }

    if (__DEV__) {
      console.log(
        `[Dbg-NTPTiming] Drift: ${driftMs}ms ` +
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
      this.#ntpStartView[0] = ntpStartTime;
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
      return Atomics.load(this.#driftView, 0);
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
   * Get global offset in milliseconds
   * @returns {number}
   */
  getGlobalOffset() {
    if (this.#globalOffsetView) {
      return Atomics.load(this.#globalOffsetView, 0);
    }
    return this.#localGlobalOffsetMs;
  }

  /**
   * Set global timing offset (for multi-system sync)
   * @param {number} offsetMs - Offset in milliseconds
   */
  setGlobalOffset(offsetMs) {
    this.#localGlobalOffsetMs = offsetMs;

    if (this.#mode === 'sab' && this.#globalOffsetView) {
      Atomics.store(this.#globalOffsetView, 0, offsetMs);
    } else if (this.#workletPort) {
      this.#workletPort.postMessage({
        type: 'setGlobalOffset',
        globalOffsetMs: offsetMs
      });
    }

    if (__DEV__) {
      console.log(`[Dbg-NTPTiming] Global offset set: ${offsetMs}ms`);
    }
  }

  /**
   * Calculate bundle timing for scheduling
   * @param {Uint8Array} uint8Data - OSC bundle data
   * @returns {Object|null} {audioTimeS, currentTimeS} or null for immediate
   */
  calculateBundleWait(uint8Data) {
    if (uint8Data.length < 16) {
      return null;
    }

    const header = String.fromCharCode.apply(null, uint8Data.slice(0, 8));
    if (header !== '#bundle\0') {
      return null;
    }

    const ntpStartTime = this.getNTPStartTime();
    if (ntpStartTime === 0) {
      console.warn('[NTPTiming] NTP start time not yet initialized');
      return null;
    }

    // Read current drift offset
    const driftMs = this.getDriftOffset();
    const driftSeconds = driftMs / 1000.0;

    // Read global offset (for future multi-system sync)
    const globalMs = this.getGlobalOffset();
    const globalSeconds = globalMs / 1000.0;

    const totalOffset = ntpStartTime + driftSeconds + globalSeconds;

    const view = new DataView(uint8Data.buffer, uint8Data.byteOffset);
    const ntpSeconds = view.getUint32(8, false);
    const ntpFraction = view.getUint32(12, false);

    // Immediate bundle (timetag 0 or 1)
    if (ntpSeconds === 0 && (ntpFraction === 0 || ntpFraction === 1)) {
      return null;
    }

    const ntpTimeS = ntpSeconds + ntpFraction / 0x100000000;
    const audioTimeS = ntpTimeS - totalOffset;
    const currentTimeS = this.#audioContext?.currentTime ?? 0;

    return { audioTimeS, currentTimeS };
  }

  /**
   * Reset timing state (for shutdown/recover)
   */
  reset() {
    this.stopDriftTimer();
    this.#initialNTPStartTime = undefined;
    this.#localDriftMs = 0;
    this.#localGlobalOffsetMs = 0;
    this.#ntpStartView = null;
    this.#driftView = null;
    this.#globalOffsetView = null;
  }
}

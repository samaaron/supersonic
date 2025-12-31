// SPDX-License-Identifier: MIT OR GPL-3.0-or-later
// Copyright (c) 2025 Sam Aaron

import {
  calculateCurrentNTP,
  calculateNTPStartTime,
} from './timing_utils.js';
import { DRIFT_UPDATE_INTERVAL_MS } from '../timing_constants.js';

/**
 * Manages NTP timing synchronization between AudioContext and wall clock.
 * Periodically resyncs to auto-correct any drift from background throttling, etc.
 */
export class NTPTiming {
  #mode;
  #audioContext;
  #workletPort;
  #bufferConstants;
  #ringBufferBase;

  // Cached SAB views (SAB mode only)
  #ntpStartView;
  #globalOffsetView;

  // Local storage (postMessage mode, or fallback)
  #ntpStartTime;
  #localGlobalOffsetMs = 0;

  // Resync timer
  #resyncTimer = null;

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
   * Sets the NTP start time when AudioContext started
   * Blocks until audio is actually flowing (contextTime > 0)
   */
  async initialize() {
    if (!this.#bufferConstants || !this.#audioContext) {
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

    // Sync timing
    this.#updateNTPStartTime(timestamp);

    if (__DEV__) {
      console.log(
        `[Dbg-NTPTiming] Initialized: start=${this.#ntpStartTime.toFixed(6)}s`
      );
    }
  }

  /**
   * Calculate and update NTP start time from current timestamp
   * @param {Object} timestamp - From audioContext.getOutputTimestamp()
   */
  #updateNTPStartTime(timestamp) {
    const perfTimeMs = performance.timeOrigin + timestamp.performanceTime;
    const currentNTP = calculateCurrentNTP(perfTimeMs);
    const ntpStartTime = calculateNTPStartTime(currentNTP, timestamp.contextTime);

    // Store locally
    this.#ntpStartTime = ntpStartTime;

    // Update memory (SAB directly, or via postMessage)
    if (this.#mode === 'sab' && this.#ntpStartView) {
      this.#ntpStartView[0] = ntpStartTime;
    } else if (this.#workletPort) {
      this.#workletPort.postMessage({
        type: 'setNTPStartTime',
        ntpStartTime: ntpStartTime
      });
    }
  }

  /**
   * Resync NTP timing - recalculates NTP start time from current clocks.
   * Called periodically to auto-correct any drift from background throttling, etc.
   */
  resync() {
    if (!this.#audioContext) {
      return;
    }

    const timestamp = this.#audioContext.getOutputTimestamp();
    if (!timestamp || timestamp.contextTime <= 0) {
      return;
    }

    this.#updateNTPStartTime(timestamp);

    if (__DEV__) {
      console.log(`[Dbg-NTPTiming] Resynced: start=${this.#ntpStartTime.toFixed(6)}s`);
    }
  }

  /**
   * Start periodic resync timer
   * Resyncs NTP timing every interval to auto-correct any drift
   */
  startDriftTimer() {
    this.stopDriftTimer();

    this.#resyncTimer = setInterval(() => {
      this.resync();
    }, DRIFT_UPDATE_INTERVAL_MS);

    if (__DEV__) {
      console.log(`[Dbg-NTPTiming] Started resync timer (every ${DRIFT_UPDATE_INTERVAL_MS}ms)`);
    }
  }

  /**
   * Stop periodic resync timer
   */
  stopDriftTimer() {
    if (this.#resyncTimer) {
      clearInterval(this.#resyncTimer);
      this.#resyncTimer = null;
    }
  }

  /**
   * Get NTP start time
   * @returns {number}
   */
  getNTPStartTime() {
    if (this.#ntpStartView) {
      return this.#ntpStartView[0];
    }
    return this.#ntpStartTime ?? 0;
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

    // Read global offset (for future multi-system sync)
    const globalMs = this.getGlobalOffset();
    const globalSeconds = globalMs / 1000.0;

    const totalOffset = ntpStartTime + globalSeconds;

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
    this.#ntpStartTime = undefined;
    this.#localGlobalOffsetMs = 0;
    this.#ntpStartView = null;
    this.#globalOffsetView = null;
  }
}

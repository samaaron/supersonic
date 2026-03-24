// SPDX-License-Identifier: MIT OR GPL-3.0-or-later
// Copyright (c) 2025 Sam Aaron

/**
 * Cross-browser audio health monitor.
 *
 * Compares AudioContext.currentTime progression against wall clock
 * (performance.now()) to detect when the audio thread falls behind.
 * Returns a health percentage: 100 = all expected audio delivered,
 * < 100 = audio thread is struggling.
 */
export class AudioHealthMonitor {
  #audioContext;
  #prevContextTime = 0;
  #prevPerfTime = 0;
  #healthPct = 100;

  /** Minimum wall-clock delta (ms) before we compute a new reading.
   *  1000ms eliminates render quantum quantization noise (~0.3% error at 48kHz/128)
   *  while still being responsive enough for health monitoring. */
  static #MIN_DELTA_MS = 1000;

  /**
   * @param {{ audioContext: AudioContext }} options
   */
  constructor({ audioContext }) {
    this.#audioContext = audioContext;
  }

  /**
   * Sample current times and recompute health percentage.
   * Called from #metricsContext() on each metrics read (~10Hz).
   * @returns {number} Health percentage 0-100
   */
  update() {
    if (this.#audioContext.state !== 'running') {
      return this.#healthPct;
    }

    const now = performance.now();
    const contextTime = this.#audioContext.currentTime;

    // First call — seed the baseline, don't compute yet
    if (this.#prevPerfTime === 0) {
      this.#prevPerfTime = now;
      this.#prevContextTime = contextTime;
      return this.#healthPct;
    }

    const wallDeltaMs = now - this.#prevPerfTime;

    // Skip if too little wall time has passed (noisy ratio)
    if (wallDeltaMs < AudioHealthMonitor.#MIN_DELTA_MS) {
      return this.#healthPct;
    }

    const wallDeltaS = wallDeltaMs / 1000;
    const audioDelta = contextTime - this.#prevContextTime;

    this.#healthPct = Math.min(100, Math.round((audioDelta / wallDeltaS) * 100));

    this.#prevPerfTime = now;
    this.#prevContextTime = contextTime;

    return this.#healthPct;
  }

  /**
   * Get current health reading without recomputing.
   * @returns {{ healthPct: number }}
   */
  getHealth() {
    return { healthPct: this.#healthPct };
  }

  /**
   * Clear accumulated state. Call on suspend/resume to avoid
   * stale deltas producing misleading readings.
   */
  reset() {
    this.#prevContextTime = 0;
    this.#prevPerfTime = 0;
    this.#healthPct = 100;
  }
}

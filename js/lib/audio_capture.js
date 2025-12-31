// SPDX-License-Identifier: MIT OR GPL-3.0-or-later
// Copyright (c) 2025 Sam Aaron

/**
 * Audio capture for testing and debugging
 * Captures audio output to SharedArrayBuffer for verification
 */
export class AudioCapture {
  #sharedBuffer;
  #bufferConstants;
  #ringBufferBase;

  /**
   * @param {Object} options
   * @param {SharedArrayBuffer} options.sharedBuffer
   * @param {Object} options.bufferConstants
   * @param {number} options.ringBufferBase
   */
  constructor(options = {}) {
    this.#sharedBuffer = options.sharedBuffer || null;
    this.#bufferConstants = options.bufferConstants || null;
    this.#ringBufferBase = options.ringBufferBase || 0;
  }

  /**
   * Update references (for recovery)
   */
  update(sharedBuffer, ringBufferBase, bufferConstants) {
    this.#sharedBuffer = sharedBuffer;
    this.#ringBufferBase = ringBufferBase;
    this.#bufferConstants = bufferConstants;
  }

  /**
   * Check if capture is available
   * @returns {boolean}
   */
  isAvailable() {
    return !!(this.#sharedBuffer && this.#bufferConstants);
  }

  /**
   * Start capturing audio output to the shared buffer
   * @throws {Error} If not initialized
   * @example
   * capture.start();
   * sonic.send('/s_new', 'sonic-pi-beep', 1000, 0, 0);
   * await sonic.wait(500);
   * const audio = capture.stop();
   */
  start() {
    if (!this.isAvailable()) {
      throw new Error('AudioCapture not initialized');
    }

    const bc = this.#bufferConstants;
    const headerOffset = this.#ringBufferBase + bc.AUDIO_CAPTURE_START;

    // Use Atomics for cross-thread access
    const headerView = new Uint32Array(this.#sharedBuffer, headerOffset, 4);

    // Reset head position and enable capture
    Atomics.store(headerView, 1, 0);  // head = 0
    Atomics.store(headerView, 0, 1);  // enabled = 1
  }

  /**
   * Stop capturing audio and return captured data
   * @returns {Object} Captured audio data with sampleRate, channels, frames, left, right arrays
   * @throws {Error} If not initialized
   */
  stop() {
    if (!this.isAvailable()) {
      throw new Error('AudioCapture not initialized');
    }

    const bc = this.#bufferConstants;
    const headerOffset = this.#ringBufferBase + bc.AUDIO_CAPTURE_START;

    // Read header
    const headerView = new Uint32Array(this.#sharedBuffer, headerOffset, 4);

    // Disable capture first
    Atomics.store(headerView, 0, 0);  // enabled = 0

    // Read captured data
    const head = Atomics.load(headerView, 1);  // frames captured
    const sampleRate = headerView[2];
    const channels = headerView[3];

    // Read audio data (interleaved: L0, R0, L1, R1, ...)
    const dataOffset = headerOffset + bc.AUDIO_CAPTURE_HEADER_SIZE;
    const dataView = new Float32Array(this.#sharedBuffer, dataOffset, head * channels);

    // Deinterleave into separate channel arrays (copy to non-shared buffer)
    const left = new Float32Array(head);
    const right = channels > 1 ? new Float32Array(head) : null;

    for (let i = 0; i < head; i++) {
      left[i] = dataView[i * channels];
      if (right) {
        right[i] = dataView[i * channels + 1];
      }
    }

    return {
      sampleRate,
      channels,
      frames: head,
      left,
      right
    };
  }

  /**
   * Check if audio capture is currently enabled
   * @returns {boolean} True if capture is enabled
   */
  isEnabled() {
    if (!this.isAvailable()) {
      return false;
    }

    const bc = this.#bufferConstants;
    const headerOffset = this.#ringBufferBase + bc.AUDIO_CAPTURE_START;
    const headerView = new Uint32Array(this.#sharedBuffer, headerOffset, 1);
    return Atomics.load(headerView, 0) === 1;
  }

  /**
   * Get current capture position in frames
   * @returns {number} Number of frames captured so far
   */
  getFrameCount() {
    if (!this.isAvailable()) {
      return 0;
    }

    const bc = this.#bufferConstants;
    const headerOffset = this.#ringBufferBase + bc.AUDIO_CAPTURE_START;
    const headerView = new Uint32Array(this.#sharedBuffer, headerOffset, 2);
    return Atomics.load(headerView, 1);
  }

  /**
   * Get maximum capture duration in seconds
   * @returns {number} Maximum capture duration based on buffer size and sample rate
   */
  getMaxDuration() {
    if (!this.#bufferConstants) return 0;
    const bc = this.#bufferConstants;
    return bc.AUDIO_CAPTURE_FRAMES / (bc.AUDIO_CAPTURE_SAMPLE_RATE || 48000);
  }
}

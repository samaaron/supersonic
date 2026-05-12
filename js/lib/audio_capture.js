// SPDX-License-Identifier: MIT OR GPL-3.0-or-later
// Copyright (c) 2025 Sam Aaron

/**
 * Audio capture reader for slot 0 (master output) of the
 * shm_audio_buffer multi-slot ring at SHM_AUDIO_START.
 *
 * Slot 0 is written by the audio thread's post-block hook in
 * audio_processor.cpp whenever `enabled` is set; start()/stop() flip
 * the flag and reset/read the slot. Producer is the audio thread, not
 * a synth, so graph ordering cannot displace it.
 *
 * Slots 1..N-1 are driven by AudioOut2 UGens, independent of this class.
 *
 * Layout (matches src/scsynth/common/shm_audio_buffer.hpp):
 *
 *     offset 0   atomic<u32> enabled
 *     offset 4   u32         sample_rate
 *     offset 8   u32         channels
 *     offset 12  u32         capacity_frames
 *     offset 16  atomic<u64> write_position   (monotonic frame counter)
 *     offset 24  u32[2]      _padding         (16-aligned data)
 *     offset 32  float[]     data             (interleaved ring)
 *
 * Ring-with-overwrite. write_position is monotonic; physical slot index
 * is write_position % capacity_frames. Tests stay below capacity so
 * linear indexing into `data` walks captured frames in order.
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

  /** Update references (for recovery) */
  update(sharedBuffer, ringBufferBase, bufferConstants) {
    this.#sharedBuffer = sharedBuffer;
    this.#ringBufferBase = ringBufferBase;
    this.#bufferConstants = bufferConstants;
  }

  /** @returns {boolean} */
  isAvailable() {
    return !!(this.#sharedBuffer && this.#bufferConstants);
  }

  // Header layout offsets (in Uint32 indices from the slot start).
  static #IDX_ENABLED       = 0;  // u32
  static #IDX_SAMPLE_RATE   = 1;  // u32
  static #IDX_CHANNELS      = 2;  // u32
  // index 3: u32 capacity_frames (unused by JS reader)
  static #IDX_WPOS_LOW      = 4;  // u64 low (write_position)
  static #IDX_WPOS_HIGH     = 5;  // u64 high
  static #HEADER_U32_COUNT  = 8;  // 32 bytes / 4

  /**
   * Begin capturing master output. Resets write_position and sets the
   * `enabled` flag; the audio thread's post-block hook then writes the
   * master bus into slot 0 every block. The first sample arrives up to
   * one audio-worklet quantum later (~2.67ms at 48kHz, block size 128).
   *
   * @throws {Error} If not initialized
   */
  start() {
    if (!this.isAvailable()) {
      throw new Error('AudioCapture not initialized');
    }
    const bc = this.#bufferConstants;
    const headerOffset = this.#ringBufferBase + bc.SHM_AUDIO_START;
    const h = new Uint32Array(this.#sharedBuffer, headerOffset,
                              AudioCapture.#HEADER_U32_COUNT);
    // Disable first so any write in flight finishes before the position
    // reset below. Any block that completes after this store has its
    // position update clobbered by the next two stores, which is fine
    // because enabled=0 stops further writes until the final store
    // below re-enables.
    Atomics.store(h, AudioCapture.#IDX_ENABLED, 0);
    Atomics.store(h, AudioCapture.#IDX_WPOS_LOW, 0);
    Atomics.store(h, AudioCapture.#IDX_WPOS_HIGH, 0);
    Atomics.store(h, AudioCapture.#IDX_ENABLED, 1);
  }

  /**
   * Stop capturing and return the captured audio. Clears the `enabled`
   * flag and reads the slot. Up to one audio-worklet quantum of frames
   * may be appended after this call returns, before the audio thread
   * observes the flag clear.
   *
   * @returns {Object} { sampleRate, channels, frames, left, right }
   * @throws {Error} If not initialized
   */
  stop() {
    if (!this.isAvailable()) {
      throw new Error('AudioCapture not initialized');
    }
    const bc = this.#bufferConstants;
    const headerOffset = this.#ringBufferBase + bc.SHM_AUDIO_START;
    const h = new Uint32Array(this.#sharedBuffer, headerOffset, 1);
    Atomics.store(h, AudioCapture.#IDX_ENABLED, 0);
    return this.read();
  }

  /**
   * Snapshot the captured audio. Normally called after stop(). May be
   * called while capture is active; returns whatever the writer has
   * published so far.
   *
   * @returns {Object} { sampleRate, channels, frames, left, right }
   * @throws {Error} If not initialized
   */
  read() {
    if (!this.isAvailable()) {
      throw new Error('AudioCapture not initialized');
    }
    const bc = this.#bufferConstants;
    const headerOffset = this.#ringBufferBase + bc.SHM_AUDIO_START;
    const h = new Uint32Array(this.#sharedBuffer, headerOffset,
                              AudioCapture.#HEADER_U32_COUNT);

    const sampleRate = h[AudioCapture.#IDX_SAMPLE_RATE];
    const channels   = h[AudioCapture.#IDX_CHANNELS];
    // Monotonic uint64 frame counter. Tests stay below 2^32; the high
    // half is checked defensively.
    const wposLow  = Atomics.load(h, AudioCapture.#IDX_WPOS_LOW);
    const wposHigh = Atomics.load(h, AudioCapture.#IDX_WPOS_HIGH);
    if (wposHigh !== 0) {
      // 2^32 frames is ~24h at 48kHz; deinterleaving that into typed
      // arrays would exhaust memory.
      throw new Error(`AudioCapture: write_position too large (high=${wposHigh})`);
    }
    const frames = wposLow;

    // shm_audio_buffer is alignas(16) and its 32-byte header is fully
    // populated, so data starts at headerOffset + 32 with no padding.
    const dataOffset = headerOffset + bc.SHM_AUDIO_HEADER_SIZE;
    const dataView = new Float32Array(this.#sharedBuffer, dataOffset,
                                      frames * channels);

    // Deinterleave into non-SAB typed arrays.
    const left  = new Float32Array(frames);
    const right = channels > 1 ? new Float32Array(frames) : null;
    for (let i = 0; i < frames; i++) {
      left[i] = dataView[i * channels];
      if (right) right[i] = dataView[i * channels + 1];
    }
    return { sampleRate, channels, frames, left, right };
  }

  /** @returns {boolean} */
  isEnabled() {
    if (!this.isAvailable()) return false;
    const bc = this.#bufferConstants;
    const headerOffset = this.#ringBufferBase + bc.SHM_AUDIO_START;
    const h = new Uint32Array(this.#sharedBuffer, headerOffset, 1);
    return Atomics.load(h, AudioCapture.#IDX_ENABLED) === 1;
  }

  /** @returns {number} Frames captured so far (low 32 bits) */
  getFrameCount() {
    if (!this.isAvailable()) return 0;
    const bc = this.#bufferConstants;
    const headerOffset = this.#ringBufferBase + bc.SHM_AUDIO_START;
    const h = new Uint32Array(this.#sharedBuffer, headerOffset,
                              AudioCapture.#HEADER_U32_COUNT);
    return Atomics.load(h, AudioCapture.#IDX_WPOS_LOW);
  }

  /** @returns {number} Maximum capture duration in seconds */
  getMaxDuration() {
    if (!this.#bufferConstants) return 0;
    const bc = this.#bufferConstants;
    return bc.SHM_AUDIO_FRAMES / (bc.SHM_AUDIO_SAMPLE_RATE || 48000);
  }
}

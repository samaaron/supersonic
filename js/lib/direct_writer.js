/*
    SuperSonic - SuperCollider AudioWorklet WebAssembly port
    Copyright (c) 2025 Sam Aaron

    Based on SuperCollider by James McCartney and community
    GPL v3 or later
*/

import { writeToRingBuffer } from "./ring_buffer_writer.js";

// Lookahead threshold - bundles within this time bypass prescheduler
const BYPASS_LOOKAHEAD_S = 0.2; // 200ms, matches prescheduler LOOKAHEAD_S

/**
 * DirectWriter - Low-latency ring buffer writer that bypasses the prescheduler
 *
 * Handles direct writes to the OSC IN ring buffer for messages that don't need
 * scheduling (non-bundles, immediate bundles, or bundles within 200ms).
 * Falls back to worker-based writes for future-scheduled bundles.
 */
export class DirectWriter {
  #sharedBuffer;
  #ringBufferBase;
  #bufferConstants;
  #getAudioContextTime;
  #getNTPStartTime;

  // Cached views for efficient writes
  #atomicView;
  #dataView;
  #uint8View;
  #controlIndices;

  /**
   * @param {Object} options
   * @param {SharedArrayBuffer} options.sharedBuffer - Shared memory buffer
   * @param {number} options.ringBufferBase - Base offset of ring buffers
   * @param {Object} options.bufferConstants - Buffer layout constants
   * @param {Function} options.getAudioContextTime - Returns audioContext.currentTime
   * @param {Function} options.getNTPStartTime - Returns NTP start time from SAB
   */
  constructor({
    sharedBuffer,
    ringBufferBase,
    bufferConstants,
    getAudioContextTime,
    getNTPStartTime,
  }) {
    if (!sharedBuffer || !bufferConstants) {
      throw new Error("DirectWriter requires sharedBuffer and bufferConstants");
    }
    if (typeof getAudioContextTime !== "function") {
      throw new Error("DirectWriter requires getAudioContextTime callback");
    }
    if (typeof getNTPStartTime !== "function") {
      throw new Error("DirectWriter requires getNTPStartTime callback");
    }

    this.#sharedBuffer = sharedBuffer;
    this.#ringBufferBase = ringBufferBase;
    this.#bufferConstants = bufferConstants;
    this.#getAudioContextTime = getAudioContextTime;
    this.#getNTPStartTime = getNTPStartTime;

    this.#initializeViews();
  }

  /**
   * Attempt to write OSC message directly to ring buffer
   * @param {Uint8Array} oscData - OSC message bytes
   * @returns {boolean} True if written directly, false if caller should use worker
   */
  tryWrite(oscData) {
    if (!this.#atomicView || !this.#controlIndices) {
      return false;
    }

    // Check if this message should bypass the prescheduler
    if (!this.#shouldBypass(oscData)) {
      return false;
    }

    // Attempt direct ring buffer write
    return writeToRingBuffer({
      atomicView: this.#atomicView,
      dataView: this.#dataView,
      uint8View: this.#uint8View,
      bufferConstants: this.#bufferConstants,
      ringBufferBase: this.#ringBufferBase,
      controlIndices: this.#controlIndices,
      oscMessage: oscData,
    });
  }

  /**
   * Check if OSC data is a bundle (starts with #bundle)
   * @param {Uint8Array} oscData
   * @returns {boolean}
   */
  isBundle(oscData) {
    return oscData.length >= 8 && oscData[0] === 0x23; // '#' character
  }

  // ============================================================================
  // PRIVATE METHODS
  // ============================================================================

  #initializeViews() {
    this.#atomicView = new Int32Array(this.#sharedBuffer);
    this.#dataView = new DataView(this.#sharedBuffer);
    this.#uint8View = new Uint8Array(this.#sharedBuffer);

    // Control indices (must match shared_memory.h ControlPointers layout)
    const CONTROL_START = this.#bufferConstants.CONTROL_START;
    this.#controlIndices = {
      IN_HEAD: (this.#ringBufferBase + CONTROL_START + 0) / 4,
      IN_TAIL: (this.#ringBufferBase + CONTROL_START + 4) / 4,
      IN_SEQUENCE: (this.#ringBufferBase + CONTROL_START + 24) / 4,
      IN_WRITE_LOCK: (this.#ringBufferBase + CONTROL_START + 40) / 4,
    };
  }

  /**
   * Check if an OSC message/bundle should bypass the prescheduler
   * Returns true for: non-bundles, immediate bundles (timetag 0/1), past timetags, or within 200ms
   * @param {Uint8Array} oscData
   * @returns {boolean}
   */
  #shouldBypass(oscData) {
    // Non-bundles always bypass
    if (!this.isBundle(oscData)) {
      return true;
    }

    // Bundle format: "#bundle\0" (8 bytes) + timetag (8 bytes)
    if (oscData.length < 16) {
      return true; // Malformed bundle, let it through
    }

    // Read NTP timetag (big-endian)
    const view = new DataView(
      oscData.buffer,
      oscData.byteOffset,
      oscData.byteLength
    );
    const ntpSeconds = view.getUint32(8, false);
    const ntpFraction = view.getUint32(12, false);

    // Timetag 0 or 1 means "execute immediately"
    if (ntpSeconds === 0 && (ntpFraction === 0 || ntpFraction === 1)) {
      return true;
    }

    // Get timing context
    const currentTime = this.#getAudioContextTime();
    const ntpStartTime = this.#getNTPStartTime();

    if (currentTime === null || ntpStartTime === 0) {
      return true; // Can't compare, let it through
    }

    // Calculate current NTP and compare to bundle NTP
    const currentNTP = currentTime + ntpStartTime;
    const bundleNTP = ntpSeconds + ntpFraction / 0x100000000;

    // Bypass if timetag is in the past or within lookahead window
    const diffSeconds = bundleNTP - currentNTP;
    return diffSeconds < BYPASS_LOOKAHEAD_S;
  }
}

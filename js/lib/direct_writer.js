// SPDX-License-Identifier: MIT OR GPL-3.0-or-later
// Copyright (c) 2025 Sam Aaron

import { writeToRingBuffer } from "./ring_buffer_writer.js";
import {
  classifyOscMessage,
  shouldBypass,
  isBundle,
  DEFAULT_BYPASS_LOOKAHEAD_S,
} from "./osc_classifier.js";

/**
 * DirectWriter - Low-latency ring buffer writer that bypasses the prescheduler
 *
 * Handles direct writes to the OSC IN ring buffer for messages that don't need
 * scheduling (non-bundles, immediate bundles, or bundles within the bypass threshold).
 * Falls back to worker-based writes for future-scheduled bundles.
 */
export class DirectWriter {
  #sharedBuffer;
  #ringBufferBase;
  #bufferConstants;
  #getAudioContextTime;
  #getNTPStartTime;
  #bypassLookaheadS;

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
   * @param {number} [options.bypassLookaheadS=0.2] - Threshold for bypass routing (seconds)
   */
  constructor({
    sharedBuffer,
    ringBufferBase,
    bufferConstants,
    getAudioContextTime,
    getNTPStartTime,
    bypassLookaheadS,
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
    this.#bypassLookaheadS = bypassLookaheadS ?? DEFAULT_BYPASS_LOOKAHEAD_S;

    this.#initializeViews();
  }

  /**
   * Attempt to write OSC message directly to ring buffer
   * @param {Uint8Array} oscData - OSC message bytes
   * @returns {string|false} Category string ('nonBundle', 'immediate', 'nearFuture', 'late') on success, false if caller should use worker
   */
  tryWrite(oscData) {
    if (!this.#atomicView || !this.#controlIndices) {
      return false;
    }

    // Classify using shared logic with audio-context-based NTP
    const category = classifyOscMessage(oscData, {
      getCurrentNTP: () => {
        const currentTime = this.#getAudioContextTime();
        const ntpStartTime = this.#getNTPStartTime();
        if (currentTime === null || ntpStartTime === 0) return null;
        return currentTime + ntpStartTime;
      },
      bypassLookaheadS: this.#bypassLookaheadS,
    });

    // Only bypass categories should be written directly
    if (!shouldBypass(category)) {
      return false;
    }

    // Attempt direct ring buffer write
    const written = writeToRingBuffer({
      atomicView: this.#atomicView,
      dataView: this.#dataView,
      uint8View: this.#uint8View,
      bufferConstants: this.#bufferConstants,
      ringBufferBase: this.#ringBufferBase,
      controlIndices: this.#controlIndices,
      oscMessage: oscData,
    });

    return written ? category : false;
  }

  /**
   * Check if OSC data is a bundle (starts with #bundle)
   * @param {Uint8Array} oscData
   * @returns {boolean}
   */
  isBundle(oscData) {
    return isBundle(oscData);
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
}

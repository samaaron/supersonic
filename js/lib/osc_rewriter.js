// SPDX-License-Identifier: MIT OR GPL-3.0-or-later
// Copyright (c) 2025 Sam Aaron

/**
 * OSC Rewriter - Transforms buffer allocation OSC messages
 *
 * Rewrites /b_alloc, /b_allocRead, /b_allocReadChannel, /b_allocFile
 * into /b_allocPtr messages that work with SuperSonic's buffer pool.
 */
export class OSCRewriter {
  #bufferManager;
  #getDefaultSampleRate;

  /**
   * @param {Object} options
   * @param {Object} options.bufferManager - BufferManager instance for buffer operations
   * @param {Function} options.getDefaultSampleRate - Callback returning default sample rate
   */
  constructor({ bufferManager, getDefaultSampleRate }) {
    if (!bufferManager) {
      throw new Error("OSCRewriter requires bufferManager");
    }
    if (typeof getDefaultSampleRate !== "function") {
      throw new Error("OSCRewriter requires getDefaultSampleRate callback");
    }
    this.#bufferManager = bufferManager;
    this.#getDefaultSampleRate = getDefaultSampleRate;
  }

  /**
   * Rewrite an OSC packet (message or bundle), transforming buffer commands
   * @param {Object} packet - Decoded OSC packet
   * @returns {Promise<{packet: Object, changed: boolean}>}
   */
  async rewritePacket(packet) {
    if (Array.isArray(packet)) {
      const { message, changed } = await this.#rewriteMessage(packet);
      return { packet: message, changed };
    }

    if (this.#isBundle(packet)) {
      const subResults = await Promise.all(
        packet.packets.map((subPacket) => this.rewritePacket(subPacket))
      );

      const changed = subResults.some((result) => result.changed);

      if (!changed) {
        return { packet, changed: false };
      }

      const rewrittenPackets = subResults.map((result) => result.packet);

      return {
        packet: {
          timeTag: packet.timeTag,
          packets: rewrittenPackets,
        },
        changed: true,
      };
    }

    return { packet, changed: false };
  }

  // ============================================================================
  // PRIVATE METHODS
  // ============================================================================

  async #rewriteMessage(message) {
    const address = message[0];
    const args = message.slice(1);
    switch (address) {
      case "/b_alloc":
        return {
          message: await this.#rewriteAlloc(args),
          changed: true,
        };
      case "/b_allocRead":
        return {
          message: await this.#rewriteAllocRead(args),
          changed: true,
        };
      case "/b_allocReadChannel":
        return {
          message: await this.#rewriteAllocReadChannel(args),
          changed: true,
        };
      case "/b_allocFile":
        return {
          message: await this.#rewriteAllocFile(args),
          changed: true,
        };
      default:
        return { message, changed: false };
    }
  }

  async #rewriteAlloc(args) {
    const bufnum = this.#requireIntArg(
      args,
      0,
      "/b_alloc requires a buffer number"
    );
    const numFrames = this.#requireIntArg(
      args,
      1,
      "/b_alloc requires a frame count"
    );

    let argIndex = 2;
    let numChannels = 1;
    let sampleRate = this.#getDefaultSampleRate();

    if (this.#isNumericArg(this.#argAt(args, argIndex))) {
      numChannels = Math.max(
        1,
        this.#optionalIntArg(args, argIndex, 1)
      );
      argIndex++;
    }

    if (this.#argAt(args, argIndex)?.type === "b") {
      if (__DEV__) console.warn('[OSCRewriter] /b_alloc completion message detected but not supported — it will be dropped. Buffer allocation is async via the buffer pipeline.');
      argIndex++;
    }

    if (this.#isNumericArg(this.#argAt(args, argIndex))) {
      sampleRate = this.#getArgValue(this.#argAt(args, argIndex));
    }

    const bufferInfo = await this.#bufferManager.prepareEmpty({
      bufnum,
      numFrames,
      numChannels,
      sampleRate,
    });

    this.#detachAllocationPromise(
      bufferInfo.allocationComplete,
      `/b_alloc ${bufnum}`
    );
    return this.#buildAllocPtrMessage(bufnum, bufferInfo);
  }

  async #rewriteAllocRead(args) {
    const bufnum = this.#requireIntArg(
      args,
      0,
      "/b_allocRead requires a buffer number"
    );
    const path = this.#requireStringArg(
      args,
      1,
      "/b_allocRead requires a file path"
    );
    const startFrame = this.#optionalIntArg(args, 2, 0);
    const numFrames = this.#optionalIntArg(args, 3, 0);

    const bufferInfo = await this.#bufferManager.prepareFromFile({
      bufnum,
      path,
      startFrame,
      numFrames,
    });

    this.#detachAllocationPromise(
      bufferInfo.allocationComplete,
      `/b_allocRead ${bufnum}`
    );
    return this.#buildAllocPtrMessage(bufnum, bufferInfo);
  }

  async #rewriteAllocReadChannel(args) {
    const bufnum = this.#requireIntArg(
      args,
      0,
      "/b_allocReadChannel requires a buffer number"
    );
    const path = this.#requireStringArg(
      args,
      1,
      "/b_allocReadChannel requires a file path"
    );
    const startFrame = this.#optionalIntArg(args, 2, 0);
    const numFrames = this.#optionalIntArg(args, 3, 0);

    const channels = [];
    for (let i = 4; i < (args?.length || 0); i++) {
      if (!this.#isNumericArg(args[i])) {
        break;
      }
      channels.push(Math.floor(this.#getArgValue(args[i])));
    }

    const bufferInfo = await this.#bufferManager.prepareFromFile({
      bufnum,
      path,
      startFrame,
      numFrames,
      channels: channels.length > 0 ? channels : null,
    });

    this.#detachAllocationPromise(
      bufferInfo.allocationComplete,
      `/b_allocReadChannel ${bufnum}`
    );
    return this.#buildAllocPtrMessage(bufnum, bufferInfo);
  }

  /**
   * Handle /b_allocFile - SuperSonic extension (not standard scsynth OSC)
   * Loads audio from inline file data (FLAC, WAV, OGG, etc.) without URL fetch.
   */
  async #rewriteAllocFile(args) {
    const bufnum = this.#requireIntArg(
      args,
      0,
      "/b_allocFile requires a buffer number"
    );
    const blob = this.#requireBlobArg(
      args,
      1,
      "/b_allocFile requires audio file data as blob"
    );

    const bufferInfo = await this.#bufferManager.prepareFromBlob({
      bufnum,
      blob,
    });

    this.#detachAllocationPromise(
      bufferInfo.allocationComplete,
      `/b_allocFile ${bufnum}`
    );
    return this.#buildAllocPtrMessage(bufnum, bufferInfo);
  }

  #buildAllocPtrMessage(bufnum, bufferInfo) {
    return [
      "/b_allocPtr",
      Math.floor(bufnum),
      Math.floor(bufferInfo.ptr),
      Math.floor(bufferInfo.numFrames),
      Math.floor(bufferInfo.numChannels),
      bufferInfo.sampleRate,
      String(bufferInfo.uuid),
    ];
  }

  #isBundle(packet) {
    return (
      packet && packet.timeTag !== undefined && Array.isArray(packet.packets)
    );
  }

  // ============================================================================
  // ARGUMENT HELPERS (plain values - osc_fast infers types)
  // ============================================================================

  #argAt(args, index) {
    if (!Array.isArray(args)) {
      return undefined;
    }
    return args[index];
  }

  #getArgValue(arg) {
    if (arg === undefined || arg === null) {
      return undefined;
    }
    // Support both plain values and legacy {type, value} format for compatibility
    return typeof arg === "object" &&
      Object.prototype.hasOwnProperty.call(arg, "value")
      ? arg.value
      : arg;
  }

  #requireIntArg(args, index, errorMessage) {
    const value = this.#getArgValue(this.#argAt(args, index));
    if (!Number.isFinite(value)) {
      throw new Error(errorMessage);
    }
    return Math.floor(value);
  }

  #optionalIntArg(args, index, defaultValue = 0) {
    const value = this.#getArgValue(this.#argAt(args, index));
    if (!Number.isFinite(value)) {
      return defaultValue;
    }
    return Math.floor(value);
  }

  #requireStringArg(args, index, errorMessage) {
    const value = this.#getArgValue(this.#argAt(args, index));
    if (typeof value !== "string") {
      throw new Error(errorMessage);
    }
    return value;
  }

  #requireBlobArg(args, index, errorMessage) {
    const value = this.#getArgValue(this.#argAt(args, index));
    if (!(value instanceof Uint8Array || value instanceof ArrayBuffer)) {
      throw new Error(errorMessage);
    }
    return value;
  }

  #isNumericArg(arg) {
    if (!arg) {
      return false;
    }
    const value = this.#getArgValue(arg);
    return Number.isFinite(value);
  }

  #detachAllocationPromise(promise, context) {
    if (!promise || typeof promise.catch !== "function") {
      return;
    }

    promise.catch((error) => {
      console.error(`[OSCRewriter] ${context} allocation failed:`, error);
    });
  }
}

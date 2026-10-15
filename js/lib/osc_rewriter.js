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
      case "/b_read":
        return { message: await this.#rewriteRead(args, false), changed: true };
      case "/b_readChannel":
        return { message: await this.#rewriteRead(args, true), changed: true };
      case "/b_write":
        return { message: await this.#rewriteWrite(args), changed: true };
      default:
        return { message, changed: false };
    }
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
    return this.#buildCommitMessage(bufnum, bufferInfo);
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
      const v = this.#val(args, i);
      if (!Number.isFinite(v)) break;
      channels.push(Math.floor(v));
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
    return this.#buildCommitMessage(bufnum, bufferInfo);
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
    return this.#buildCommitMessage(bufnum, bufferInfo);
  }

  // /b_read bufnum path [startFrame numFrames bufStart leaveOpen], and
  // /b_readChannel with the channels after leaveOpen: the frames are decoded
  // here and staged, and the engine copies them into the buffer it already
  // has (/supersonic/buffer/read), as the native front does it.
  async #rewriteRead(args, withChannels) {
    const verb = withChannels ? "/b_readChannel" : "/b_read";
    const bufnum = this.#requireIntArg(args, 0, `${verb} requires a buffer number`);
    const path = this.#requireStringArg(args, 1, `${verb} requires a file path`);
    const startFrame = this.#optionalIntArg(args, 2, 0);
    const numFrames = this.#optionalIntArg(args, 3, -1);
    const bufStart = this.#optionalIntArg(args, 4, 0);
    const leaveOpen = this.#optionalIntArg(args, 5, 0);
    if (leaveOpen) throw new Error(`${verb} leaveOpen is not available: this engine does not stream from disk`);
    const channels = [];
    if (withChannels) {
      for (let i = 6; i < (args?.length || 0); i++) {
        const v = this.#val(args, i);
        if (!Number.isFinite(v)) break;
        channels.push(Math.floor(v));
      }
    }
    const info = await this.#bufferManager.prepareReadInto({
      bufnum, path, bufStart, startFrame, numFrames: numFrames > 0 ? numFrames : 0,
      channels: channels.length ? channels : null,
    });
    return [
      "/supersonic/buffer/read",
      Math.floor(bufnum), Math.floor(info.bufStart),
      Math.floor(info.laneOffset),   // an offset into the assets area, not an address
      Math.floor(info.numFrames), Math.floor(info.numChannels), info.sampleRate,
    ];
  }

  // /b_write bufnum path headerFormat sampleFormat [numFrames startFrame leaveOpen]:
  // the engine puts the frames in the returns area (/supersonic/buffer/publish) and
  // the buffer manager hands the product a WAV of them.
  async #rewriteWrite(args) {
    const bufnum = this.#requireIntArg(args, 0, "/b_write requires a buffer number");
    const path = this.#requireStringArg(args, 1, "/b_write requires a file path");
    const numFrames = this.#optionalIntArg(args, 4, -1);
    const startFrame = this.#optionalIntArg(args, 5, 0);
    const info = await this.#bufferManager.prepareWrite({ bufnum, path, numFrames, startFrame });
    return ["/supersonic/buffer/publish", Math.floor(bufnum), Math.floor(info.startFrame), Math.floor(info.numFrames), 0];
  }

  // The one binding on every platform (clockwork dsp_api.h, "Assets"): the
  // frames are in the assets area, and the engine binds the buffer to them where
  // they lie. The payload offset is past the guard frames this side laid.
  #buildCommitMessage(bufnum, bufferInfo) {
    return [
      "/clockwork/asset/commit",
      Math.floor(bufnum),
      1,   // CLOCKWORK_ASSET_AUDIO_F32
      Math.floor(bufferInfo.payloadOffset),   // an offset into the assets area, not an address
      Math.floor(bufferInfo.byteCount),
      Math.floor(bufferInfo.numChannels),
      Math.floor(bufferInfo.numFrames),
      bufferInfo.sampleRate,
    ];
  }

  #isBundle(packet) {
    return (
      packet && packet.timeTag !== undefined && Array.isArray(packet.packets)
    );
  }

  // ============================================================================
  // ARGUMENT HELPERS
  // ============================================================================

  /** Get the raw value of an arg: a plain value, or the value of a tagged {type, value} one */
  #val(args, index) {
    const arg = Array.isArray(args) ? args[index] : undefined;
    if (arg === undefined || arg === null) return undefined;
    return typeof arg === "object" && Object.prototype.hasOwnProperty.call(arg, "value")
      ? arg.value : arg;
  }

  /** Require an arg of a given type, throwing errorMessage if missing/wrong */
  #require(args, index, check, errorMessage) {
    const v = this.#val(args, index);
    if (!check(v)) throw new Error(errorMessage);
    return v;
  }

  #requireIntArg(args, index, errorMessage) {
    return Math.floor(this.#require(args, index, Number.isFinite, errorMessage));
  }

  #optionalIntArg(args, index, defaultValue = 0) {
    const v = this.#val(args, index);
    return Number.isFinite(v) ? Math.floor(v) : defaultValue;
  }

  #requireStringArg(args, index, errorMessage) {
    return this.#require(args, index, v => typeof v === "string", errorMessage);
  }

  #requireBlobArg(args, index, errorMessage) {
    return this.#require(args, index, v => v instanceof Uint8Array || v instanceof ArrayBuffer, errorMessage);
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

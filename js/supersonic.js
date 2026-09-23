// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2025-2026 Sam Aaron
/*
 * supersonic.js — SuperSonic's client: clockwork's, plus what scsynth means.
 *
 * Clockwork client is deliberately guest-agnostic. It sends OSC, holds the
 * clock, manages devices, and knows nothing about synthdefs — a definition
 * reaches the DSP through a verb the DSP declared, and clockwork never learns
 * what the bytes are.
 *
 * That is right for clockwork and not enough for a product. Sonic Pi asks
 * SuperSonic to load "sonic-pi-beep", and something has to know that means
 * fetching sonic-pi-beep.scsyndef and sending it to /d_recv. That knowledge
 * lives here, on this side of the seam, in the only class that is allowed to
 * know what an scsyndef is.
 */
import { Clockwork } from "../clockwork/js/clockwork.js";
import { BufferManager } from "./lib/buffer_manager.js";
import { OSCRewriter } from "./lib/osc_rewriter.js";
import * as oscFast from "../clockwork/js/lib/osc_fast.js";
import { scsynthProfile } from "./scsynth_profile.js";
import { NODE_TREE_HEADER_SIZE, NODE_TREE_ENTRY_SIZE } from "./lib/node_tree_parser.js";
import { parseNodeTree } from "./lib/node_tree_parser.js";
import { MemoryLayout } from "../clockwork/js/memory_layout.js";
import { defaultScsynthOptions, validateScsynthOptions, encodeScsynthOptions } from "./scsynth_options.js";

/*
 * HOW SUPERSONIC SPLITS ITS MEMORY.
 *
 * Clockwork hands over one opaque region and never asks what is in it.
 * scsynth needs two things in there, so the split is decided HERE:
 *
 *   inbox().offset ───────── buffer pool  sample frames, grows
 *
 * IT USED TO BE TWO. The front of the region was an RT arena for scsynth's
 * AllocPool, carved here and handed to the engine as a byte offset in a
 * config-block slot, which the engine read back through a pair of file-scope
 * externs — the only allocation on any target that did not come from
 * clockwork::mem. The engine takes its pool from clockwork::mem now (memArenaSize in
 * memory_layout.js is the span clockwork gives it), so the client no longer
 * reserves anything here and the whole region is buffers.
 */
/* Room in the arena for everything that is not the engine's pool:
 * clockwork's own heap (CLOCKWORK_HEAP_SIZE, 8MB on this target) and the AllocPool's
 * area headers. */
const RT_ARENA_HEADROOM = 16 * 1024 * 1024;
const BUFFERS_INIT   =  4 * 1024 * 1024;
const BUFFERS_MAX    = 768 * 1024 * 1024;

/** Refuse a malformed buffer command at the call site rather than in the queue. */
function validateBufferCommand(address, args) {
  const int = (i, msg) => { if (!Number.isFinite(args[i])) throw new Error(msg); };
  const str = (i, msg) => { if (typeof args[i] !== "string") throw new Error(msg); };
  const blob = (i, msg) => {
    const v = args[i];
    if (!(v instanceof Uint8Array || v instanceof ArrayBuffer)) throw new Error(msg);
  };
  switch (address) {
    case "/b_alloc":
      int(0, "/b_alloc requires a buffer number");
      int(1, "/b_alloc requires a frame count"); break;
    case "/b_allocRead":
      int(0, "/b_allocRead requires a buffer number");
      str(1, "/b_allocRead requires a file path"); break;
    case "/b_allocReadChannel":
      int(0, "/b_allocReadChannel requires a buffer number");
      str(1, "/b_allocReadChannel requires a file path"); break;
    case "/b_allocFile":
      int(0, "/b_allocFile requires a buffer number");
      blob(1, "/b_allocFile requires audio file data as blob"); break;
  }
}

/** The arena scsynth asks for, in bytes, never below what it used to get. */
/*
 * The verbs that cannot reach the engine as sent.
 *
 * scsynth's allocate-from-a-file commands name a PATH, and the engine has no
 * filesystem — in a browser it never did. So they are answered on this side:
 * the material is fetched and decoded here, staged into our slice of guest
 * memory, and what actually reaches the engine is /b_allocPtr with an
 * address. Purely a product concern; clockwork forwards verbs it was never
 * asked to understand.
 */
const BUFFER_ALLOC_COMMANDS = new Set([
  "/b_alloc", "/b_allocRead", "/b_allocReadChannel", "/b_allocFile",
]);

export { OscChannel } from "../clockwork/js/osc_channel.js";

const looksLikePathOrURL = (s) =>
  s.includes("/") || s.includes("\\") || s.startsWith("http") || s.endsWith(".scsyndef");

export class SuperSonic extends Clockwork {
  /**
   * The metrics SuperSonic reports: clockwork's, plus scsynth's own.
   *
   * Overriding the static form matters because callers inspect what a product
   * reports without booting one — the suite does exactly that.
   */
  static getMetricsSchema() {
    return Clockwork.mergeGuestMetrics(scsynthProfile.metrics, scsynthProfile.metricsPanels);
  }

  #synthdefBaseURL;
  #sampleBaseURL;
  #bufferManager = null;
  #buffersInit;
  #buffersMax;
  #numBuffers;
  #scsynthOptions;
  #rewriter = null;
  // the buffer manager's ear on the egress, while there is one (#buffers, shutdown)
  #onBufferReply = null;
  #bufferQueue = Promise.resolve();
  /*
   * The synthdefs this client has loaded, name → bytes.
   *
   * TAU USED TO KEEP THIS and should never have. It watched for
   * /d_recv, /d_free and /d_freeAll going past — verbs it was handed by one
   * engine — and kept a copy against a device switch, which asked clockwork
   * to know which of a guest's messages carry state worth keeping. That went
   * on 2026-08-31 with the rest of the definition cache, on the understanding
   * that restore across a rebuild is the client's job. Only the buffer half
   * of that landed; this is the other half.
   *
   * The bytes are kept, not just the names, because restoring means sending
   * the definition again — a name alone cannot be replayed. They are the same
   * blobs the caller already handed over, so this costs one reference each.
   */
  #loadedSynthDefs = new Map();

  constructor(options = {}) {
    /*
     * `scsynthOptions` is what a SuperSonic caller calls the engine's config —
     * the name the whole suite and Sonic Pi use — and it is scsynth's schema:
     * numBuffers, maxNodes, maxWireBufs, numRGens, realTimeMemorySize and the
     * rest. Clockwork knows this object only as opaque bytes it copies into
     * a region. Its defaults, its validation and its binary layout are all
     * scsynth knowledge, so all three live here.
     *
     * `worldOptions` is still accepted as a spelling of the same thing. It was
     * clockwork's word for it until 2026-08-31, when "world" went back to
     * being scsynth's concept rather than everyone's.
     */
    const scOpts = { ...defaultScsynthOptions,
                     ...options.worldOptions, ...options.scsynthOptions };
    validateScsynthOptions(scOpts);
    // The guest's vocabulary, unless the caller overrides it. Without this
    // clockwork falls back to NO_DSP: nothing is cached across a device switch
    // and sync() refuses, because no verb asks.
    // The artifact this product ships. clockwork's default name is
    // deliberately guest-agnostic — it cannot know what its guest compiled to —
    // so naming the file is SuperSonic's job, the same way declaring the
    // vocabulary is.
    // Same precedence as clockwork's own: the core package's directory
    // (coreBaseURL) holds the wasm; baseURL only stands in for it when no
    // core directory was named. Ignoring coreBaseURL here sent a CDN boot to
    // <client package>/dist/wasm/, which ships no wasm — every documented
    // CDN configuration 404'd at init (0.81.0).
    const wasmBase = options.wasmBaseURL
      || (options.coreBaseURL ? `${options.coreBaseURL}wasm/` : null)
      || (options.baseURL ? `${options.baseURL}wasm/` : null);
    const wasmUrl = options.wasmUrl
      || (wasmBase ? `${wasmBase}scsynth-nrt.wasm` : undefined);

    const bufInit  = options.bufferPoolSize ?? BUFFERS_INIT;
    const bufMax   = options.maxBufferMemory ?? BUFFERS_MAX;
    const memory   = { ...options.memory };
    /*
     * The engine's real-time pool comes out of clockwork's placement arena,
     * so the arena has to be at least as big as the pool this guest is about
     * to ask for. scsynth asks in `realTimeMemorySize` KB, which is scsynth's
     * word, known only here.
     *
     * This is the arithmetic that used to size an RT arena inside the guest's
     * own region and pass its offset to the engine in a config slot. The
     * region moved and the side channel went; the sum did not change, and it
     * still belongs to the guest's client rather than to clockwork.
     *
     * The headroom covers what else is taken from the same span —
     * clockwork's own heap, and the pool's area bookkeeping — so a default
     * config does not land exactly on the boundary.
     */
    const rtBytes = scOpts.realTimeMemorySize ?? 8192;
    memory.memArenaSize = memory.memArenaSize
      ?? Math.max(MemoryLayout.memArenaSize, rtBytes * 1024 + RT_ARENA_HEADROOM);
    // /b_allocPtr NAMES A SAMPLE'S POSITION AS AN OFFSET FROM THE INBOX BASE,
    // never as an address: the guest adds the inbox pointer it was handed at
    // dsp_new. The buffer manager computes that offset once (laneOffset) and
    // every sender — here and the OSC rewriter — reads it, so there is one
    // subtraction to get right. On the web the sum is the address this side
    // already had; natively the engine maps the lane wherever it likes, and
    // an address from here would name the wrong bytes without faulting
    // (dsp_api.h: "OFFSETS, NOT POINTERS").
    // THE SAMPLE POOL LIVES IN THE INBOX. It is bulk the client writes and the
    // guest reads in place — /b_allocPtr points a buffer straight at it — which
    // is what the inbox is for, and it is the region clockwork grows.
    memory.inboxSize    = memory.inboxSize    ?? bufInit;
    memory.maxInboxSize = memory.maxInboxSize ?? bufMax;

    super({
      guestMetrics: scsynthProfile.metrics,
      guestMetricsPanels: scsynthProfile.metricsPanels,
      ...options,
      memory,
      // Opaque to clockwork; this class encodes it in encodeGuestConfig.
      guestOptions: { ...scOpts },
      // What clockwork needs in its OWN words: the channels it must open on
      // the audio graph, and the channels it must read from the device.
      audio: {
        outputChannels: scOpts.numOutputBusChannels,
        inputChannels: scOpts.numInputBusChannels,
      },
      ...(wasmUrl ? { wasmUrl } : {}),
    });

    this.#scsynthOptions = { ...scOpts };
    this.#buffersInit  = bufInit;
    this.#buffersMax   = bufMax;
    this.#numBuffers   = scOpts.numBuffers;
    /*
     * maxNodes against the mirror's capacity.
     *
     * Clockwork warned about this until 2026-08-31, which meant comparing a
     * number it knew only as `maxNodes` — scsynth's word — against a capacity
     * it publishes generically. It publishes the capacity; deciding whether it
     * is enough is arithmetic in the guest's own units, so it happens here.
     */
    this.on("ready", () => {
      // Clockwork publishes the window's SIZE; how many nodes fit is this
      // guest's arithmetic, the same sum node_tree.h does for
      // NODE_TREE_MIRROR_MAX_NODES, in the same units.
      const windowBytes = this.bufferConstants?.SHM_WINDOW_SIZE;
      if (!windowBytes) return;
      const mirrorMax = Math.floor((windowBytes - NODE_TREE_HEADER_SIZE) / NODE_TREE_ENTRY_SIZE);
      if (scOpts.maxNodes > mirrorMax) {
        const needed = NODE_TREE_HEADER_SIZE + scOpts.maxNodes * NODE_TREE_ENTRY_SIZE;
        console.warn(
          `SuperSonic: maxNodes (${scOpts.maxNodes}) exceeds `
          + `NODE_TREE_MIRROR_MAX_NODES (${mirrorMax}). Nodes beyond it play `
          + `normally but will not appear in getTree(); droppedCount counts `
          + `them. Rebuild with -DCLOCKWORK_WINDOW_BYTES=${needed} to see them all.`);
      }
    });

    this.#sampleBaseURL = options.sampleBaseURL
      || (options.baseURL ? `${options.baseURL}samples/` : null);
    // Derived from baseURL when not given, as upstream did: a caller that
    // co-locates its assets should not have to name each tree.
    this.#synthdefBaseURL = options.synthdefBaseURL
      || (options.baseURL ? `${options.baseURL}synthdefs/` : null);
  }

  /**
   * Load one synthdef: a bare name, a path or URL, or the bytes themselves.
   *
   * A bare name is resolved against synthdefBaseURL — that resolution is the
   * whole reason this method exists rather than callers using send() directly.
   */
  async loadSynthDef(source) {
    let bytes;

    if (typeof source === "string") {
      const path = looksLikePathOrURL(source)
        ? source
        : (() => {
            if (!this.#synthdefBaseURL) throw new Error("synthdefBaseURL not configured.");
            return `${this.#synthdefBaseURL}${source}.scsyndef`;
          })();
      // Through clockwork's loader, not a bare fetch: it carries the retry
      // policy AND emits loading:start / loading:complete, which is how a
      // client shows progress. A raw fetch works and is silent, so the events
      // simply never fired.
      const pathName = (path.split("/").pop() || path).replace(/\.scsyndef$/i, "");
      const buf = await this.assetLoader.fetch(path, { type: "synthdef", name: pathName });
      bytes = new Uint8Array(buf);

    } else if (source instanceof ArrayBuffer) {
      bytes = new Uint8Array(source);
    } else if (ArrayBuffer.isView(source)) {
      bytes = new Uint8Array(source.buffer, source.byteOffset, source.byteLength);
    } else if (typeof Blob !== "undefined" && source instanceof Blob) {
      bytes = new Uint8Array(await source.arrayBuffer());
    } else {
      throw new Error(
        "loadSynthDef source must be a name, path/URL string, ArrayBuffer, Uint8Array, or File/Blob");
    }

    // The name is read by the DSP's own profile, not by anything here — the
    // same function clockwork uses to key its definition cache.
    const name = scsynthProfile.nameOf(bytes);
    if (!name) {
      throw new Error("Could not extract synthdef name from the data. Make sure it is a valid .scsyndef file.");
    }

    // send() records it — this call goes through the same interception a
    // caller writing /d_recv by hand does, so there is one place that knows
    // what has been loaded rather than two that can drift.
    await this.send(scsynthProfile.defineVerb, bytes);
    // `{name, size}`, not a bare name: callers want to know how much went
    // over, and the suite reads both. Upstream's shape, kept deliberately.
    return { name, size: bytes.length };
  }

  /**
   * The synthdefs loaded through this client, as a Map of name → bytes.
   *
   * Live, not a copy: `.has(name)` and `.size` are what callers and the suite
   * ask, and handing back a clone on every access would make a hot path out of
   * a bookkeeping read.
   */
  get loadedSynthDefs() { return this.#loadedSynthDefs; }

  /** Several, in parallel. Returns their names in the order given. */
  async loadSynthDefs(names) {
    return Promise.all(names.map((n) => this.loadSynthDef(n)));
  }

  // ── Samples ──────────────────────────────────────────────────────────────
  //
  // Clockwork has no idea what a sample is. It reserved a region and will
  // move opaque bytes into it on request; everything below — decoding,
  // interleaving, guard frames, the bufnum table — is what scsynth means by a
  // buffer, so it lives here.

  /**
   * The buffer manager, built on first use.
   *
   * Lazily, because it needs a booted engine: the audio context does the
   * decoding and the guest region does not exist until memory is initialised.
   */
  #buffers() {
    if (this.#bufferManager) return this.#bufferManager;

    const pool = this.inbox();
    this.#bufferManager = new BufferManager({
      clockwork: this,
      mode: this.mode,
      audioContext: this.audioContext,
      sharedBuffer: this.sharedBuffer,
      wasmMemory: this.wasmMemory,
      // The whole inbox: nothing is reserved ahead of the pool.
      bufferPoolConfig: {
        start: pool.offset,
        size: this.#buffersInit,
        maxSize: this.#buffersMax,
      },
      maxBufferMemory: this.#buffersMax,
      assetLoader: this.assetLoader,
      sampleBaseURL: this.#sampleBaseURL,
      maxBuffers: this.#numBuffers,
      onBufferPoolGrowth: (info) => this.emit("buffer:pool:grown", info),
    });

    this.#rewriter = new OSCRewriter({
      bufferManager: this.#bufferManager,
      getDefaultSampleRate: () => this.audioContext?.sampleRate || 44100,
    });

    // The engine answers an allocation on the egress rather than with /done, so the completion is picked out of the
    // inbound stream. One listener per manager, taken off with it (shutdown): a listener left from an engine before
    // would answer every allocation again.
    const manager = this.#bufferManager;
    this.#onBufferReply = ({ oscData }) => {
      let msg;
      try { msg = oscFast.decodePacket(oscData); } catch { return; }
      const [address, ...args] = msg;
      if (address === "/supersonic/buffer/allocated") manager.handleBufferAllocated(args);
      else if (address === "/supersonic/buffer/freed") manager.handleBufferFreed(args);
    };
    this.on("in:osc", this.#onBufferReply);

    return this.#bufferManager;
  }

  /**
   * scsynth's config block, in the byte layout its C++ reads.
   *
   * Eighteen slots, seventeen of which are scsynth's own fields. Clockwork
   * wrote them itself until 2026-08-31 — every field name and every index
   * hardcoded in a guest-agnostic worklet — which meant no other guest could
   * be configured without editing it. Clockwork reserves the region and
   * copies these bytes; only this side knows what they say.
   */
  encodeGuestConfig(ctx) {
    return encodeScsynthOptions(this.#scsynthOptions, ctx);
  }

  /**
   * Send, intercepting the verbs that must be answered client-side.
   *
   * They are queued rather than sent: rewriting is asynchronous — it may
   * fetch and decode a file — and two allocations racing would interleave
   * their pointers. The queue keeps them in the order the caller wrote them.
   */
  send(address, ...args) {
    // Nothing is refused here. Until 2026-09-13 eight verbs were — the
    // file-path loads the browser cannot serve, /clearSched, /error — with a
    // friendlier message than the engine's. The engine's own /fail is the
    // honest answer for the seven, and /error -1/-2 is a standard way to
    // quiet a bundle's failures that a blanket refusal forbade.
    // The definition verbs, tracked on the way past.
    //
    // Not intercepted — every one of these still goes to the engine exactly as
    // written. The client only notes what it will have to put back after a
    // rebuild, which is the job clockwork gave up when it stopped reading a
    // guest's messages to find its state.
    if (address === scsynthProfile.defineVerb) {
      const blob = args.find((a) => a instanceof ArrayBuffer || ArrayBuffer.isView(a));
      if (blob) {
        const bytes = blob instanceof ArrayBuffer
          ? new Uint8Array(blob)
          : new Uint8Array(blob.buffer, blob.byteOffset, blob.byteLength);
        // A blob whose name will not parse is still sent — refusing it is the
        // engine's call, not ours — it simply cannot be replayed later.
        const name = scsynthProfile.nameOf(bytes);
        if (name) this.#loadedSynthDefs.set(name, bytes);
      }
    } else if (address === scsynthProfile.forgetVerb) {
      const name = args.find((a) => typeof a === "string");
      if (name) this.#loadedSynthDefs.delete(name);
    } else if (address === scsynthProfile.forgetAllVerb) {
      this.#loadedSynthDefs.clear();
    }

    if (!BUFFER_ALLOC_COMMANDS.has(address)) return super.send(address, ...args);

    const normalized = args.map((a) => (a instanceof ArrayBuffer ? new Uint8Array(a) : a));
    // Validated HERE, synchronously, before anything is queued: a caller that
    // wrote a bad command should have it thrown back at the call, not learn
    // about it later on the error channel with no stack pointing at them.
    validateBufferCommand(address, normalized);
    this.#bufferQueue = this.#bufferQueue
      .then(async () => {
        this.#buffers();
        const { packet } = await this.#rewriter.rewritePacket([address, ...normalized]);
        return super.send(packet[0], ...packet.slice(1));
      })
      .catch((error) => {
        // The caller has already been handed a void return, so the only way
        // this can be seen is on the error channel.
        console.error(`[SuperSonic] ${address} failed:`, error);
        this.emit?.("error", error);
      });
    return undefined;
  }

  /**
   * Values for the metrics scsynth_profile declares — flat, by declared name.
   *
   * Clockwork used to be handed nested `bufferPoolStats` objects and know
   * how to unpack them, which put scsynth's shapes inside guest-agnostic
   * code. It takes declared names and nothing else now.
   */
  clientMetrics() {
    const stats  = this.#bufferManager?.getStats();
    const growth = this.#bufferManager?.getGrowthStats();
    return {
      bufferPoolUsedBytes:      stats?.used?.size ?? 0,
      bufferPoolAvailableBytes: stats?.available ?? 0,
      bufferPoolAllocations:    stats?.used?.count ?? 0,
      bufferPoolTotalCapacity:  growth?.totalCapacity ?? 0,
      bufferPoolMaxCapacity:    growth?.maxCapacity ?? 0,
      bufferPoolGrowthCount:    growth?.growthCount ?? 0,
      bufferPoolPoolCount:      growth?.poolCount ?? 0,
      loadedSynthDefs:          this.#loadedSynthDefs.size,
    };
  }

  /**
   * Put the sample buffers back after a reload.
   *
   * The frames themselves are still in guest memory — a reload rebuilds the
   * engine, not the region — so this only has to hand the engine the pointers
   * again. In postMessage mode a buffer that came from a file is reloaded
   * from it instead, because the client's pool there is bookkeeping and the
   * worklet's heap went with the engine.
   */
  async restoreClientState() {
    // The reload may have made a new audio context (clockwork's own, when it made the last one): the buffers are
    // decoded, and their default rate read, through the one the engine now plays in.
    if (this.audioContext) this.#bufferManager?.updateAudioContext(this.audioContext);

    // DEFINITIONS FIRST. A buffer is just frames, but a synth made from a
    // definition the rebuilt engine has not been given fails at /s_new — so
    // the definitions go back before anything that might reference them.
    for (const [name, bytes] of this.#loadedSynthDefs) {
      try {
        await super.send(scsynthProfile.defineVerb, bytes);
      } catch (e) {
        console.error(`[SuperSonic] synthdef ${name} did not survive the reload:`, e);
      }
    }

    const buffers = this.#bufferManager?.getAllocatedBuffers() || [];
    for (const buf of buffers) {
      try {
        if (this.mode === "postMessage" && buf.source?.type === "file") {
          await this.loadSample(buf.bufnum, buf.source.path,
                                buf.source.startFrame || 0, buf.source.numFrames || 0);
        } else {
          await this.send("/b_allocPtr", buf.bufnum, buf.laneOffset, buf.numFrames,
                          buf.numChannels, buf.sampleRate, crypto.randomUUID());
        }
      } catch (e) {
        console.error(`[SuperSonic] buffer ${buf.bufnum} did not survive the reload:`, e);
      }
    }
  }

  /**
   * Full teardown, which forgets what was loaded.
   *
   * reset() is shutdown + init, so the engine that comes back has been given
   * nothing and this client must not claim otherwise. reload() does NOT come
   * through here — it partially tears down and calls restoreClientState(),
   * which needs the record intact to put the definitions back.
   */
  async shutdown(...args) {
    this.#loadedSynthDefs.clear();
    // and what was allocated: the buffer manager belongs to the engine it was built against (its memory, its pool, its
    // table of buffers), so the next engine gets a new one, built on first use as the first did
    if (this.#onBufferReply) this.off("in:osc", this.#onBufferReply);
    this.#onBufferReply = null;
    this.#bufferManager = null;
    this.#rewriter = null;
    return super.shutdown(...args);
  }

  /** Settle any queued buffer commands — tests and shutdown both need this. */
  drainBufferQueue() { return this.#bufferQueue; }

  /**
   * Sync, after the queued buffer commands have actually gone out.
   *
   * Interception makes those commands asynchronous, so a caller that writes
   * `/b_allocFile` then `sync()` would otherwise pass the barrier before the
   * allocation had been sent — the barrier would be telling the truth about
   * an engine that had not yet been asked. Draining first is what makes the
   * sequence mean what it reads like.
   */
  async sync(...args) {
    await this.#bufferQueue;
    return super.sync(...args);
  }

  /**
   * clockwork's request(), with scsynth's word for a refusal filled in:
   * `/fail` rejects unless the caller names another.
   */
  request(address, args = [], options = {}) {
    return super.request(address, args, { error: "/fail", ...options });
  }

  /**
   * Load audio into a buffer number.
   *
   * `source` may be a path or URL resolved against sampleBaseURL, raw bytes,
   * or a File/Blob. The frames are decoded, staged into our slice of guest
   * memory, and the engine is handed the pointer — the bytes never ride OSC.
   */
  async loadSample(bufnum, source, startFrame = 0, numFrames = 0) {
    const buffers = this.#buffers();

    let info;
    if (typeof source === "string") {
      info = await buffers.prepareFromFile({ bufnum, path: source, startFrame, numFrames });
    } else if (source instanceof ArrayBuffer || ArrayBuffer.isView(source)) {
      info = await buffers.prepareFromBlob({ bufnum, blob: source, startFrame, numFrames });
    } else if (typeof Blob !== "undefined" && source instanceof Blob) {
      info = await buffers.prepareFromBlob({
        bufnum, blob: await source.arrayBuffer(), startFrame, numFrames,
      });
    } else {
      throw new Error("loadSample source must be a path/URL, ArrayBuffer, TypedArray, or Blob");
    }

    await this.send("/b_allocPtr", bufnum, info.laneOffset, info.numFrames,
                    info.numChannels, info.sampleRate, info.uuid);
    await info.allocationComplete;

    const { numFrames: frames, numChannels: channels, sampleRate: sr } = info;
    return {
      bufnum,
      hash: info.hash,
      source: typeof source === "string" ? source : null,
      numFrames: frames,
      numChannels: channels,
      sampleRate: sr,
      duration: sr > 0 ? frames / sr : 0,
    };
  }

  /** Allocate an empty buffer: the same path, with no material to decode. */
  async allocSample(bufnum, numFrames, numChannels = 1, sampleRate = null) {
    const buffers = this.#buffers();
    const info = await buffers.prepareEmpty({ bufnum, numFrames, numChannels, sampleRate });
    await this.send("/b_allocPtr", bufnum, info.laneOffset, info.numFrames,
                    info.numChannels, info.sampleRate, info.uuid);
    await info.allocationComplete;
    return { bufnum, numFrames: info.numFrames, numChannels: info.numChannels,
             sampleRate: info.sampleRate };
  }

  /** What is loaded, for a client that wants to show it. */
  // ── The node tree ─────────────────────────────────────────────────────────
  //
  // scsynth publishes its tree into the window clockwork reserves. The host
  // knows only that the window's first word is a version stamp; the shape of
  // the rest is ours, so the parsing is here rather than there.

  /**
   * What getTree() and getRawTree() hand back, described for a consumer that
   * builds a UI or an inspector from the shape rather than from reading this
   * file. Static, because the shape is a property of this class, not of any
   * one engine.
   *
   * Kept honest against the parser, not against intent: an earlier version
   * said `id` was a number when getTree() gives a node its UUID whenever it
   * has one, and left out every v2 field the parser produces. schema.spec.mjs
   * now compares these against a real tree, so the two cannot drift apart
   * silently again.
   */
  static getTreeSchema() {
    return {
      nodeCount: { type: 'number', description: 'Nodes present in the mirror' },
      version: { type: 'number', description: 'Increments on any change; watch it to know when to re-read' },
      droppedCount: { type: 'number', description: 'Nodes beyond the mirror\'s capacity, absent below' },
      root: {
        type: 'object',
        nullable: true,
        description: 'The root group, or null before the engine has published one',
        schema: {
          id: { type: ['uuid', 'number'], description: 'The node\'s UUID (16 bytes) when it has one, else its numeric id' },
          type: { type: 'string', values: ['group', 'synth'], description: 'Group or synth' },
          defName: { type: 'string', description: 'Synthdef name; empty for a group' },
          children: { type: 'array', description: 'Child nodes in sibling order (recursive)', itemSchema: '(self)' },
        },
      },
    };
  }

  static getRawTreeSchema() {
    return {
      nodeCount: { type: 'number', description: 'Nodes present in the mirror' },
      version: { type: 'number', description: 'Increments on any change; watch it to know when to re-read' },
      droppedCount: { type: 'number', description: 'Nodes beyond the mirror\'s capacity, absent below' },
      nodes: {
        type: 'array',
        description: 'Every node as the mirror holds it, with its linkage intact',
        itemSchema: {
          id: { type: 'number', nullable: true, description: 'Numeric node id; null for a row the engine surface cannot address' },
          parentId: { type: 'number', description: 'Parent node id; -1 for the root' },
          isGroup: { type: 'boolean', description: 'True for a group, false for a synth' },
          prevId: { type: 'number', description: 'Previous sibling; -1 if none' },
          nextId: { type: 'number', description: 'Next sibling; -1 if none' },
          headId: { type: 'number', description: 'First child (groups); -1 if empty' },
          defName: { type: 'string', description: 'Synthdef name; empty for a group' },
          uuid: { type: 'uuid', nullable: true, description: '16-byte UUID, or null when the node has none' },
          parentUuid: { type: 'uuid', nullable: true, description: 'Parent\'s UUID, or null' },
          outPeak: { type: 'number', description: 'Peak of the node\'s output over the last block' },
          synthCount: { type: 'number', description: 'Synths under this node, itself included' },
          listens: { type: 'boolean', description: 'Whether the node subscribes to input' },
        },
      },
    };
  }

  getRawTree() {
    const w = this.readWindow();
    if (!w) return { nodeCount: 0, version: 0, droppedCount: 0, nodes: [] };
    return parseNodeTree(w.buffer, w.offset, w.size);
  }

  getTree() {
    const raw = this.getRawTree();

    // One map per node holding both raw + tree representations, finding the
    // root in the same pass.
    const byId = new Map();
    let rootRaw = null;
    for (const rawNode of raw.nodes) {
      const tree = {
        id: rawNode.uuid || rawNode.id,
        type: rawNode.isGroup ? 'group' : 'synth',
        defName: rawNode.defName,
        children: [],
      };
      byId.set(rawNode.id, { raw: rawNode, tree });
      if (rawNode.parentId === -1) rootRaw = rawNode;
    }

    // Hang each group's children off it IN SIBLING ORDER: from the group's
    // headId along nextId, which is the order scsynth plays them and the
    // order /n_order and HEAD-action inserts change. The mirror's row order
    // is allocation order and says nothing about siblings — walking the rows
    // instead put every group's children in the order they were created,
    // which is wrong the moment anything is moved or inserted at the head.
    for (const { raw: groupRaw, tree: groupTree } of byId.values()) {
      if (!groupRaw.isGroup) continue;
      const seen = new Set();
      for (let id = groupRaw.headId; id !== -1 && !seen.has(id); ) {
        seen.add(id);                       // a cycle is corruption, not a loop
        const child = byId.get(id);
        if (!child) break;
        groupTree.children.push(child.tree);
        id = child.raw.nextId;
      }
    }

    return {
      nodeCount: raw.nodeCount,
      version: raw.version,
      droppedCount: raw.droppedCount,
      root: rootRaw ? byId.get(rootRaw.id).tree : null,
    };
  }

  getLoadedBuffers() {
    const buffers = this.#bufferManager?.getAllocatedBuffers() || [];
    return buffers.map(({ bufnum, numFrames, numChannels, sampleRate, source, hash }) => ({
      bufnum,
      hash: hash || null,
      source: source?.path || source?.name || null,
      numFrames, numChannels, sampleRate,
      duration: sampleRate > 0 ? numFrames / sampleRate : 0,
    }));
  }

  /** Read a file's shape without loading it into the engine. */
  async sampleInfo(source, startFrame = 0, numFrames = 0) {
    const src = (typeof Blob !== "undefined" && source instanceof Blob)
      ? await source.arrayBuffer() : source;
    return this.#buffers().sampleInfo({ source: src, startFrame, numFrames });
  }
}

/*
 * The OSC codec, re-exported.
 *
 * Clockwork publishes it (`export const osc` in js/clockwork.js) and upstream
 * SuperSonic published it too, as `SuperSonic.osc`. This client extends
 * Clockwork rather than re-declaring its surface, and a `class ... extends`
 * inherits STATICS but says nothing about MODULE exports — so `osc` silently
 * stopped being importable from the product's entry point while remaining
 * importable from the substrate's.
 *
 * Anything importing it from here got `undefined` and failed at first use,
 * far from the missing line: the demo's scheduler worker builds every note
 * with osc.encodeMessage, so the whole page loaded, reported "engine ready",
 * and made no sound at all.
 */
export const osc = SuperSonic.osc;

export default SuperSonic;

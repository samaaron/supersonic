/*
    scsynth Runtime Configuration

    Runtime options (scsynthOptions):
      - Configurable via clockwork constructor (no rebuild required)
      - Passed to the DSP World_New()
      - Must fit within build-time memory allocations

    Build-time configuration (memory layout):
      - Defined in js/memory_layout.js
      - Requires scripts/build-web.sh when changed

    Can be overridden via:
      - new clockwork({ the DSPOptions: { numBuffers: 2048 } })

    Memory usage validation:
      - Total allocations must fit within wasmHeapSize (~16MB by default)
 */
const defaultScsynthOptions = {
  /**
   * Maximum number of audio buffers (SndBuf slots)
   * Each buffer slot: 104 bytes overhead (2x SndBuf + SndBufUpdates structs)
   * Actual audio data is stored in buffer pool (separate from heap)
   * Default: 1024 (matching SuperCollider default)
   * Range: 1-65535 (limited by practical memory constraints)
   */
  numBuffers: 1024,

  /**
   * Maximum number of synthesis nodes (synths + groups)
   * Each node: ~200-500 bytes depending on synth complexity
   * Default: 1024 (matching SuperCollider default)
   */
  maxNodes: 8192,

  /**
   * Maximum number of synth definitions (SynthDef count)
   * Each definition: variable size (typically 1-10KB)
   * Default: 1024 (matching SuperCollider default)
   */
  maxGraphDefs: 1024,

  /**
   * Maximum wire buffers for internal audio routing
   * Wire buffers: temporary buffers for UGen connections
   * Each: bufLength * sizeof(float) bytes (128 samples * 4 = 512 bytes)
   * Default: 64 (matching SuperCollider default)
   */
  maxWireBufs: 64,

  /**
   * Number of audio bus channels
   * Audio buses: real-time audio routing between synths
   * Memory: bufLength * numChannels * 4 bytes (128 * 128 * 4 = 64KB)
   * Default: 128 (clockwork default, SC uses 1024)
   */
  numAudioBusChannels: 128,

  /**
   * Number of input bus channels (hardware audio input)
   * Allocates space for up to N input channels from AudioContext
   * Actual channels used depends on hardware (worklet copies min(N, actual))
   * Default: 2 (stereo)
   */
  numInputBusChannels: 2,

  /**
   * Number of output bus channels (hardware audio output)
   * Allocates space for up to N output channels to AudioContext
   * Actual channels used depends on hardware (worklet copies min(N, actual))
   * Default: 2 (stereo)
   */
  numOutputBusChannels: 2,

  /**
   * Number of control bus channels
   * Control buses: control-rate data sharing between synths
   * Memory: numChannels * 4 bytes (4096 * 4 = 16KB)
   * Default: 4096 (clockwork default, SC uses 16384)
   */
  numControlBusChannels: 4096,

  /**
   * Audio buffer length in samples (AudioWorklet quantum)
   *
   * FIXED at 128 (WebAudio API spec - cannot be changed)
   * Unlike SuperCollider (configurable 32/64/128), AudioWorklet has a fixed quantum.
   * Overriding this value will cause initialization to fail.
   *
   * Default: 128
   */
  bufLength: 128,

  /**
   * Real-time memory pool size in kilobytes
   * AllocPool for synthesis-time allocations (UGen memory, etc.)
   * This is the largest single allocation from WASM heap
   * Memory: realTimeMemorySize * 1024 bytes (8192 * 1024 = 8MB)
   * Default: 8192 KB (8MB, matching Sonic Pi and SuperCollider defaults)
   */
  realTimeMemorySize: 8192,

  /**
   * Number of random number generators
   * Each synth can have its own RNG for reproducible randomness
   * Default: 64 (matching SuperCollider default)
   */
  numRGens: 64,

  /**
   * Clock source mode
   * false = Externally clocked (driven by AudioWorklet process() callback)
   * true = Internally clocked (not applicable in WebAudio context)
   * Note: In SC terminology, this is "NRT mode" but we're still doing real-time audio
   * Default: false (clockwork is always externally clocked by AudioWorklet)
   */
  realTime: false,

  /**
   * Memory locking (mlock)
   * Not applicable in WebAssembly/browser environment
   * Default: false
   */
  memoryLocking: false,

  /**
   * Auto-load SynthDefs from disk
   * 0 = don't auto-load (synths sent via /d_recv)
   * 1 = auto-load from plugin path
   * Default: 0 (clockwork loads synthdefs via network)
   */
  loadGraphDefs: 0,

  /**
   * Preferred sample rate (if not specified, uses AudioContext.sampleRate)
   * Common values: 44100, 48000, 96000
   * Default: 0 (use AudioContext default, typically 48000)
   */
  preferredSampleRate: 0,

  /**
   * Debug verbosity level
   * 0 = normal (default), 1+ = increasingly verbose. SC uses negative values for quieter modes but clockwork clamps to 0-4.
   * Default: 0
   */
  verbosity: 0,
};

export { defaultScsynthOptions };
export default defaultScsynthOptions;

/*
 * Validation, moved out of clockwork on 2026-08-31.
 *
 * Every rule here names an scsynth field and an scsynth range. It sat in
 * clockwork.js, which is supposed not to know what engine is underneath: a guest
 * with none of these fields could not satisfy it and had nothing of its own
 * it could have checked instead.
 */
export function validateScsynthOptions(opts) {
  /*
   * The ranges are scsynth's, ported unchanged from clockwork. Defaults
   * are merged in before this runs, so every field is present and each is
   * checked unconditionally — an undefined here means a caller passed one
   * explicitly as undefined, which is worth refusing.
   */
  const numericRules = [
    ["numBuffers", 1, 65535],
    ["maxNodes", 1],
    ["maxGraphDefs", 1],
    ["maxWireBufs", 1],
    ["numAudioBusChannels", 1],
    ["numInputBusChannels", 0],
    ["numOutputBusChannels", 1, 128],
    ["numControlBusChannels", 1],
    ["realTimeMemorySize", 1],
    ["numRGens", 1],
    ["preferredSampleRate", 0, 384000],
    ["verbosity", 0, 4],
  ];
  for (const [name, min, max] of numericRules) {
    const v = opts[name];
    if (typeof v !== "number" || !Number.isFinite(v)) {
      throw new Error(`scsynthOptions.${name} must be a finite number, got: ${v}`);
    }
    if (v < min) throw new Error(`scsynthOptions.${name} must be >= ${min}, got: ${v}`);
    if (max !== undefined && v > max) {
      throw new Error(`scsynthOptions.${name} must be <= ${max}, got: ${v}`);
    }
  }

  // 128 is not scsynth's rule but AudioWorklet's: it renders 128 frames and
  // nothing else. It is checked here because this is where the field with
  // that name lives.
  if (opts.bufLength !== 128) {
    throw new Error(
      `scsynthOptions.bufLength must be 128 (WebAudio API constraint), got: ${opts.bufLength}`);
  }
  for (const name of ["realTime", "memoryLocking"]) {
    if (typeof opts[name] !== "boolean") {
      throw new Error(`scsynthOptions.${name} must be a boolean, got: ${typeof opts[name]}`);
    }
  }
  if (opts.loadGraphDefs !== 0 && opts.loadGraphDefs !== 1) {
    throw new Error(`scsynthOptions.loadGraphDefs must be 0 or 1, got: ${opts.loadGraphDefs}`);
  }
  if (opts.preferredSampleRate !== 0 && opts.preferredSampleRate < 8000) {
    throw new Error(
      `scsynthOptions.preferredSampleRate must be 0 (auto) or >= 8000, got: ${opts.preferredSampleRate}`);
  }
}

/*
 * The binary block scsynth's C++ reads at GUEST_CONFIG_START.
 *
 * ORDER IS THE CONTRACT. These indices are read by name in
 * audio_processor.cpp through sonicpi::WorldOpts, so a slot moved here is a
 * different option there, silently. Clockwork used to write this layout
 * itself, field by field, in its worklet — which is why no second guest could
 * be configured without editing clockwork code.
 *
 * Slot 17 is the one value this side cannot know: which transport clockwork
 * opened. It arrives as `ctx.mode` rather than being written by clockwork
 * into a slot it would otherwise have to know the number of.
 */
export function encodeScsynthOptions(o, ctx = {}) {
  const buf = new ArrayBuffer(18 * 4);
  const u32 = new Uint32Array(buf);
  u32[0]  = o.numBuffers ?? 1024;
  u32[1]  = o.maxNodes ?? 1024;
  u32[2]  = o.maxGraphDefs ?? 1024;
  u32[3]  = o.maxWireBufs ?? 64;
  u32[4]  = o.numAudioBusChannels ?? 128;
  u32[5]  = o.numInputBusChannels ?? 0;
  u32[6]  = o.numOutputBusChannels ?? 0;
  u32[7]  = o.numControlBusChannels ?? 4096;
  u32[8]  = o.bufLength ?? 128;
  u32[9]  = o.realTimeMemorySize ?? 16384;
  u32[10] = o.numRGens ?? 64;
  u32[11] = o.realTime ? 1 : 0;
  u32[12] = o.memoryLocking ? 1 : 0;
  u32[13] = o.loadGraphDefs ?? 0;
  u32[14] = o.preferredSampleRate ?? 0;
  u32[15] = o.verbosity ?? 0;
  // Slot 16 carried rtPoolOffset until 2026-09-01: where the client had
  // carved an RT arena out of the front of its own guest region, read back by
  // the engine through two file-scope externs. The engine takes its pool from
  // clockwork::mem now, like every other target, so nothing writes this slot.
  u32[17] = ctx.mode === "postMessage" ? 1 : 0;
  return buf;
}

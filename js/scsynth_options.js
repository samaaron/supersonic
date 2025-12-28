/*
    SuperSonic Runtime Configuration

    Runtime options (worldOptions):
      - Configurable via SuperSonic constructor (no rebuild required)
      - Passed to scsynth World_New()
      - Must fit within build-time memory allocations

    Build-time configuration (memory layout):
      - Defined in js/memory_layout.js
      - Requires ./build.sh when changed

    Can be overridden via:
      - new SuperSonic({ scsynthOptions: { numBuffers: 2048 } })

    Memory usage validation:
      - Total allocations must fit within wasmHeapSize (~16MB by default)
 */
const defaultWorldOptions = {
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
  maxNodes: 1024,

  /**
   * Maximum number of synth definitions (SynthDef count)
   * Each definition: variable size (typically 1-10KB)
   * Default: 1024 (matching SuperCollider default)
   */
  maxGraphDefs: 1024,

  /**
   * Maximum wire buffers for internal audio routing
   * Wire buffers: temporary buffers for UGen connections
   * Each: bufLength * 8 bytes (128 samples * 8 = 1024 bytes)
   * Default: 64 (matching SuperCollider default)
   */
  maxWireBufs: 64,

  /**
   * Number of audio bus channels
   * Audio buses: real-time audio routing between synths
   * Memory: bufLength * numChannels * 4 bytes (128 * 128 * 4 = 64KB)
   * Default: 128 (SuperSonic default, SC uses 1024)
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
   * Default: 4096 (SuperSonic default, SC uses 16384)
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
   * Default: false (SuperSonic is always externally clocked by AudioWorklet)
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
   * Default: 0 (SuperSonic loads synthdefs via network)
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
   * 0 = quiet, 1 = errors, 2 = warnings, 3 = info, 4 = debug
   * Default: 0
   */
  verbosity: 0,
};

export { defaultWorldOptions };
export default defaultWorldOptions;

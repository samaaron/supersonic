/*
    SuperSonic Configuration

    This file contains both build-time and runtime configuration for SuperSonic.

    Build-time options (memory layout):
      - Require rebuild when changed (./build.sh)
      - Define SharedArrayBuffer memory layout
      - Must stay synchronized with build.sh -sINITIAL_MEMORY flag

    Runtime options (worldOptions):
      - Can be overridden via SuperSonic constructor without rebuild
      - Passed to scsynth World_New()
      - Must fit within build-time memory allocations
*/

/**
 * Memory Layout Configuration (Build-time)
 *
 * SharedArrayBuffer is divided into three regions:
 *   0-32MB:     WASM heap (scsynth C++ allocations via malloc/AllocPool)
 *   32-64MB:    Ring buffers (OSC communication, ~64KB actual usage)
 *   64-192MB:   Buffer pool (audio sample storage via @thi.ng/malloc)
 *
 * IMPORTANT: Changing these values requires:
 *   1. Update this file
 *   2. Update build.sh -sINITIAL_MEMORY to match totalMemory
 *   3. Rebuild: ./build.sh
 *   4. Verify worldOptions fit within new wasmHeapSize
 */
const memory = {
    /**
     * Total WebAssembly memory in pages (1 page = 64KB)
     * Current: 3072 pages = 192MB
     * Must match build.sh -sINITIAL_MEMORY / 65536
     */
    totalPages: 3072,

    /**
     * WASM heap size (implicit, first section of memory)
     * Not directly configurable here - defined by bufferPoolOffset
     * Current: 0-32MB (32 * 1024 * 1024 = 33554432 bytes)
     */
    // wasmHeapSize is implicitly: bufferPoolOffset - ringBufferReserved

    /**
     * Ring buffer reserved space (between WASM heap and buffer pool)
     * Actual usage is ~64KB but reserve 32MB for alignment/future growth
     * Current: 32MB reserved (ring buffers start where WASM heap ends)
     */
    ringBufferReserved: 32 * 1024 * 1024,  // 32MB reserved

    /**
     * Buffer pool byte offset from start of SharedArrayBuffer
     * Audio samples are allocated from this pool using @thi.ng/malloc
     * Must be after WASM heap + ring buffer area
     * Current: 64MB offset = after 32MB heap + 32MB ring reserve
     */
    bufferPoolOffset: 64 * 1024 * 1024,  // 67108864 bytes

    /**
     * Buffer pool size in bytes
     * Used for audio sample storage (loaded files + allocated buffers)
     * Current: 128MB (enough for ~40 seconds of stereo at 48kHz uncompressed)
     */
    bufferPoolSize: 128 * 1024 * 1024,  // 134217728 bytes

    /**
     * Total memory calculation (should equal totalPages * 65536)
     * wasmHeap (~32MB) + ringReserve (32MB) + bufferPool (128MB) = 192MB
     */
    get totalMemory() {
        return this.bufferPoolOffset + this.bufferPoolSize;
    },

    /**
     * Effective WASM heap size (derived)
     * This is the space available for scsynth C++ allocations
     */
    get wasmHeapSize() {
        return this.bufferPoolOffset - this.ringBufferReserved;
    }
};

/**
 * SuperCollider World Options (Runtime)
 *
 * These options are passed to World_New() in scsynth.
 * They control synthesis engine behavior and resource limits.
 *
 * Can be overridden via:
 *   new SuperSonic({ scsynthOptions: { numBuffers: 2048 } })
 *
 * Memory usage validation:
 *   - Total allocations must fit within wasmHeapSize (~32MB)
 *   - realTimeMemorySize is the largest single allocation
 *   - Buffers, nodes, and buses also consume heap memory
 */
const worldOptions = {
    /**
     * Maximum number of audio buffers (SndBuf slots)
     * Each buffer slot: ~48 bytes overhead (3 SndBuf structs)
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
     * WebAudio/AudioWorklet input
     * Default: 0 (no input in current SuperSonic implementation)
     */
    numInputBusChannels: 0,

    /**
     * Number of output bus channels (hardware audio output)
     * WebAudio/AudioWorklet output
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
     * MUST be 128 for AudioWorklet compatibility
     * This is the number of samples processed per audio callback
     * Default: 128 (WebAudio standard, cannot be changed)
     */
    bufLength: 128,

    /**
     * Real-time memory pool size in kilobytes
     * AllocPool for synthesis-time allocations (UGen memory, etc.)
     * This is the largest single allocation from WASM heap
     * Memory: realTimeMemorySize * 1024 bytes (16384 * 1024 = 16MB)
     * Default: 16384 KB (16MB, SuperSonic default, SC uses 8192 = 8MB)
     */
    realTimeMemorySize: 16384,

    /**
     * Number of random number generators
     * Each synth can have its own RNG for reproducible randomness
     * Default: 64 (matching SuperCollider default)
     */
    numRGens: 64,

    /**
     * Real-time mode flag
     * false = Non-real-time (NRT) mode, externally driven by AudioWorklet
     * true = Real-time mode (not used in WebAudio context)
     * Default: false (SuperSonic always uses NRT mode)
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
    verbosity: 0
};

/**
 * Complete SuperSonic configuration
 */
export const ScsynthConfig = {
    memory,
    worldOptions
};

export default ScsynthConfig;

/*
    SuperSonic Runtime Configuration

    This file contains runtime configuration for SuperSonic that can be
    overridden via the SuperSonic constructor without requiring a rebuild.

    Runtime options (worldOptions):
      - Can be overridden via SuperSonic constructor without rebuild
      - Passed to scsynth World_New()
      - Must fit within build-time memory allocations (see js/memory_layout.js)

    Build-time configuration (memory layout):
      - See js/memory_layout.js
      - Requires rebuild when changed (./build.sh)
*/

import { MemoryLayout } from './memory_layout.js';

/**
 * Memory Layout Configuration (Build-time)
 *
 * Imported from memory_layout.js
 * See that file for details and to modify memory layout.
 *
 * IMPORTANT: Changing memory layout requires:
 *   1. Edit js/memory_layout.js
 *   2. Rebuild: ./build.sh
 *   3. Verify worldOptions fit within new wasmHeapSize
 */
// Import compile-time memory layout (defined in memory_layout.js)

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
     *
     * FIXED VALUE - DO NOT CHANGE
     * This MUST be 128 for AudioWorklet compatibility (WebAudio API spec).
     *
     * Unlike SuperCollider (where bufLength can be 32, 64, 128, etc.),
     * SuperSonic is locked to 128 because AudioWorklet has a fixed quantum size.
     *
     * This value is kept in the config for:
     * 1. Documentation (shows what value SuperSonic uses)
     * 2. Passing to C++ code (required by WorldOptions)
     * 3. Validation (catches accidental changes)
     *
     * If you provide this in your config, it MUST be 128 or initialization will fail.
     * It's recommended to omit this field entirely and let the default be used.
     *
     * Default: 128 (fixed, cannot be changed)
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
 *
 * Combines:
 *   - memory: Build-time layout (from memory_layout.js)
 *   - worldOptions: Runtime defaults (can be overridden)
 */
export const ScsynthConfig = {
    memory: MemoryLayout,
    worldOptions
};

export default ScsynthConfig;

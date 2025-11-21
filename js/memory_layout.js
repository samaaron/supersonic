/*
    SuperSonic Memory Layout (COMPILE-TIME Configuration)

    This file defines the WebAssembly memory layout that is fixed at build time.

    IMPORTANT: Changing these values requires a rebuild:
        1. Edit this file
        2. Run: ./build.sh
        3. Restart your application

    The memory layout cannot be changed at runtime - it's baked into the WASM binary
    and the SharedArrayBuffer created at initialisation.

    Memory Layout:
      0-16MB:   WASM Heap (scsynth C++ allocations)
      16-17MB:  Ring Buffers (OSC communication, 1MB)
      17-80MB:  Buffer Pool (audio sample storage)

    Total: 80MB (1280 WebAssembly pages x 64KB)
*/

/**
 * Memory Layout Configuration
 *
 * Defines SharedArrayBuffer structure and WebAssembly memory allocation.
 * These values are read by build.sh to set emscripten's -sINITIAL_MEMORY flag.
 */
export const MemoryLayout = {
    /**
     * Total WebAssembly memory in pages (1 page = 64KB)
     * Current: 1280 pages = 80MB
     *
     * This value is used by build.sh to set -sINITIAL_MEMORY
     * Must match: totalPages * 65536 = bufferPoolOffset + bufferPoolSize
     */
    totalPages: 1280,

    /**
     * WASM heap size (implicit, first section of memory)
     * Not directly configurable here - defined by bufferPoolOffset - ringBufferReserved
     * Current: 0-16MB (16 * 1024 * 1024 = 16777216 bytes)
     */
    // wasmHeapSize is implicitly: bufferPoolOffset - ringBufferReserved

    /**
     * Ring buffer reserved space (between WASM heap and buffer pool)
     * Actual ring buffer usage: IN: 768KB, OUT: 128KB, DEBUG: 64KB = 960KB
     * Plus control structures: CONTROL_SIZE (40B) + METRICS_SIZE (48B) + NTP_START_TIME_SIZE (8B) ≈ 96B
     * Total actual usage: ~960KB
     * Reserved: 1MB (provides ~64KB headroom for alignment and future expansion)
     * Current: 1MB reserved (starts where WASM heap ends at 16MB)
     */
    ringBufferReserved: 1 * 1024 * 1024,  // 1MB reserved

    /**
     * Buffer pool byte offset from start of SharedArrayBuffer
     * Audio samples are allocated from this pool using @thi.ng/malloc
     * Must be after WASM heap + ring buffer area
     * Current: 17MB offset = after 16MB heap + 1MB ring buffers
     */
    bufferPoolOffset: 17 * 1024 * 1024,  // 17825792 bytes

    /**
     * Buffer pool size in bytes
     * Used for audio sample storage (loaded files + allocated buffers)
     * Current: 63MB (enough for ~3.5 minutes of stereo at 48kHz uncompressed)
     */
    bufferPoolSize: 63 * 1024 * 1024,  // 66060288 bytes

    /**
     * Total memory calculation (should equal totalPages * 65536)
     * wasmHeap (16MB) + ringReserve (1MB) + bufferPool (63MB) = 80MB
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

export default MemoryLayout;

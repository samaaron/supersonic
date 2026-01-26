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
     * Plus control structures: CONTROL_SIZE (48B) + METRICS_SIZE (184B) + timing (16B) = 248B
     * Plus node tree: ~57KB
     * Plus audio capture buffer (for testing): 3sec at 48kHz stereo = ~1.1MB
     * Total actual usage: ~2.2MB
     * Reserved: 3MB (provides headroom for future expansion)
     * Current: 3MB reserved (starts where WASM heap ends at 16MB)
     */
    ringBufferReserved: 3 * 1024 * 1024,  // 3MB reserved

    /**
     * Buffer pool byte offset from start of SharedArrayBuffer
     * Audio samples are allocated from this pool using @thi.ng/malloc
     * Must be after WASM heap + ring buffer area
     * Current: 19MB offset = after 16MB heap + 3MB ring buffers
     */
    bufferPoolOffset: 19 * 1024 * 1024,  // 19922944 bytes

    /**
     * Buffer pool size in bytes
     * Used for audio sample storage (loaded files + allocated buffers)
     * Current: 61MB (enough for ~3.4 minutes of stereo at 48kHz uncompressed)
     */
    bufferPoolSize: 61 * 1024 * 1024,  // 63963136 bytes

    /**
     * Total memory calculation (should equal totalPages * 65536)
     * wasmHeap (16MB) + ringReserve (3MB) + bufferPool (61MB) = 80MB
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

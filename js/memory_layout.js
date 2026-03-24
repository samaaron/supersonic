/*
    SuperSonic Memory Layout

    Defines the WASM memory regions. The initial committed memory is small
    (heap + ring buffers + default buffer pool). The buffer pool grows on
    demand up to maxBufferPoolSize by extending WASM memory.

    Both bufferPoolSize and maxBufferPoolSize can be overridden at runtime
    via SuperSonic constructor options:
      memory: { bufferPoolSize: 32 * 1024 * 1024 }  // 32MB initial pool
      maxBufferMemory: 128 * 1024 * 1024             // 128MB ceiling

    Memory Layout:
      0-16MB:   WASM Heap (scsynth C++ allocations)
      16-19MB:  Ring Buffers (OSC, metrics, node tree, audio capture)
      19MB+:    Buffer Pool (audio samples, grows on demand)
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
     * Current: 368 pages = 23MB (19MB heap/ring + 4MB default pool)
     *
     * This value is used by build.sh to set -sINITIAL_MEMORY.
     * Must match: totalPages * 65536 = bufferPoolOffset + bufferPoolSize.
     * Can be overridden at runtime — buffer pool grows on demand.
     */
    totalPages: 368,

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
     * Buffer pool size in bytes (initial committed allocation)
     * Audio samples are allocated from this pool. When exhausted, the pool
     * grows automatically up to maxBufferPoolSize.
     * Default: 4MB. Override at runtime: memory: { bufferPoolSize: N }
     */
    bufferPoolSize: 4 * 1024 * 1024,  // 4194304 bytes

    /**
     * Maximum buffer pool size in bytes (hard ceiling for growth)
     * The pool starts at bufferPoolSize and can grow on demand up to this limit.
     * WASM memory is reserved (virtual address space) up to bufferPoolOffset + maxBufferPoolSize
     * but only committed as needed.
     * Current: 256MB max (overridable at runtime via maxBufferMemory option,
     * but cannot exceed this build-time cap)
     */
    maxBufferPoolSize: 256 * 1024 * 1024,  // 268435456 bytes

    /**
     * Total memory calculation (should equal totalPages * 65536)
     * wasmHeap (16MB) + ringReserve (3MB) + bufferPool (4MB) = 23MB
     */
    get totalMemory() {
        return this.bufferPoolOffset + this.bufferPoolSize;
    },

    /**
     * Maximum total memory (bufferPoolOffset + maxBufferPoolSize)
     * Used by build.sh for -sMAXIMUM_MEMORY flag.
     * Must be a multiple of 65536 (WebAssembly page size).
     */
    get maxTotalMemory() {
        return this.bufferPoolOffset + this.maxBufferPoolSize;
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

/*
    SuperSonic Memory Layout

    Defines the WASM memory regions. The initial committed memory is small
    (heap + ring buffers + RT pool + default buffer pool). The buffer pool
    grows on demand up to maxBufferPoolSize by extending WASM memory.

    Both bufferPoolSize and maxBufferPoolSize can be overridden at runtime
    via SuperSonic constructor options:
      memory: { bufferPoolSize: 32 * 1024 * 1024 }  // 32MB initial pool
      maxBufferMemory: 128 * 1024 * 1024             // 128MB ceiling

    Memory Layout:
      0-8MB:    WASM Heap (emscripten malloc, static data, stack)
      8-11MB:   Ring Buffers (OSC, metrics, node tree, audio capture)
      11-139MB: RT Pool (scsynth real-time allocator)
      139MB+:   Buffer Pool (audio samples, grows on demand)
*/

/**
 * Memory Layout Configuration
 *
 * Defines SharedArrayBuffer structure and WebAssembly memory allocation.
 * These values are read by build.sh to set emscripten's -sINITIAL_MEMORY flag.
 */
export const MemoryLayout = {
    /**
     * WASM heap size in bytes
     * Space for emscripten malloc, static data, and stack.
     * Reduced from implicit 16MB since the RT pool is now a separate region.
     * Current: 8MB
     */
    wasmHeapSize: 8 * 1024 * 1024,  // 8MB

    /**
     * Ring buffer reserved space (between WASM heap and RT pool)
     * Actual ring buffer usage: IN: 768KB, OUT: 128KB, DEBUG: 64KB = 960KB
     * Plus control structures: CONTROL_SIZE (48B) + METRICS_SIZE (184B) + timing (16B) = 248B
     * Plus node tree: ~57KB
     * Plus shm_audio_buffer slot array:
     *   MAX_SHM_AUDIO_BUFFERS * (32B header + SHM_AUDIO_FRAMES * SHM_AUDIO_CHANNELS * 4B)
     *   = 4 * (32 + 48000 * 2 * 4) ≈ 1.5MB at the production 1-second ring.
     * Total ~2.6MB. Overflow into the RT pool silently breaks OSC.
     */
    ringBufferReserved: 3 * 1024 * 1024,  // 3MB reserved

    /**
     * RT pool size in bytes (dedicated region for scsynth's real-time allocator).
     * Pre-allocated in the SharedArrayBuffer so AllocPool doesn't use malloc
     * (which would grow the WASM heap into the buffer pool).
     *
     * This default is overridden at runtime by scsynthOptions.realTimeMemorySize.
     * See #buildMemoryConfig in supersonic.js.
     * Default: 32MB (sufficient for heavy live coding).
     */
    rtPoolSize: 32 * 1024 * 1024,  // 32MB

    /**
     * Buffer pool size in bytes (initial committed allocation)
     * Audio samples are allocated from this pool. When exhausted, the pool
     * grows automatically up to maxBufferPoolSize.
     * Default: 4MB. Override at runtime: memory: { bufferPoolSize: N }
     */
    bufferPoolSize: 4 * 1024 * 1024,  // 4MB

    /**
     * Maximum buffer pool size in bytes (hard ceiling for growth)
     * The pool starts at bufferPoolSize and can grow on demand up to this limit.
     * WASM memory is reserved (virtual address space) up to bufferPoolOffset + maxBufferPoolSize
     * but only committed as needed.
     * Current: 256MB max (overridable at runtime via maxBufferMemory option,
     * but cannot exceed this build-time cap)
     */
    maxBufferPoolSize: 768 * 1024 * 1024,  // 768MB

    /**
     * RT pool byte offset from start of SharedArrayBuffer (derived)
     * rtPoolOffset = wasmHeapSize + ringBufferReserved
     */
    get rtPoolOffset() {
        return this.wasmHeapSize + this.ringBufferReserved;
    },

    /**
     * Buffer pool byte offset from start of SharedArrayBuffer (derived)
     * bufferPoolOffset = rtPoolOffset + rtPoolSize
     */
    get bufferPoolOffset() {
        return this.rtPoolOffset + this.rtPoolSize;
    },

    /**
     * Total memory calculation (derived)
     * bufferPoolOffset + bufferPoolSize
     */
    get totalMemory() {
        return this.bufferPoolOffset + this.bufferPoolSize;
    },

    /**
     * Maximum total WASM memory (derived, used by build.sh for -sMAXIMUM_MEMORY).
     * Computed as: heap + ring + rtPool + maxBufferPoolSize.
     * The RT pool doesn't grow, but the buffer pool does — up to maxBufferPoolSize.
     * At runtime, #buildMemoryConfig may increase rtPoolSize based on
     * scsynthOptions.realTimeMemorySize, which calls memory.grow() as needed.
     */
    get maxTotalMemory() {
        return this.bufferPoolOffset + this.maxBufferPoolSize;
    },

    /**
     * Total WebAssembly memory in pages (derived, 1 page = 64KB)
     * Used by build.sh to set -sINITIAL_MEMORY.
     */
    get totalPages() {
        return Math.ceil(this.totalMemory / 65536);
    },
};

export default MemoryLayout;

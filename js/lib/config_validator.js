/*
    SuperSonic Configuration Validator

    Validates worldOptions and memory layout to catch errors early
    with helpful error messages.

    IMPORTANT: Validation is based on actual SuperCollider source code
    (version 3.14.1-rc2, see include/server/SC_WorldOptions.h and
    server/scsynth/SC_World.cpp).

    SuperCollider Default Values (SC_WorldOptions.h):
    - mNumBuffers: 1024
    - mMaxNodes: 1024
    - mMaxGraphDefs: 1024
    - mMaxWireBufs: 64
    - mNumAudioBusChannels: 1024
    - mNumInputBusChannels: 8
    - mNumOutputBusChannels: 8
    - mNumControlBusChannels: 16384
    - mBufLength: 64 (but we use 128 for AudioWorklet quantum)
    - mRealTimeMemorySize: 8192 KB (8MB)
    - mNumRGens: 64
    - mPreferredSampleRate: 0 (auto)
    - mVerbosity: 0

    Note: SuperSonic defaults may differ from SC defaults to optimize
    for browser/WASM constraints. See js/scsynth_options.js.
*/

/**
 * Validates SuperSonic configuration options
 */
export class ConfigValidator {
    /**
     * Validate worldOptions for common errors
     * @param {Object} worldOptions - WorldOptions to validate
     * @param {Object} memoryConfig - Memory layout configuration
     * @throws {Error} If validation fails with helpful message
     */
    static validateWorldOptions(worldOptions, memoryConfig) {
        if (!worldOptions || typeof worldOptions !== 'object') {
            throw new Error('worldOptions must be an object');
        }

        // Validate numeric fields (must be finite numbers >= 0)
        const numericFields = [
            'numBuffers',
            'maxNodes',
            'maxGraphDefs',
            'maxWireBufs',
            'numAudioBusChannels',
            'numInputBusChannels',
            'numOutputBusChannels',
            'numControlBusChannels',
            'bufLength',
            'realTimeMemorySize',
            'numRGens',
            'verbosity',
            'preferredSampleRate',
            'loadGraphDefs'
        ];

        // Fields that can be 0 (others must be > 0)
        const canBeZero = new Set([
            'numInputBusChannels',   // 0 = no audio inputs (valid for synthesis-only)
            'numOutputBusChannels',  // 0 = no audio outputs (unusual but valid)
            'verbosity',             // 0 = no verbose output
            'preferredSampleRate',   // 0 = auto-detect sample rate
            'loadGraphDefs'          // 0 = don't auto-load SynthDefs from disk
        ]);

        for (const field of numericFields) {
            if (field in worldOptions) {
                const value = worldOptions[field];

                if (!Number.isFinite(value)) {
                    throw new Error(
                        `worldOptions.${field} must be a finite number, got ${typeof value}: ${value}`
                    );
                }

                const minValue = canBeZero.has(field) ? 0 : 1;
                if (value < minValue) {
                    throw new Error(
                        `worldOptions.${field} must be ${minValue > 0 ? 'positive' : 'non-negative'}, got ${value}`
                    );
                }

                // Check for reasonable maximums (prevent accidental huge values)
                const maxValue = this.#getMaxValueFor(field);
                if (value > maxValue) {
                    throw new Error(
                        `worldOptions.${field} exceeds reasonable maximum.\n` +
                        `  Got: ${value}\n` +
                        `  Max: ${maxValue}\n` +
                        `This may indicate a configuration error.`
                    );
                }
            }
        }

        // Validate bufLength (must be 128 - AudioWorklet quantum)
        if ('bufLength' in worldOptions && worldOptions.bufLength !== 128) {
            throw new Error(
                `worldOptions.bufLength must be 128 (AudioWorklet quantum size).\n` +
                `  Got: ${worldOptions.bufLength}\n` +
                `This value cannot be changed as it's defined by the Web Audio API.\n` +
                `Remove 'bufLength' from your configuration.`
            );
        }

        // Validate boolean fields
        const booleanFields = ['realTime', 'memoryLocking'];
        for (const field of booleanFields) {
            if (field in worldOptions && typeof worldOptions[field] !== 'boolean') {
                throw new Error(
                    `worldOptions.${field} must be a boolean, got ${typeof worldOptions[field]}`
                );
            }
        }

        // Validate sampleRate if present
        if ('preferredSampleRate' in worldOptions) {
            const rate = worldOptions.preferredSampleRate;
            if (rate !== 0 && !Number.isFinite(rate)) {
                throw new Error(
                    `worldOptions.preferredSampleRate must be 0 (auto) or a finite number, got ${rate}`
                );
            }
            if (rate !== 0 && (rate < 8000 || rate > 192000)) {
                throw new Error(
                    `worldOptions.preferredSampleRate out of range: ${rate}\n` +
                    `Valid range: 8000-192000 Hz, or 0 for auto`
                );
            }
        }

        // Validate compatible combinations
        this.#validateCombinations(worldOptions);

        // Check if options fit within available heap memory
        if (memoryConfig) {
            const heapSize = memoryConfig.wasmHeapSize;
            if (heapSize) {
                MemoryEstimator.validateHeapFits(worldOptions, heapSize);
            }
        }
    }

    /**
     * Validate memory layout for coherence
     * @param {Object} memory - Memory configuration
     * @throws {Error} If layout is invalid
     */
    static validateMemoryLayout(memory) {
        if (!memory || typeof memory !== 'object') {
            throw new Error('memory configuration must be an object');
        }

        const { totalPages, bufferPoolOffset, bufferPoolSize, ringBufferReserved } = memory;

        // Validate required fields exist
        if (!Number.isFinite(totalPages) || totalPages <= 0) {
            throw new Error(
                `memory.totalPages must be a positive number, got ${totalPages}`
            );
        }

        if (!Number.isFinite(bufferPoolOffset) || bufferPoolOffset <= 0) {
            throw new Error(
                `memory.bufferPoolOffset must be a positive number, got ${bufferPoolOffset}`
            );
        }

        if (!Number.isFinite(bufferPoolSize) || bufferPoolSize <= 0) {
            throw new Error(
                `memory.bufferPoolSize must be a positive number, got ${bufferPoolSize}`
            );
        }

        // Validate total memory calculation
        const totalMemory = totalPages * 65536;
        const expectedTotal = bufferPoolOffset + bufferPoolSize;

        if (totalMemory !== expectedTotal) {
            throw new Error(
                `Memory layout mismatch:\n` +
                `  totalPages * 65536 = ${totalMemory} bytes (${(totalMemory / 1024 / 1024).toFixed(0)}MB)\n` +
                `  bufferPoolOffset + bufferPoolSize = ${expectedTotal} bytes (${(expectedTotal / 1024 / 1024).toFixed(0)}MB)\n` +
                `These must be equal. Check your memory configuration.`
            );
        }

        // Validate buffer pool doesn't overlap with earlier regions
        const minBufferPoolOffset = ringBufferReserved || (32 * 1024 * 1024);
        if (bufferPoolOffset < minBufferPoolOffset) {
            throw new Error(
                `memory.bufferPoolOffset (${bufferPoolOffset}) is too small.\n` +
                `  Must be at least: ${minBufferPoolOffset} bytes (${(minBufferPoolOffset / 1024 / 1024).toFixed(0)}MB)\n` +
                `This is required to avoid collision with WASM heap and ring buffers.`
            );
        }

        // Warn if total memory is very large (browser memory pressure)
        if (totalMemory > 512 * 1024 * 1024) {
            console.warn(
                `[SuperSonic] Warning: Total memory is ${(totalMemory / 1024 / 1024).toFixed(0)}MB.\n` +
                `Large memory allocations may cause browser performance issues on some devices.`
            );
        }

        // Validate WASM heap size is reasonable
        const wasmHeapSize = memory.wasmHeapSize;
        if (wasmHeapSize && wasmHeapSize < 16 * 1024 * 1024) {
            console.warn(
                `[SuperSonic] Warning: WASM heap is only ${(wasmHeapSize / 1024 / 1024).toFixed(0)}MB.\n` +
                `This may be too small for typical usage. Consider increasing bufferPoolOffset.`
            );
        }
    }

    /**
     * Get maximum reasonable value for a field
     * Based on SuperCollider defaults and reasonable WASM/browser limits
     * @private
     */
    static #getMaxValueFor(field) {
        const limits = {
            // SC default: 1024, All uint32 in SC_WorldOptions.h
            // Limit to prevent excessive memory allocation
            numBuffers: 65535,

            maxNodes: 65535,             // SC default: 1024
            maxGraphDefs: 65535,         // SC default: 1024
            maxWireBufs: 2048,           // SC default: 64

            // SC default: 1024 (NOT 128!)
            // But we use lower for browser context
            numAudioBusChannels: 4096,

            numInputBusChannels: 256,    // SC default: 8
            numOutputBusChannels: 256,   // SC default: 8

            // SC default: 16384 (NOT 4096!)
            numControlBusChannels: 65535,

            bufLength: 128,              // Fixed for AudioWorklet (SC default: 64)

            // SC default: 8192 KB (8MB)
            // Limit to 1GB for browser safety
            realTimeMemorySize: 1048576, // KB (1GB max)

            numRGens: 1024,              // SC default: 64
            verbosity: 10,
            preferredSampleRate: 192000, // Maximum sample rate
            loadGraphDefs: 1             // SC default: 1, 0=off 1=on
        };

        return limits[field] || Number.MAX_SAFE_INTEGER;
    }

    /**
     * Validate compatible combinations of options
     * @private
     */
    static #validateCombinations(worldOptions) {
        // Ensure realTimeMemorySize isn't absurdly small
        if (worldOptions.realTimeMemorySize && worldOptions.realTimeMemorySize < 1024) {
            console.warn(
                `[SuperSonic] Warning: realTimeMemorySize is only ${worldOptions.realTimeMemorySize}KB (${(worldOptions.realTimeMemorySize / 1024).toFixed(1)}MB).\n` +
                `This is very small and may cause allocation failures. Recommended minimum: 4096 KB (4MB).`
            );
        }

        // Warn if output channels is unusual
        if (worldOptions.numOutputBusChannels && worldOptions.numOutputBusChannels > 8) {
            console.warn(
                `[SuperSonic] Warning: numOutputBusChannels is ${worldOptions.numOutputBusChannels}.\n` +
                `Most audio hardware supports 2-8 channels. Verify your AudioContext supports this.`
            );
        }
    }
}

/**
 * Estimates WASM heap memory usage
 */
export class MemoryEstimator {
    /**
     * Estimate WASM heap usage for given WorldOptions
     * Returns breakdown of memory usage by component
     *
     * Based on actual SuperCollider source code allocations (SC_World.cpp, SC_Graph.cpp)
     *
     * @param {Object} worldOptions - Configuration to analyze
     * @returns {Object} { total, breakdown: {...} }
     */
    static estimateHeapUsage(worldOptions) {
        const breakdown = {
            // AllocPool for real-time synthesis (SC_World.cpp:328)
            // new AllocPool(..., inOptions->mRealTimeMemorySize * 1024, 0)
            realTimeMemory: worldOptions.realTimeMemorySize * 1024, // KB → bytes

            // SndBuf arrays (SC_World.cpp:383-385)
            // mSndBufs: sizeof(SndBuf) = 48 bytes (SC_SndBuf.h:152-168, WASM32 with 32-bit pointers)
            // mSndBufsNonRealTimeMirror: sizeof(SndBuf) = 48 bytes
            // mSndBufUpdates: sizeof(SndBufUpdates) = 8 bytes (2 × int32)
            // Total: (48 + 48 + 8) * mNumBuffers = 104 bytes per buffer
            sndBufs: worldOptions.numBuffers * 104,

            // Audio buses (SC_World.cpp:376-377)
            // mAudioBus = zalloc(mBufLength * mNumAudioBusChannels, sizeof(float))
            audioBuses: worldOptions.numAudioBusChannels * worldOptions.bufLength * 4,

            // Audio bus touched flags (SC_World.cpp:379)
            // mAudioBusTouched = zalloc(mNumAudioBusChannels, sizeof(int32))
            audioBusTouched: worldOptions.numAudioBusChannels * 4,

            // Control buses (SC_World.cpp:370)
            // mControlBus = zalloc(mNumControlBusChannels, sizeof(float))
            controlBuses: worldOptions.numControlBusChannels * 4,

            // Control bus touched flags (SC_World.cpp:380)
            // mControlBusTouched = zalloc(mNumControlBusChannels, sizeof(int32))
            controlBusTouched: worldOptions.numControlBusChannels * 4,

            // Wire buffers (SC_World.cpp:917)
            // mWireBufSpace = malloc_alig(mMaxWireBufs * mBufLength * sizeof(float))
            wireBufs: worldOptions.maxWireBufs * worldOptions.bufLength * 4,

            // Random number generators (SC_World.cpp:398)
            // mRGen = new RGen[mNumRGens]
            // sizeof(RGen) = 12 bytes (3 × uint32, SC_RGen.h:57-85)
            rgens: worldOptions.numRGens * 12,

            // Node hash table (SC_World.cpp:334)
            // IntHashTable<Node, AllocPool> with capacity mMaxNodes
            // Conservative estimate: ~64 bytes per slot for hash table overhead
            nodes: worldOptions.maxNodes * 64,

            // GraphDef hash table (SC_World.cpp:333)
            // HashTable<GraphDef, Malloc> with capacity mMaxGraphDefs
            // Conservative estimate: ~128 bytes per slot (varies per GraphDef)
            graphDefs: worldOptions.maxGraphDefs * 128,

            // World struct, HiddenWorld, and other fixed overhead
            // Includes: World struct, HiddenWorld struct, locks, semaphores, etc.
            overhead: 2 * 1024 * 1024 // 2MB
        };

        const total = Object.values(breakdown).reduce((sum, val) => sum + val, 0);

        return { total, breakdown };
    }

    /**
     * Check if worldOptions fit within available heap
     * @param {Object} worldOptions
     * @param {number} heapSize - Available heap in bytes
     * @throws {Error} If estimated usage exceeds heap
     */
    static validateHeapFits(worldOptions, heapSize) {
        const { total, breakdown } = this.estimateHeapUsage(worldOptions);

        if (total > heapSize) {
            const totalMB = (total / (1024 * 1024)).toFixed(2);
            const heapMB = (heapSize / (1024 * 1024)).toFixed(2);

            // Find largest allocations to suggest reductions
            const sorted = Object.entries(breakdown)
                .sort(([, a], [, b]) => b - a)
                .slice(0, 3)
                .map(([key, bytes]) => `  ${key}: ${(bytes / (1024 * 1024)).toFixed(2)}MB`)
                .join('\n');

            throw new Error(
                `WorldOptions estimated to use ${totalMB}MB but WASM heap is only ${heapMB}MB.\n\n` +
                `Largest allocations:\n${sorted}\n\n` +
                `To fix this error:\n` +
                `1. Reduce realTimeMemorySize (currently ${worldOptions.realTimeMemorySize}KB)\n` +
                `2. Reduce numBuffers (currently ${worldOptions.numBuffers})\n` +
                `3. Reduce maxNodes (currently ${worldOptions.maxNodes})\n` +
                `4. Or: Increase WASM heap size (requires rebuild - see CONFIGURATION_PHASES.md Phase 4)`
            );
        }

        // Warn if usage is > 80% of heap (tight fit)
        const usagePercent = (total / heapSize) * 100;
        if (usagePercent > 80) {
            console.warn(
                `[SuperSonic] Warning: Estimated heap usage is ${usagePercent.toFixed(0)}% of available space.\n` +
                `  Used: ${(total / 1024 / 1024).toFixed(1)}MB\n` +
                `  Available: ${(heapSize / 1024 / 1024).toFixed(1)}MB\n` +
                `Consider reducing options or increasing heap size for safety margin.`
            );
        }
    }
}

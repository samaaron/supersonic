/**
 * BufferManager - Handles loading and managing audio buffers
 *
 * Responsibilities:
 * - Load audio files and decode with WebAudio
 * - Allocate memory in buffer pool
 * - Copy decoded audio to SharedArrayBuffer
 * - Manage concurrent buffer operations
 */

import { MemPool } from '@thi.ng/malloc';

const BUFFER_POOL_ALIGNMENT = 8;  // Float64 alignment

export class BufferManager {
    // Private configuration
    #sampleBaseURL;
    #assetLoader;

    // Private implementation
    #audioContext;
    #sharedBuffer;
    #bufferPool;
    #bufferPoolSize;
    #allocatedBuffers;
    #pendingBufferOps;
    #bufferLocks;

    constructor(options) {
        const {
            audioContext,
            sharedBuffer,
            bufferPoolConfig,
            sampleBaseURL,
            maxBuffers = 1024,
            assetLoader = null
        } = options;

        // Validate required dependencies
        if (!audioContext) {
            throw new Error('BufferManager requires audioContext');
        }
        if (!sharedBuffer || !(sharedBuffer instanceof SharedArrayBuffer)) {
            throw new Error('BufferManager requires sharedBuffer (SharedArrayBuffer)');
        }
        if (!bufferPoolConfig || typeof bufferPoolConfig !== 'object') {
            throw new Error('BufferManager requires bufferPoolConfig (object with start, size, align)');
        }
        if (!Number.isFinite(bufferPoolConfig.start) || bufferPoolConfig.start < 0) {
            throw new Error('bufferPoolConfig.start must be a non-negative number');
        }
        if (!Number.isFinite(bufferPoolConfig.size) || bufferPoolConfig.size <= 0) {
            throw new Error('bufferPoolConfig.size must be a positive number');
        }

        // Validate optional dependencies
        if (!Number.isInteger(maxBuffers) || maxBuffers <= 0) {
            throw new Error('maxBuffers must be a positive integer');
        }

        this.#audioContext = audioContext;
        this.#sharedBuffer = sharedBuffer;
        this.#sampleBaseURL = sampleBaseURL;
        this.#assetLoader = assetLoader;

        // Create and own buffer pool
        this.#bufferPool = new MemPool({
            buf: sharedBuffer,
            start: bufferPoolConfig.start,
            size: bufferPoolConfig.size,
            align: BUFFER_POOL_ALIGNMENT
        });
        this.#bufferPoolSize = bufferPoolConfig.size;

        // Create and own buffer state
        this.#allocatedBuffers = new Map();  // bufnum -> { ptr, size, pendingToken, ... }
        this.#pendingBufferOps = new Map();  // UUID -> { resolve, reject, timeout }
        this.#bufferLocks = new Map();       // bufnum -> promise chain tail

        // Guard samples prevent interpolation artifacts at buffer boundaries.
        // SuperCollider uses 3 samples before and 1 sample after for cubic interpolation.
        this.GUARD_BEFORE = 3;
        this.GUARD_AFTER = 1;

        // Maximum buffer count (from config)
        this.MAX_BUFFERS = maxBuffers;

        const poolSizeMB = (bufferPoolConfig.size / (1024 * 1024)).toFixed(0);
        const poolOffsetMB = (bufferPoolConfig.start / (1024 * 1024)).toFixed(0);
        if (__DEV__) console.log(`[BufferManager] Initialized: ${poolSizeMB}MB pool at offset ${poolOffsetMB}MB`);
    }

    #resolveAudioPath(scPath) {
        // Validate path to prevent directory traversal attacks
        if (typeof scPath !== 'string' || scPath.length === 0) {
            throw new Error(`Invalid audio path: must be a non-empty string`);
        }

        // Reject directory traversal patterns
        if (scPath.includes('..')) {
            throw new Error(`Invalid audio path: path cannot contain '..' (got: ${scPath})`);
        }

        // Reject URL-encoded traversal attempts
        if (scPath.includes('%2e') || scPath.includes('%2E')) {
            throw new Error(`Invalid audio path: path cannot contain URL-encoded characters (got: ${scPath})`);
        }

        // Reject backslash (Windows path separator) to ensure consistent behaviour
        if (scPath.includes('\\')) {
            throw new Error(`Invalid audio path: use forward slashes only (got: ${scPath})`);
        }

        // If path looks like a URL or already-resolved path, use it directly
        // - URLs (http://, https://, etc.)
        // - Absolute paths (/foo/bar)
        // - Explicit relative paths (./foo or ../foo - though .. is rejected above)
        if (scPath.includes('://') || scPath.startsWith('/') || scPath.startsWith('./')) {
            return scPath;
        }

        // Simple filenames need sampleBaseURL
        if (!this.#sampleBaseURL) {
            throw new Error(
                'sampleBaseURL not configured. Please set it in SuperSonic constructor options.\n' +
                'Example: new SuperSonic({ sampleBaseURL: "./dist/samples/" })\n' +
                'Or use CDN: new SuperSonic({ sampleBaseURL: "https://unpkg.com/supersonic-scsynth-samples@latest/samples/" })\n' +
                'Or install: npm install supersonic-scsynth-samples'
            );
        }

        // Prepend base URL for simple filenames
        return this.#sampleBaseURL + scPath;
    }

    #validateBufferNumber(bufnum) {
        if (!Number.isInteger(bufnum) || bufnum < 0 || bufnum >= this.MAX_BUFFERS) {
            throw new Error(`Invalid buffer number ${bufnum} (must be 0-${this.MAX_BUFFERS - 1})`);
        }
    }

    /**
     * Execute a buffer operation with proper locking, registration, and cleanup
     * @private
     * @param {number} bufnum - Buffer number
     * @param {number} timeoutMs - Operation timeout
     * @param {Function} operation - Async function that performs the actual buffer work
     *                                Should return {ptr, sizeBytes, ...extraProps}
     * @returns {Promise<Object>} Result object with ptr, uuid, allocationComplete, and extra props
     */
    async #executeBufferOperation(bufnum, timeoutMs, operation) {
        let allocatedPtr = null;
        let pendingToken = null;
        let allocationRegistered = false;

        const releaseLock = await this.#acquireBufferLock(bufnum);
        let lockReleased = false;

        try {
            await this.#awaitPendingReplacement(bufnum);

            // Execute the actual operation (loading file or allocating empty buffer)
            const { ptr, sizeBytes, numFrames, numChannels, sampleRate, ...extraProps } = await operation();
            allocatedPtr = ptr;

            // Register pending operation
            const { uuid, allocationComplete } = this.#registerPending(bufnum, timeoutMs);
            pendingToken = uuid;
            this.#recordAllocation(bufnum, allocatedPtr, sizeBytes, uuid, allocationComplete, {
                numFrames, numChannels, sampleRate
            });
            allocationRegistered = true;

            const managedCompletion = this.#attachFinalizer(bufnum, uuid, allocationComplete);
            releaseLock();
            lockReleased = true;

            return {
                ptr: allocatedPtr,
                uuid,
                allocationComplete: managedCompletion,
                numFrames,
                numChannels,
                sampleRate,
                ...extraProps
            };
        } catch (error) {
            if (allocationRegistered && pendingToken) {
                this.#finalizeReplacement(bufnum, pendingToken, false);
            } else if (allocatedPtr) {
                this.#bufferPool.free(allocatedPtr);
            }
            throw error;
        } finally {
            if (!lockReleased) {
                releaseLock();
            }
        }
    }

    async prepareFromBlob(params) {
        const {
            bufnum,
            blob,
            startFrame = 0,
            numFrames = 0,
            channels = null
        } = params;

        this.#validateBufferNumber(bufnum);

        if (!blob || !(blob instanceof ArrayBuffer || ArrayBuffer.isView(blob))) {
            throw new Error('/b_allocFile requires audio data as ArrayBuffer or typed array');
        }

        // Convert to ArrayBuffer if needed (for decodeAudioData)
        const arrayBuffer = blob instanceof ArrayBuffer
            ? blob
            : blob.buffer.slice(blob.byteOffset, blob.byteOffset + blob.byteLength);

        // Use 30s timeout for decode (no network, but decode can be slow for large files)
        return this.#executeBufferOperation(bufnum, 30000, async () => {
            const audioBuffer = await this.#audioContext.decodeAudioData(arrayBuffer);

            // Calculate frame range
            const start = Math.max(0, Math.floor(startFrame || 0));
            const availableFrames = audioBuffer.length - start;
            const framesRequested = numFrames && numFrames > 0
                ? Math.min(Math.floor(numFrames), availableFrames)
                : availableFrames;

            if (framesRequested <= 0) {
                throw new Error(`No audio frames available for buffer ${bufnum}`);
            }

            // Determine channel selection
            const selectedChannels = this.#normalizeChannels(channels, audioBuffer.numberOfChannels);
            const numChannels = selectedChannels.length;

            // Allocate memory with guard samples
            const totalSamples = (framesRequested * numChannels) +
                ((this.GUARD_BEFORE + this.GUARD_AFTER) * numChannels);

            const ptr = this.#malloc(totalSamples);
            const interleaved = new Float32Array(totalSamples);
            const dataOffset = this.GUARD_BEFORE * numChannels;

            // Copy audio data with interleaving
            for (let frame = 0; frame < framesRequested; frame++) {
                for (let ch = 0; ch < numChannels; ch++) {
                    const sourceChannel = selectedChannels[ch];
                    const channelData = audioBuffer.getChannelData(sourceChannel);
                    interleaved[dataOffset + (frame * numChannels) + ch] =
                        channelData[start + frame];
                }
            }

            this.#writeToSharedBuffer(ptr, interleaved);
            const sizeBytes = interleaved.length * 4;

            return {
                ptr,
                sizeBytes,
                numFrames: framesRequested,
                numChannels,
                sampleRate: audioBuffer.sampleRate
            };
        });
    }

    async prepareFromFile(params) {
        const {
            bufnum,
            path,
            startFrame = 0,
            numFrames = 0,
            channels = null
        } = params;

        this.#validateBufferNumber(bufnum);

        // Use 60s timeout for file operations (network fetch + decode can be slow)
        return this.#executeBufferOperation(bufnum, 60000, async () => {
            // Fetch and decode audio file
            const resolvedPath = this.#resolveAudioPath(path);
            const sampleName = path.split('/').pop();

            // Fetch using AssetLoader (handles retry, HEAD request, loading:start event)
            const arrayBuffer = await this.#assetLoader.fetch(resolvedPath, {
                type: 'sample',
                name: sampleName,
            });

            const audioBuffer = await this.#audioContext.decodeAudioData(arrayBuffer);

            // Calculate frame range
            const start = Math.max(0, Math.floor(startFrame || 0));
            const availableFrames = audioBuffer.length - start;
            const framesRequested = numFrames && numFrames > 0
                ? Math.min(Math.floor(numFrames), availableFrames)
                : availableFrames;

            if (framesRequested <= 0) {
                throw new Error(`No audio frames available for buffer ${bufnum} from ${path}`);
            }

            // Determine channel selection
            const selectedChannels = this.#normalizeChannels(channels, audioBuffer.numberOfChannels);
            const numChannels = selectedChannels.length;

            // Allocate memory with guard samples
            const totalSamples = (framesRequested * numChannels) +
                ((this.GUARD_BEFORE + this.GUARD_AFTER) * numChannels);

            const ptr = this.#malloc(totalSamples);
            const interleaved = new Float32Array(totalSamples);
            const dataOffset = this.GUARD_BEFORE * numChannels;

            // Copy audio data with interleaving
            for (let frame = 0; frame < framesRequested; frame++) {
                for (let ch = 0; ch < numChannels; ch++) {
                    const sourceChannel = selectedChannels[ch];
                    const channelData = audioBuffer.getChannelData(sourceChannel);
                    interleaved[dataOffset + (frame * numChannels) + ch] =
                        channelData[start + frame];
                }
            }

            this.#writeToSharedBuffer(ptr, interleaved);
            const sizeBytes = interleaved.length * 4;

            return {
                ptr,
                sizeBytes,
                numFrames: framesRequested,
                numChannels,
                sampleRate: audioBuffer.sampleRate
            };
        });
    }

    async prepareEmpty(params) {
        const {
            bufnum,
            numFrames,
            numChannels = 1,
            sampleRate = null
        } = params;

        this.#validateBufferNumber(bufnum);

        // Validate parameters
        if (!Number.isFinite(numFrames) || numFrames <= 0) {
            throw new Error(`/b_alloc requires a positive number of frames (got ${numFrames})`);
        }

        if (!Number.isFinite(numChannels) || numChannels <= 0) {
            throw new Error(`/b_alloc requires a positive channel count (got ${numChannels})`);
        }

        const roundedFrames = Math.floor(numFrames);
        const roundedChannels = Math.floor(numChannels);

        // Use 5s timeout for allocations (should be nearly instant)
        return this.#executeBufferOperation(bufnum, 5000, async () => {
            // Calculate total samples needed with guard samples
            const totalSamples = (roundedFrames * roundedChannels) +
                ((this.GUARD_BEFORE + this.GUARD_AFTER) * roundedChannels);

            // Allocate and zero-initialize buffer
            const ptr = this.#malloc(totalSamples);
            const interleaved = new Float32Array(totalSamples);
            this.#writeToSharedBuffer(ptr, interleaved);
            const sizeBytes = interleaved.length * 4;

            return {
                ptr,
                sizeBytes,
                numFrames: roundedFrames,
                numChannels: roundedChannels,
                sampleRate: sampleRate || this.#audioContext.sampleRate
            };
        });
    }

    #normalizeChannels(requestedChannels, fileChannels) {
        if (!requestedChannels || requestedChannels.length === 0) {
            return Array.from({ length: fileChannels }, (_, i) => i);
        }

        requestedChannels.forEach((channel) => {
            if (!Number.isInteger(channel) || channel < 0 || channel >= fileChannels) {
                throw new Error(`Channel ${channel} is out of range (file has ${fileChannels} channels)`);
            }
        });

        return requestedChannels;
    }

    #malloc(totalSamples) {
        const bytesNeeded = totalSamples * 4;
        const ptr = this.#bufferPool.malloc(bytesNeeded);

        if (ptr === 0) {
            const stats = this.#bufferPool.stats();
            const availableMB = ((stats.available || 0) / (1024 * 1024)).toFixed(2);
            const totalMB = ((stats.total || 0) / (1024 * 1024)).toFixed(2);
            const requestedMB = (bytesNeeded / (1024 * 1024)).toFixed(2);
            throw new Error(
                `Buffer pool allocation failed: requested ${requestedMB}MB, ` +
                `available ${availableMB}MB of ${totalMB}MB total`
            );
        }

        return ptr;
    }

    #writeToSharedBuffer(ptr, data) {
        const heap = new Float32Array(this.#sharedBuffer, ptr, data.length);
        heap.set(data);
    }

    #createPendingOperation(uuid, bufnum, timeoutMs) {
        return new Promise((resolve, reject) => {
            const timeout = setTimeout(() => {
                this.#pendingBufferOps.delete(uuid);
                reject(new Error(`Buffer ${bufnum} allocation timeout (${timeoutMs}ms)`));
            }, timeoutMs);

            this.#pendingBufferOps.set(uuid, { resolve, reject, timeout });
        });
    }

    #registerPending(bufnum, timeoutMs) {
        const uuid = crypto.randomUUID();
        const allocationComplete = this.#createPendingOperation(uuid, bufnum, timeoutMs);
        return { uuid, allocationComplete };
    }

    async #acquireBufferLock(bufnum) {
        const prev = this.#bufferLocks.get(bufnum) || Promise.resolve();
        let releaseLock;
        const current = new Promise((resolve) => {
            releaseLock = resolve;
        });
        this.#bufferLocks.set(bufnum, prev.then(() => current));
        await prev;

        return () => {
            if (releaseLock) {
                releaseLock();
                releaseLock = null;
            }
            if (this.#bufferLocks.get(bufnum) === current) {
                this.#bufferLocks.delete(bufnum);
            }
        };
    }

    #recordAllocation(bufnum, ptr, sizeBytes, pendingToken, pendingPromise, metadata = {}) {
        const previousEntry = this.#allocatedBuffers.get(bufnum);
        const entry = {
            ptr,
            size: sizeBytes,
            numFrames: metadata.numFrames || 0,
            numChannels: metadata.numChannels || 1,
            sampleRate: metadata.sampleRate || 48000,
            pendingToken,
            pendingPromise,
            previousAllocation: previousEntry
                ? { ptr: previousEntry.ptr, size: previousEntry.size }
                : null
        };
        this.#allocatedBuffers.set(bufnum, entry);
        return entry;
    }

    async #awaitPendingReplacement(bufnum) {
        const existing = this.#allocatedBuffers.get(bufnum);
        if (existing && existing.pendingToken && existing.pendingPromise) {
            try {
                await existing.pendingPromise;
            } catch {
                // Ignore failures; finalizer already handled cleanup
            }
        }
    }

    #attachFinalizer(bufnum, pendingToken, promise) {
        if (!promise || typeof promise.then !== 'function') {
            this.#finalizeReplacement(bufnum, pendingToken, true);
            return Promise.resolve();
        }

        return promise.then((value) => {
            this.#finalizeReplacement(bufnum, pendingToken, true);
            return value;
        }).catch((error) => {
            this.#finalizeReplacement(bufnum, pendingToken, false);
            throw error;
        });
    }

    #finalizeReplacement(bufnum, pendingToken, success) {
        const entry = this.#allocatedBuffers.get(bufnum);
        if (!entry || entry.pendingToken !== pendingToken) {
            return;
        }

        const previous = entry.previousAllocation;

        if (success) {
            entry.pendingToken = null;
            entry.pendingPromise = null;
            entry.previousAllocation = null;
            if (previous?.ptr) {
                this.#bufferPool.free(previous.ptr);
            }
            return;
        }

        if (entry.ptr) {
            this.#bufferPool.free(entry.ptr);
        }

        entry.pendingPromise = null;

        if (previous?.ptr) {
            this.#allocatedBuffers.set(bufnum, {
                ptr: previous.ptr,
                size: previous.size,
                pendingToken: null,
                previousAllocation: null
            });
        } else {
            this.#allocatedBuffers.delete(bufnum);
        }
    }

    /**
     * Handle /buffer/freed notification from scsynth
     * Called by SuperSonic when /buffer/freed OSC message is received
     * @param {Array} args - [bufnum, freedPtr]
     */
    handleBufferFreed(args) {
        const bufnum = args[0];
        const freedPtr = args[1];

        const bufferInfo = this.#allocatedBuffers.get(bufnum);

        if (!bufferInfo) {
            if (typeof freedPtr === 'number' && freedPtr !== 0) {
                this.#bufferPool.free(freedPtr);
            }
            return;
        }

        if (typeof freedPtr === 'number' && freedPtr === bufferInfo.ptr) {
            this.#bufferPool.free(bufferInfo.ptr);
            this.#allocatedBuffers.delete(bufnum);
            return;
        }

        if (
            typeof freedPtr === 'number' &&
            bufferInfo.previousAllocation &&
            bufferInfo.previousAllocation.ptr === freedPtr
        ) {
            this.#bufferPool.free(freedPtr);
            bufferInfo.previousAllocation = null;
            return;
        }

        // Fallback: free whichever pointer we're tracking and clear the entry
        this.#bufferPool.free(bufferInfo.ptr);
        this.#allocatedBuffers.delete(bufnum);
    }

    /**
     * Handle /buffer/allocated notification from scsynth
     * Called by SuperSonic when /buffer/allocated OSC message is received
     * @param {Array} args - [uuid, bufnum]
     */
    handleBufferAllocated(args) {
        const uuid = args[0];  // UUID string
        const bufnum = args[1]; // Buffer number

        // Find and resolve the pending operation
        const pending = this.#pendingBufferOps.get(uuid);
        if (pending) {
            clearTimeout(pending.timeout);
            pending.resolve({ bufnum });
            this.#pendingBufferOps.delete(uuid);
        }
    }

    /**
     * Allocate raw buffer memory
     * @param {number} numSamples - Number of Float32 samples
     * @returns {number} Byte offset, or 0 if failed
     */
    allocate(numSamples) {
        const sizeBytes = numSamples * 4;
        const addr = this.#bufferPool.malloc(sizeBytes);

        if (addr === 0) {
            const stats = this.#bufferPool.stats();
            const availableMB = ((stats.available || 0) / (1024 * 1024)).toFixed(2);
            const totalMB = ((stats.total || 0) / (1024 * 1024)).toFixed(2);
            const requestedMB = (sizeBytes / (1024 * 1024)).toFixed(2);
            console.error(
                `[BufferManager] Allocation failed: requested ${requestedMB}MB, ` +
                `available ${availableMB}MB of ${totalMB}MB total`
            );
        }

        return addr;
    }

    /**
     * Free previously allocated buffer
     * @param {number} addr - Buffer address
     * @returns {boolean} true if freed successfully
     */
    free(addr) {
        return this.#bufferPool.free(addr);
    }

    /**
     * Get Float32Array view of buffer
     * @param {number} addr - Buffer address
     * @param {number} numSamples - Number of samples
     * @returns {Float32Array} Typed array view
     */
    getView(addr, numSamples) {
        return new Float32Array(this.#sharedBuffer, addr, numSamples);
    }

    /**
     * Get buffer pool statistics
     * @returns {Object} Stats including total, available, used
     */
    getStats() {
        return this.#bufferPool.stats();
    }

    /**
     * Get all allocated buffers for recovery
     * Returns info needed to re-send /b_allocPtr to scsynth after a soft reset
     * @returns {Array<{bufnum: number, ptr: number, numFrames: number, numChannels: number, sampleRate: number}>}
     */
    getAllocatedBuffers() {
        const buffers = [];
        for (const [bufnum, entry] of this.#allocatedBuffers.entries()) {
            if (!entry || !entry.ptr) continue;
            buffers.push({
                bufnum,
                ptr: entry.ptr,
                numFrames: entry.numFrames,
                numChannels: entry.numChannels,
                sampleRate: entry.sampleRate
            });
        }
        return buffers;
    }

    /**
     * Update the AudioContext reference after reload
     * Called by SuperSonic during #partialInit() when AudioContext is recreated
     * @param {AudioContext} audioContext - New AudioContext instance
     */
    updateAudioContext(audioContext) {
        if (!audioContext) {
            throw new Error('BufferManager.updateAudioContext requires audioContext');
        }
        this.#audioContext = audioContext;
        if (__DEV__) console.log('[BufferManager] AudioContext updated');
    }

    /**
     * Get buffer diagnostics
     * @returns {Object} Buffer state and pool statistics
     */
    getDiagnostics() {
        const poolStats = this.#bufferPool.stats();
        let bytesActive = 0;
        let pendingCount = 0;

        for (const entry of this.#allocatedBuffers.values()) {
            if (!entry) continue;
            bytesActive += entry.size || 0;
            if (entry.pendingToken) {
                pendingCount++;
            }
        }

        return {
            active: this.#allocatedBuffers.size,
            pending: pendingCount,
            bytesActive,
            pool: {
                total: this.#bufferPoolSize,  // Use configured size, not stats().total (which returns full buffer size)
                available: poolStats.available || 0,
                freeBytes: poolStats.free?.size || 0,
                freeBlocks: poolStats.free?.count || 0,
                usedBytes: poolStats.used?.size || 0,
                usedBlocks: poolStats.used?.count || 0
            }
        };
    }

    /**
     * Clean up resources
     */
    destroy() {
        // Cancel all pending operations
        for (const [uuid, pending] of this.#pendingBufferOps.entries()) {
            clearTimeout(pending.timeout);
            pending.reject(new Error('BufferManager destroyed'));
        }
        this.#pendingBufferOps.clear();

        // Free all allocated buffers
        for (const [bufnum, entry] of this.#allocatedBuffers.entries()) {
            if (entry.ptr) {
                this.#bufferPool.free(entry.ptr);
            }
        }
        this.#allocatedBuffers.clear();

        // Clear buffer locks
        this.#bufferLocks.clear();

        if (__DEV__) console.log('[BufferManager] Destroyed');
    }
}

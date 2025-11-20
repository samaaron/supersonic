/**
 * BufferManager - Handles loading and managing audio buffers
 *
 * Responsibilities:
 * - Load audio files and decode with WebAudio
 * - Allocate memory in buffer pool
 * - Copy decoded audio to SharedArrayBuffer
 * - Manage concurrent buffer operations
 */

export class BufferManager {
    // Private configuration fields
    #sampleBaseURL;
    #audioPathMap;

    constructor(options) {
        const {
            audioContext,
            sharedBuffer,
            bufferPool,
            allocatedBuffers,
            sampleBaseURL,
            audioPathMap = {},
            onPendingOp,
            maxBuffers = 1024
        } = options;

        // Validate required dependencies
        if (!audioContext) {
            throw new Error('BufferManager requires audioContext');
        }
        if (!sharedBuffer || !(sharedBuffer instanceof SharedArrayBuffer)) {
            throw new Error('BufferManager requires sharedBuffer (SharedArrayBuffer)');
        }
        if (!bufferPool || typeof bufferPool.malloc !== 'function' || typeof bufferPool.free !== 'function') {
            throw new Error('BufferManager requires bufferPool with malloc() and free() methods');
        }
        if (!allocatedBuffers || !(allocatedBuffers instanceof Map)) {
            throw new Error('BufferManager requires allocatedBuffers (Map)');
        }

        // Validate optional dependencies
        if (onPendingOp && typeof onPendingOp !== 'function') {
            throw new Error('onPendingOp must be a function');
        }
        if (audioPathMap && typeof audioPathMap !== 'object') {
            throw new Error('audioPathMap must be an object');
        }
        if (!Number.isInteger(maxBuffers) || maxBuffers <= 0) {
            throw new Error('maxBuffers must be a positive integer');
        }

        this.audioContext = audioContext;
        this.sharedBuffer = sharedBuffer;
        this.bufferPool = bufferPool;
        this.allocatedBuffers = allocatedBuffers;
        this.#sampleBaseURL = sampleBaseURL;
        this.#audioPathMap = audioPathMap;
        this.onPendingOp = onPendingOp;
        this.bufferLocks = new Map(); // bufnum -> promise chain tail

        // Guard samples prevent interpolation artifacts at buffer boundaries.
        // SuperCollider uses 3 samples before and 1 sample after for cubic interpolation.
        this.GUARD_BEFORE = 3;
        this.GUARD_AFTER = 1;

        // Maximum buffer count (from config)
        this.MAX_BUFFERS = maxBuffers;
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

        // Reject absolute paths (both Unix and Windows style)
        if (scPath.startsWith('/') || /^[a-zA-Z]:/.test(scPath)) {
            throw new Error(`Invalid audio path: path must be relative (got: ${scPath})`);
        }

        // Reject URL-encoded traversal attempts
        if (scPath.includes('%2e') || scPath.includes('%2E')) {
            throw new Error(`Invalid audio path: path cannot contain URL-encoded characters (got: ${scPath})`);
        }

        // Reject backslash (Windows path separator) to ensure consistent behavior
        if (scPath.includes('\\')) {
            throw new Error(`Invalid audio path: use forward slashes only (got: ${scPath})`);
        }

        // Explicit mapping takes precedence
        if (this.#audioPathMap[scPath]) {
            return this.#audioPathMap[scPath];
        }

        // Check if sampleBaseURL is configured
        if (!this.#sampleBaseURL) {
            throw new Error(
                'sampleBaseURL not configured. Please set it in SuperSonic constructor options.\n' +
                'Example: new SuperSonic({ sampleBaseURL: "./dist/samples/" })\n' +
                'Or use CDN: new SuperSonic({ sampleBaseURL: "https://unpkg.com/supersonic-scsynth-samples@latest/samples/" })\n' +
                'Or install: npm install supersonic-scsynth-samples'
            );
        }

        // Otherwise prepend base URL
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
            const { ptr, sizeBytes, ...extraProps } = await operation();
            allocatedPtr = ptr;

            // Register pending operation
            const { uuid, allocationComplete } = this.#registerPending(bufnum, timeoutMs);
            pendingToken = uuid;
            this.#recordAllocation(bufnum, allocatedPtr, sizeBytes, uuid, allocationComplete);
            allocationRegistered = true;

            const managedCompletion = this.#attachFinalizer(bufnum, uuid, allocationComplete);
            releaseLock();
            lockReleased = true;

            return {
                ptr: allocatedPtr,
                uuid,
                allocationComplete: managedCompletion,
                ...extraProps
            };
        } catch (error) {
            if (allocationRegistered && pendingToken) {
                this.#finalizeReplacement(bufnum, pendingToken, false);
            } else if (allocatedPtr) {
                this.bufferPool.free(allocatedPtr);
            }
            throw error;
        } finally {
            if (!lockReleased) {
                releaseLock();
            }
        }
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
            const response = await fetch(resolvedPath);

            if (!response.ok) {
                throw new Error(`Failed to fetch ${resolvedPath}: ${response.status} ${response.statusText}`);
            }

            const arrayBuffer = await response.arrayBuffer();
            const audioBuffer = await this.audioContext.decodeAudioData(arrayBuffer);

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
                sampleRate: sampleRate || this.audioContext.sampleRate
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
        const ptr = this.bufferPool.malloc(bytesNeeded);

        if (ptr === 0) {
            const stats = this.bufferPool.stats();
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
        const heap = new Float32Array(this.sharedBuffer, ptr, data.length);
        heap.set(data);
    }

    #registerPending(bufnum, timeoutMs) {
        if (!this.onPendingOp) {
            return {
                uuid: crypto.randomUUID(),
                allocationComplete: Promise.resolve()
            };
        }

        const uuid = crypto.randomUUID();
        const allocationComplete = this.onPendingOp(uuid, bufnum, timeoutMs);
        return { uuid, allocationComplete };
    }

    async #acquireBufferLock(bufnum) {
        const prev = this.bufferLocks.get(bufnum) || Promise.resolve();
        let releaseLock;
        const current = new Promise((resolve) => {
            releaseLock = resolve;
        });
        this.bufferLocks.set(bufnum, prev.then(() => current));
        await prev;

        return () => {
            if (releaseLock) {
                releaseLock();
                releaseLock = null;
            }
            if (this.bufferLocks.get(bufnum) === current) {
                this.bufferLocks.delete(bufnum);
            }
        };
    }

    #recordAllocation(bufnum, ptr, sizeBytes, pendingToken, pendingPromise) {
        const previousEntry = this.allocatedBuffers.get(bufnum);
        const entry = {
            ptr,
            size: sizeBytes,
            pendingToken,
            pendingPromise,
            previousAllocation: previousEntry
                ? { ptr: previousEntry.ptr, size: previousEntry.size }
                : null
        };
        this.allocatedBuffers.set(bufnum, entry);
        return entry;
    }

    async #awaitPendingReplacement(bufnum) {
        const existing = this.allocatedBuffers.get(bufnum);
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
        const entry = this.allocatedBuffers.get(bufnum);
        if (!entry || entry.pendingToken !== pendingToken) {
            return;
        }

        const previous = entry.previousAllocation;

        if (success) {
            entry.pendingToken = null;
            entry.pendingPromise = null;
            entry.previousAllocation = null;
            if (previous?.ptr) {
                this.bufferPool.free(previous.ptr);
            }
            return;
        }

        if (entry.ptr) {
            this.bufferPool.free(entry.ptr);
        }

        entry.pendingPromise = null;

        if (previous?.ptr) {
            this.allocatedBuffers.set(bufnum, {
                ptr: previous.ptr,
                size: previous.size,
                pendingToken: null,
                previousAllocation: null
            });
        } else {
            this.allocatedBuffers.delete(bufnum);
        }
    }
}

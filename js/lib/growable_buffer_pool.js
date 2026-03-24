// SPDX-License-Identifier: MIT OR GPL-3.0-or-later
// Copyright (c) 2025 Sam Aaron

/**
 * GrowableBufferPool — On-demand growable buffer pool using chained MemPool instances
 *
 * Wraps multiple @thi.ng/malloc MemPool instances, each covering a contiguous
 * non-overlapping region of WASM memory. When all pools are exhausted, grows
 * the underlying WebAssembly.Memory and creates a new pool for the new region.
 *
 * malloc/free/stats are synchronous (MemPool-compatible interface).
 * grow() is async (SAB mode: resolves immediately, PM mode: message round-trip).
 *
 * Growth appends pages to the end of WASM memory. Existing SharedArrayBuffer
 * views are not detached — all existing pool regions remain valid.
 */

import { MemPool } from '@thi.ng/malloc';

const ALIGN = 8;  // Float64 alignment
const PAGE_SIZE = 65536;  // WASM page size

export class GrowableBufferPool {
    /** @type {Array<{pool: MemPool, start: number, end: number, baseOffset: number}>} */
    #pools = [];
    #wasmMemory;
    #growIncrement;
    #growthCount = 0;
    #growing = false;
    #growFn;
    #onGrowth;
    #nextPoolStart;
    #maxEnd;

    /**
     * @param {object} options
     * @param {SharedArrayBuffer|ArrayBuffer} options.buf - backing buffer for initial pool
     * @param {number} options.start - byte offset for initial pool start
     * @param {number} options.size - byte size of initial pool
     * @param {WebAssembly.Memory|null} [options.wasmMemory] - for SAB mode growth
     * @param {number} options.maxSize - maximum total buffer pool capacity (bytes)
     * @param {number} [options.growIncrement=33554432] - bytes to grow per event (default 32MB)
     * @param {Function|null} [options.growFn] - async function(pages) for PM mode growth
     * @param {Function|null} [options.onGrowth] - callback({poolIndex, newBytes, totalCapacity})
     */
    constructor({ buf, start, size, wasmMemory = null, maxSize, growIncrement = 32 * 1024 * 1024, growFn = null, onGrowth = null }) {
        this.#wasmMemory = wasmMemory;
        this.#growIncrement = growIncrement;
        this.#growFn = growFn;
        this.#onGrowth = onGrowth;
        this.#nextPoolStart = start + size;
        this.#maxEnd = start + maxSize;

        this.#addPool(buf, start, size, 0);
    }

    /**
     * Allocate bytes from the pool chain.
     * Tries pools in reverse order (newest first, most likely to have space).
     * Returns 0 on failure (caller should try grow() then retry).
     */
    malloc(bytes) {
        for (let i = this.#pools.length - 1; i >= 0; i--) {
            const entry = this.#pools[i];
            const localPtr = entry.pool.malloc(bytes);
            if (localPtr !== 0) return localPtr + entry.baseOffset;
        }
        return 0;
    }

    /**
     * Free memory at the given pointer.
     * Routes to the correct pool by address range.
     */
    free(ptr) {
        for (const entry of this.#pools) {
            if (ptr >= entry.start && ptr < entry.end) {
                return entry.pool.free(ptr - entry.baseOffset);
            }
        }
        return false;
    }

    /**
     * Aggregate stats across all pools.
     */
    stats() {
        let freeCount = 0, freeSize = 0;
        let usedCount = 0, usedSize = 0;
        let available = 0, total = 0;
        let top = 0;

        for (const entry of this.#pools) {
            const s = entry.pool.stats();
            freeCount += s.free?.count || 0;
            freeSize += s.free?.size || 0;
            usedCount += s.used?.count || 0;
            usedSize += s.used?.size || 0;
            available += s.available || 0;
            total += s.total || 0;
            if (s.top > top) top = s.top;
        }

        return {
            free: { count: freeCount, size: freeSize },
            used: { count: usedCount, size: usedSize },
            top,
            available,
            total
        };
    }

    /** Whether the pool can grow (hasn't reached maximum). */
    canGrow() {
        return this.#nextPoolStart + ALIGN < this.#maxEnd;
    }

    /**
     * Grow the pool by extending WASM memory and creating a new MemPool segment.
     * @param {number} [minBytes=0] - minimum bytes needed (grow at least this much)
     * @returns {Promise<boolean>} true if growth succeeded
     */
    async grow(minBytes = 0) {
        if (this.#growing) return false;
        if (!this.canGrow()) return false;

        this.#growing = true;
        try {
            const remaining = this.#maxEnd - this.#nextPoolStart;
            const growBytes = Math.min(Math.max(this.#growIncrement, minBytes), remaining);
            if (growBytes < ALIGN) return false;

            const pages = Math.ceil(growBytes / PAGE_SIZE);
            const newStart = this.#nextPoolStart;
            const newSize = Math.min(pages * PAGE_SIZE, this.#maxEnd - newStart);

            let buf;
            if (this.#wasmMemory) {
                const result = this.#wasmMemory.grow(pages);
                if (result === -1) return false;
                buf = this.#wasmMemory.buffer;
            } else if (this.#growFn) {
                const success = await this.#growFn(pages);
                if (!success) return false;
                // PM mode: create a compact local ArrayBuffer for MemPool bookkeeping.
                // MemPool operates at offset 0; baseOffset translates to WASM addresses.
                buf = new ArrayBuffer(newSize);
            } else {
                return false;
            }

            // baseOffset=0 for SAB (MemPool start matches WASM offset),
            // baseOffset=newStart for PM (MemPool starts at 0, we translate)
            const baseOffset = this.#wasmMemory ? 0 : newStart;
            const poolStart = this.#wasmMemory ? newStart : 0;
            this.#addPool(buf, poolStart, newSize, baseOffset);
            this.#nextPoolStart = newStart + newSize;

            this.#growthCount++;
            if (this.#onGrowth) {
                this.#onGrowth({
                    poolIndex: this.#pools.length - 1,
                    newBytes: newSize,
                    totalCapacity: this.totalCapacity
                });
            }

            return true;
        } finally {
            this.#growing = false;
        }
    }

    /** Number of pool segments */
    get poolCount() { return this.#pools.length; }

    /** Number of growth events */
    get growthCount() { return this.#growthCount; }

    /** Current committed capacity across all pools (bytes) */
    get totalCapacity() {
        let total = 0;
        for (const entry of this.#pools) {
            total += entry.end - entry.start;
        }
        return total;
    }

    /** Hard ceiling from config (bytes) */
    get maxCapacity() {
        return this.#maxEnd - this.#pools[0].start;
    }

    /** @private Create and register a MemPool for a memory region */
    #addPool(buf, start, size, baseOffset) {
        const pool = new MemPool({ buf, start, size, align: ALIGN });
        const wasmStart = start + baseOffset;
        this.#pools.push({ pool, start: wasmStart, end: wasmStart + size, baseOffset });
    }
}

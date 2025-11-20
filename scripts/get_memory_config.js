#!/usr/bin/env node
/**
 * Get memory configuration for build.sh
 *
 * Reads js/memory_layout.js and calculates INITIAL_MEMORY for emscripten.
 * This ensures build-time and runtime memory configs stay synchronized.
 *
 * Usage: node scripts/get_memory_config.js
 * Output: Single integer (bytes) suitable for -sINITIAL_MEMORY flag
 */

import { MemoryLayout } from '../js/memory_layout.js';

try {
    const memory = MemoryLayout;

    // Validate memory config exists
    if (!memory || typeof memory !== 'object') {
        console.error('Error: MemoryLayout is missing or invalid');
        process.exit(1);
    }

    // Calculate total memory in bytes
    // Either use totalMemory getter or calculate from totalPages
    let totalMemory;

    if (typeof memory.totalMemory === 'number') {
        totalMemory = memory.totalMemory;
    } else if (typeof memory.totalPages === 'number') {
        totalMemory = memory.totalPages * 65536;
    } else {
        console.error('Error: Unable to determine total memory from config');
        console.error('  memory.totalMemory:', memory.totalMemory);
        console.error('  memory.totalPages:', memory.totalPages);
        process.exit(1);
    }

    // Validate it's a reasonable value
    const MIN_MEMORY = 16 * 1024 * 1024;  // 16MB minimum
    const MAX_MEMORY = 2 * 1024 * 1024 * 1024;  // 2GB maximum

    if (totalMemory < MIN_MEMORY) {
        console.error(`Error: Total memory ${totalMemory} bytes is too small (minimum ${MIN_MEMORY})`);
        process.exit(1);
    }

    if (totalMemory > MAX_MEMORY) {
        console.error(`Error: Total memory ${totalMemory} bytes is too large (maximum ${MAX_MEMORY})`);
        process.exit(1);
    }

    // Must be a multiple of 64KB (WebAssembly page size)
    if (totalMemory % 65536 !== 0) {
        console.error(`Error: Total memory ${totalMemory} bytes is not a multiple of 65536 (WebAssembly page size)`);
        process.exit(1);
    }

    // ========================================================================
    // Internal Consistency Validation
    // ========================================================================

    // Validate totalPages matches totalMemory
    if (typeof memory.totalPages === 'number') {
        const expectedFromPages = memory.totalPages * 65536;
        if (totalMemory !== expectedFromPages) {
            console.error(`Error: Memory layout inconsistency detected`);
            console.error(`  totalPages * 65536 = ${expectedFromPages} bytes`);
            console.error(`  totalMemory = ${totalMemory} bytes`);
            console.error(`  These must match!`);
            process.exit(1);
        }
    }

    // Validate bufferPoolOffset + bufferPoolSize == totalMemory
    if (typeof memory.bufferPoolOffset === 'number' && typeof memory.bufferPoolSize === 'number') {
        const expectedTotal = memory.bufferPoolOffset + memory.bufferPoolSize;
        if (totalMemory !== expectedTotal) {
            console.error(`Error: Memory layout inconsistency detected`);
            console.error(`  bufferPoolOffset + bufferPoolSize = ${expectedTotal} bytes`);
            console.error(`  totalMemory = ${totalMemory} bytes`);
            console.error(`  These must match!`);
            process.exit(1);
        }
    }

    // Validate ringBufferReserved is present and reasonable
    if (typeof memory.ringBufferReserved === 'number') {
        if (memory.ringBufferReserved < 64 * 1024) {
            console.error(`Error: ringBufferReserved (${memory.ringBufferReserved}) is too small (minimum 64KB)`);
            process.exit(1);
        }
        if (memory.ringBufferReserved > 128 * 1024 * 1024) {
            console.error(`Error: ringBufferReserved (${memory.ringBufferReserved}) is unreasonably large (maximum 128MB)`);
            process.exit(1);
        }
    }

    // Validate bufferPoolOffset is after ring buffer space
    if (typeof memory.bufferPoolOffset === 'number' && typeof memory.ringBufferReserved === 'number') {
        if (memory.bufferPoolOffset <= memory.ringBufferReserved) {
            console.error(`Error: bufferPoolOffset (${memory.bufferPoolOffset}) must be greater than ringBufferReserved (${memory.ringBufferReserved})`);
            process.exit(1);
        }
    }

    // Validate wasmHeapSize (if getter exists) is reasonable
    if (typeof memory.wasmHeapSize === 'number') {
        const wasmHeap = memory.wasmHeapSize;
        if (wasmHeap < 1 * 1024 * 1024) {
            console.error(`Error: wasmHeapSize (${wasmHeap}) is too small (minimum 1MB)`);
            process.exit(1);
        }
        if (wasmHeap > totalMemory) {
            console.error(`Error: wasmHeapSize (${wasmHeap}) exceeds totalMemory (${totalMemory})`);
            process.exit(1);
        }
        // Validate it matches the calculation
        if (typeof memory.bufferPoolOffset === 'number' && typeof memory.ringBufferReserved === 'number') {
            const expectedWasmHeap = memory.bufferPoolOffset - memory.ringBufferReserved;
            if (wasmHeap !== expectedWasmHeap) {
                console.error(`Error: wasmHeapSize inconsistency`);
                console.error(`  wasmHeapSize getter returns: ${wasmHeap} bytes`);
                console.error(`  Expected (bufferPoolOffset - ringBufferReserved): ${expectedWasmHeap} bytes`);
                process.exit(1);
            }
        }
    }

    // Validate bufferPoolSize is reasonable
    if (typeof memory.bufferPoolSize === 'number') {
        if (memory.bufferPoolSize < 1 * 1024 * 1024) {
            console.error(`Error: bufferPoolSize (${memory.bufferPoolSize}) is too small (minimum 1MB)`);
            process.exit(1);
        }
        if (memory.bufferPoolSize > totalMemory) {
            console.error(`Error: bufferPoolSize (${memory.bufferPoolSize}) exceeds totalMemory (${totalMemory})`);
            process.exit(1);
        }
    }

    // Output just the number (for use in build.sh)
    console.log(totalMemory);

} catch (error) {
    console.error('Error reading memory configuration:', error.message);
    process.exit(1);
}

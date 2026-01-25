// SPDX-License-Identifier: MIT OR GPL-3.0-or-later
// Copyright (c) 2025 Sam Aaron

/**
 * Control pointer offset constants for SuperSonic.
 *
 * These byte offsets correspond to the ControlPointers struct in shared_memory.h.
 * The struct layout is:
 *
 *   struct alignas(4) ControlPointers {
 *       std::atomic<int32_t> in_head;        // offset 0
 *       std::atomic<int32_t> in_tail;        // offset 4
 *       std::atomic<int32_t> out_head;       // offset 8
 *       std::atomic<int32_t> out_tail;       // offset 12
 *       std::atomic<int32_t> debug_head;     // offset 16
 *       std::atomic<int32_t> debug_tail;     // offset 20
 *       std::atomic<int32_t> in_sequence;    // offset 24
 *       std::atomic<int32_t> out_sequence;   // offset 28
 *       std::atomic<int32_t> debug_sequence; // offset 32
 *       std::atomic<uint32_t> status_flags;  // offset 36
 *       std::atomic<int32_t> in_write_lock;  // offset 40
 *       int32_t _padding;                    // offset 44
 *   };
 */

// =============================================================================
// Byte offsets within ControlPointers struct
// =============================================================================

// IN buffer (OSC messages from JS to scsynth)
export const IN_HEAD = 0;
export const IN_TAIL = 4;

// OUT buffer (OSC replies from scsynth to JS)
export const OUT_HEAD = 8;
export const OUT_TAIL = 12;

// DEBUG buffer (debug messages from scsynth)
export const DEBUG_HEAD = 16;
export const DEBUG_TAIL = 20;

// Sequence counters (for detecting dropped messages)
export const IN_SEQUENCE = 24;
export const OUT_SEQUENCE = 28;
export const DEBUG_SEQUENCE = 32;

// Status and synchronization
export const STATUS_FLAGS = 36;
export const IN_WRITE_LOCK = 40;

// =============================================================================
// Helper functions
// =============================================================================

/**
 * Calculate Int32Array indices for IN buffer control pointers.
 * Used by: OscChannel, SABTransport, osc_out_prescheduler_worker
 *
 * @param {number} ringBufferBase - Base offset of ring buffer region
 * @param {number} CONTROL_START - Offset to control pointers within ring buffer
 * @returns {Object} Object with IN_HEAD, IN_TAIL, IN_SEQUENCE, IN_WRITE_LOCK indices
 */
export function calculateInControlIndices(ringBufferBase, CONTROL_START) {
    const base = ringBufferBase + CONTROL_START;
    return {
        IN_HEAD: (base + IN_HEAD) / 4,
        IN_TAIL: (base + IN_TAIL) / 4,
        IN_SEQUENCE: (base + IN_SEQUENCE) / 4,
        IN_WRITE_LOCK: (base + IN_WRITE_LOCK) / 4,
    };
}

/**
 * Calculate Int32Array indices for OUT buffer control pointers.
 * Used by: osc_in_worker
 *
 * @param {number} ringBufferBase - Base offset of ring buffer region
 * @param {number} CONTROL_START - Offset to control pointers within ring buffer
 * @returns {Object} Object with OUT_HEAD, OUT_TAIL indices
 */
export function calculateOutControlIndices(ringBufferBase, CONTROL_START) {
    const base = ringBufferBase + CONTROL_START;
    return {
        OUT_HEAD: (base + OUT_HEAD) / 4,
        OUT_TAIL: (base + OUT_TAIL) / 4,
    };
}

/**
 * Calculate Int32Array indices for DEBUG buffer control pointers.
 * Used by: debug_worker
 *
 * @param {number} ringBufferBase - Base offset of ring buffer region
 * @param {number} CONTROL_START - Offset to control pointers within ring buffer
 * @returns {Object} Object with DEBUG_HEAD, DEBUG_TAIL indices
 */
export function calculateDebugControlIndices(ringBufferBase, CONTROL_START) {
    const base = ringBufferBase + CONTROL_START;
    return {
        DEBUG_HEAD: (base + DEBUG_HEAD) / 4,
        DEBUG_TAIL: (base + DEBUG_TAIL) / 4,
    };
}

/**
 * Calculate Int32Array indices for all control pointers.
 * Used by: scsynth_audio_worklet
 *
 * @param {number} ringBufferBase - Base offset of ring buffer region
 * @param {number} CONTROL_START - Offset to control pointers within ring buffer
 * @returns {Object} Object with all control pointer indices
 */
export function calculateAllControlIndices(ringBufferBase, CONTROL_START) {
    const base = ringBufferBase + CONTROL_START;
    return {
        IN_HEAD: (base + IN_HEAD) / 4,
        IN_TAIL: (base + IN_TAIL) / 4,
        OUT_HEAD: (base + OUT_HEAD) / 4,
        OUT_TAIL: (base + OUT_TAIL) / 4,
        DEBUG_HEAD: (base + DEBUG_HEAD) / 4,
        DEBUG_TAIL: (base + DEBUG_TAIL) / 4,
        IN_SEQUENCE: (base + IN_SEQUENCE) / 4,
        OUT_SEQUENCE: (base + OUT_SEQUENCE) / 4,
        DEBUG_SEQUENCE: (base + DEBUG_SEQUENCE) / 4,
        STATUS_FLAGS: (base + STATUS_FLAGS) / 4,
        IN_WRITE_LOCK: (base + IN_WRITE_LOCK) / 4,
    };
}

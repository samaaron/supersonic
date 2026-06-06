// SPDX-License-Identifier: MIT OR GPL-3.0-or-later
// Copyright (c) 2025 Sam Aaron

/**
 * OSC timing helpers — NTP time, bundle timetag reading, bundle detection.
 *
 * Producer-side time classification is gone: every message is framed onto the IN
 * ring and the audio thread (the engine's OscIngress + BundleScheduler) decides
 * immediate-vs-future. These remain as shared helpers used across workers and the
 * main thread (NTP stamping, timetag reads).
 */

// NTP_EPOCH_OFFSET and isBundle are intentionally duplicated here (not imported
// from osc_fast.js) because these helpers are imported into AudioWorkletGlobalScope.
// Importing osc_fast.js would pull in TextDecoder at module scope, which is not
// available in all AudioWorklet contexts.

export const NTP_EPOCH_OFFSET = 2208988800;

export function isBundle(oscData) {
    return oscData.length >= 8 &&
        oscData[0] === 0x23 &&  // #
        oscData[1] === 0x62 &&  // b
        oscData[2] === 0x75 &&  // u
        oscData[3] === 0x6e &&  // n
        oscData[4] === 0x64 &&  // d
        oscData[5] === 0x6c &&  // l
        oscData[6] === 0x65 &&  // e
        oscData[7] === 0x00;    // \0
}

/**
 * Read NTP timetag from bundle header as separate uint32 components.
 * @param {Uint8Array} oscData - Bundle data (must be at least 16 bytes)
 * @returns {{ ntpSeconds: number, ntpFraction: number } | null}
 */
export function readTimetag(oscData) {
    if (oscData.length < 16) return null;
    const view = new DataView(oscData.buffer, oscData.byteOffset, oscData.byteLength);
    return {
        ntpSeconds: view.getUint32(8, false),
        ntpFraction: view.getUint32(12, false),
    };
}

/**
 * Get current NTP time from performance clock.
 * Works in any context (main thread or worker).
 * @returns {number} Current NTP time in seconds
 */
export function getCurrentNTPFromPerformance() {
    return (performance.timeOrigin + performance.now()) / 1000 + NTP_EPOCH_OFFSET;
}

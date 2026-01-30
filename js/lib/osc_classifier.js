// SPDX-License-Identifier: MIT OR GPL-3.0-or-later
// Copyright (c) 2025 Sam Aaron

/**
 * OSC Message Classification
 *
 * Shared utility for classifying OSC messages into bypass categories.
 * Used by DirectWriter, SuperSonic, and OscChannel to determine routing.
 *
 * Bypass categories:
 * - 'nonBundle'   - Plain OSC messages (not bundles)
 * - 'immediate'   - Bundles with timetag 0 or 1
 * - 'nearFuture'  - Bundles within bypass threshold of current time
 * - 'late'        - Bundles past their scheduled time
 * - 'farFuture'   - Bundles beyond bypass threshold (needs prescheduler)
 */

/**
 * Default bypass lookahead threshold in seconds.
 * Bundles within this window go direct, beyond go to prescheduler.
 */
export const DEFAULT_BYPASS_LOOKAHEAD_S = 0.5;

/**
 * NTP epoch offset: seconds between Unix epoch (1970) and NTP epoch (1900)
 */
export const NTP_EPOCH_OFFSET = 2208988800;

/**
 * Check if OSC data is a bundle (starts with #bundle\0)
 * @param {Uint8Array} oscData
 * @returns {boolean}
 */
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
 * Read NTP timetag from bundle header
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

/**
 * Classify OSC data for routing
 *
 * @param {Uint8Array} oscData - OSC message or bundle bytes
 * @param {Object} [options] - Classification options
 * @param {Function} [options.getCurrentNTP] - Function returning current NTP time in seconds.
 *                                              Defaults to performance-based calculation.
 * @param {number} [options.bypassLookaheadS=0.2] - Threshold for bypass routing (seconds)
 * @returns {'nonBundle' | 'immediate' | 'nearFuture' | 'late' | 'farFuture'}
 */
export function classifyOscMessage(oscData, options = {}) {
    const {
        getCurrentNTP = getCurrentNTPFromPerformance,
        bypassLookaheadS = DEFAULT_BYPASS_LOOKAHEAD_S,
    } = options;

    // Non-bundle messages always bypass
    if (!isBundle(oscData)) {
        return 'nonBundle';
    }

    // Read timetag from bundle header
    const timetag = readTimetag(oscData);
    if (!timetag) {
        return 'nonBundle'; // Malformed bundle, treat as non-bundle
    }

    const { ntpSeconds, ntpFraction } = timetag;

    // Timetag 0 or 1 means "execute immediately"
    if (ntpSeconds === 0 && ntpFraction <= 1) {
        return 'immediate';
    }

    // Get current NTP time for comparison
    const currentNTP = getCurrentNTP();
    if (currentNTP === null || currentNTP === 0) {
        return 'immediate'; // Can't compare, treat as immediate
    }

    // Calculate time difference
    const bundleNTP = ntpSeconds + ntpFraction / 0x100000000;
    const diffSeconds = bundleNTP - currentNTP;

    // Classify based on time difference
    if (diffSeconds < 0) {
        return 'late';
    }
    if (diffSeconds < bypassLookaheadS) {
        return 'nearFuture';
    }
    return 'farFuture';
}

/**
 * Check if a category should bypass the prescheduler
 * @param {string} category - Classification result
 * @returns {boolean} True if message should go direct, false if to prescheduler
 */
export function shouldBypass(category) {
    return category !== 'farFuture';
}

// SPDX-License-Identifier: MIT OR GPL-3.0-or-later
// Copyright (c) 2025 Sam Aaron

/**
 * OSC Message Classification
 *
 * Classifies OSC messages into routing categories for OscChannel.
 * Mirrors OscClassifier.h in C++ (native backend).
 *
 * Categories:
 * - 'nonBundle'   - Plain OSC messages (not bundles)
 * - 'immediate'   - Bundles with timetag 0 or 1
 * - 'nearFuture'  - Bundles within bypass threshold of current time
 * - 'late'        - Bundles past their scheduled time
 * - 'farFuture'   - Bundles beyond bypass threshold (needs prescheduler)
 */

import { NTP_EPOCH_OFFSET, isBundle } from './osc_fast.js';

export { NTP_EPOCH_OFFSET, isBundle };

/** Default bypass lookahead threshold in seconds. */
export const DEFAULT_BYPASS_LOOKAHEAD_S = 0.5;

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

/**
 * Classify OSC data for routing
 *
 * @param {Uint8Array} oscData - OSC message or bundle bytes
 * @param {Object} [options] - Classification options
 * @param {Function} [options.getCurrentNTP] - Function returning current NTP time in seconds.
 *                                              Defaults to performance-based calculation.
 * @param {number} [options.bypassLookaheadS=0.5] - Threshold for bypass routing (seconds)
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

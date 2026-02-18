// SPDX-License-Identifier: MIT OR GPL-3.0-or-later
// Copyright (c) 2025 Sam Aaron

/**
 * SuperSonic Timing Model
 * =======================
 *
 * SuperSonic synchronizes OSC bundles between JavaScript (wall clock) and
 * the AudioWorklet (AudioContext.currentTime). The formula is:
 *
 *   currentNTP = contextTime + ntpStartTime + drift + clockOffset
 *
 * Where:
 * - contextTime: AudioContext.currentTime (seconds since context created)
 * - ntpStartTime: NTP time when AudioContext started (calculated at init)
 * - drift: Clock skew correction (measured periodically, in milliseconds)
 * - clockOffset: User-supplied offset for multi-system sync (milliseconds)
 *
 * Drift is positive when AudioContext runs slow (behind wall clock),
 * negative when AudioContext runs fast (ahead of wall clock).
 */

import { NTP_EPOCH_OFFSET } from "../timing_constants.js";

/**
 * Calculate current NTP time from performance timestamp
 *
 * @param {number} performanceTimeMs - performance.timeOrigin + timestamp.performanceTime
 * @returns {number} Current NTP time in seconds
 *
 * @example
 * const timestamp = audioContext.getOutputTimestamp();
 * const perfTimeMs = performance.timeOrigin + timestamp.performanceTime;
 * const ntpNow = calculateCurrentNTP(perfTimeMs);
 */
export function calculateCurrentNTP(performanceTimeMs) {
  return performanceTimeMs / 1000 + NTP_EPOCH_OFFSET;
}

/**
 * Calculate NTP time when AudioContext started
 *
 * @param {number} currentNTP - Current NTP time in seconds
 * @param {number} contextTime - Current AudioContext.currentTime
 * @returns {number} NTP time at AudioContext start
 *
 * @example
 * const ntpStart = calculateNTPStartTime(ntpNow, audioContext.currentTime);
 */
export function calculateNTPStartTime(currentNTP, contextTime) {
  return currentNTP - contextTime;
}

/**
 * Calculate drift between expected and actual AudioContext time
 *
 * Positive = AudioContext running slow (behind wall clock, needs time added)
 * Negative = AudioContext running fast (ahead of wall clock, needs time subtracted)
 *
 * @param {number} expectedContextTime - Where contextTime should be based on wall clock
 * @param {number} actualContextTime - Actual AudioContext.currentTime
 * @returns {number} Drift in milliseconds (rounded to integer)
 *
 * @example
 * const expectedContextTime = currentNTP - initialNTPStartTime;
 * const driftMs = calculateDriftMs(expectedContextTime, timestamp.contextTime);
 */
export function calculateDriftMs(expectedContextTime, actualContextTime) {
  const driftSeconds = expectedContextTime - actualContextTime;
  return Math.round(driftSeconds * 1000);
}

/**
 * Convert NTP timetag to AudioContext time
 *
 * @param {number} ntpSeconds - NTP seconds component
 * @param {number} ntpFraction - NTP fraction component (0-0xFFFFFFFF)
 * @param {number} ntpStartTime - NTP time when AudioContext started
 * @param {number} driftSeconds - Current drift correction in seconds
 * @param {number} clockOffsetSeconds - Clock offset for multi-system sync in seconds
 * @returns {number} Target AudioContext time in seconds
 */
export function ntpToAudioTime(
  ntpSeconds,
  ntpFraction,
  ntpStartTime,
  driftSeconds = 0,
  clockOffsetSeconds = 0
) {
  const totalOffset = ntpStartTime + driftSeconds + clockOffsetSeconds;
  const ntpTimeS = ntpSeconds + ntpFraction / 0x100000000;
  return ntpTimeS - totalOffset;
}

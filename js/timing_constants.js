// SPDX-License-Identifier: MIT OR GPL-3.0-or-later
// Copyright (c) 2025 Sam Aaron

/**
 * Timing Constants for NTP and Drift Calculation
 */

// NTP epoch offset: seconds between 1900-01-01 (NTP epoch) and 1970-01-01 (Unix epoch)
export const NTP_EPOCH_OFFSET = 2208988800;

// Drift offset update interval in milliseconds
export const DRIFT_UPDATE_INTERVAL_MS = 15000;

// Delay before calculating initial drift at boot (ms)
// Needs enough elapsed contextTime for accurate measurement
export const INITIAL_DRIFT_DELAY_MS = 500;

// Timeout waiting for /synced response from scsynth
export const SYNC_TIMEOUT_MS = 10000;

// Timeout waiting for AudioWorklet initialization
export const WORKLET_INIT_TIMEOUT_MS = 5000;

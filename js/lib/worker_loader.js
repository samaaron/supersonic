// SPDX-License-Identifier: MIT OR GPL-3.0-or-later
// Copyright (c) 2025 Sam Aaron

// Cache for Blob URLs to avoid re-fetching
const blobUrlCache = new Map();

/**
 * Check if a URL is cross-origin relative to the current page
 * @param {string} url - The URL to check
 * @returns {boolean} - True if cross-origin
 */
export function isCrossOrigin(url) {
    try {
        const scriptUrl = new URL(url, window.location.href);
        return scriptUrl.origin !== window.location.origin;
    } catch {
        return false;
    }
}

/**
 * Fetch a script and create a Blob URL
 * Caches the result to avoid re-fetching
 * @param {string} url - The script URL to fetch
 * @returns {Promise<string>} - Blob URL for the script
 */
export async function fetchAsBlobUrl(url) {
    // Return cached Blob URL if available
    if (blobUrlCache.has(url)) {
        return blobUrlCache.get(url);
    }

    const response = await fetch(url);
    if (!response.ok) {
        throw new Error(`Failed to fetch ${url}: ${response.status} ${response.statusText}`);
    }

    const scriptText = await response.text();
    const blob = new Blob([scriptText], { type: 'application/javascript' });
    const blobUrl = URL.createObjectURL(blob);

    // Cache the Blob URL
    blobUrlCache.set(url, blobUrl);

    return blobUrl;
}

/**
 * Create a Worker, using Blob URL if the script is cross-origin
 * @param {string} url - The worker script URL
 * @param {object} options - Worker options (e.g., { type: 'module' })
 * @returns {Promise<Worker>} - The created Worker
 */
export async function createWorker(url, options = {}) {
    let workerUrl = url;

    if (isCrossOrigin(url)) {
        workerUrl = await fetchAsBlobUrl(url);
    }

    return new Worker(workerUrl, options);
}

/**
 * Add a module to AudioWorklet, using Blob URL if cross-origin
 * @param {AudioWorklet} audioWorklet - The AudioWorklet instance
 * @param {string} url - The worklet script URL
 * @returns {Promise<void>}
 */
export async function addWorkletModule(audioWorklet, url) {
    let moduleUrl = url;

    if (isCrossOrigin(url)) {
        moduleUrl = await fetchAsBlobUrl(url);
    }

    await audioWorklet.addModule(moduleUrl);
}

/**
 * Clear the Blob URL cache and revoke all cached URLs
 * Call this when cleaning up to free memory
 */
export function clearBlobUrlCache() {
    for (const blobUrl of blobUrlCache.values()) {
        URL.revokeObjectURL(blobUrl);
    }
    blobUrlCache.clear();
}

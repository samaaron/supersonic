// SPDX-License-Identifier: MIT OR GPL-3.0-or-later
// Copyright (c) 2025 Sam Aaron

/**
 * AssetLoader - Centralized asset fetching with retry and progress reporting
 *
 * Handles:
 * - Parallel HEAD + GET requests (size available before download completes)
 * - Retry with exponential backoff on network errors and 5xx responses
 * - Loading events (loading:start, loading:complete)
 */

const DEFAULT_MAX_RETRIES = 3;
const DEFAULT_BASE_DELAY = 1000;

export class AssetLoader {
  #onLoadingEvent;
  #maxRetries;
  #baseDelay;
  #skipHeadRequests;

  constructor(options = {}) {
    const {
      onLoadingEvent = null,
      maxRetries = DEFAULT_MAX_RETRIES,
      baseDelay = DEFAULT_BASE_DELAY,
      skipHeadRequests = false,
    } = options;

    this.#onLoadingEvent = onLoadingEvent;
    this.#maxRetries = maxRetries;
    this.#baseDelay = baseDelay;
    this.#skipHeadRequests = skipHeadRequests;
  }

  /**
   * Fetch an asset with retry logic and loading events
   * @param {string} url - URL to fetch
   * @param {Object} options - Options
   * @param {string} options.type - Asset type for events (e.g., 'sample', 'synthdef', 'wasm')
   * @param {string} options.name - Asset name for events
   * @returns {Promise<ArrayBuffer>} The fetched data
   */
  async fetch(url, { type, name }) {
    const getPromise = this.#fetchWithRetry(url);

    if (this.#skipHeadRequests) {
      // Skip HEAD — emit loading:start without size, do GET only
      this.#onLoadingEvent?.('loading:start', { type, name });
    } else {
      // Fire HEAD and GET requests in parallel
      // HEAD returns quickly with Content-Length, GET starts downloading immediately
      const downloadSize = await this.#fetchHead(url);
      this.#onLoadingEvent?.('loading:start', {
        type,
        name,
        ...(downloadSize != null && { size: downloadSize }),
      });
    }

    // Wait for GET to complete
    const response = await getPromise;
    const arrayBuffer = await response.arrayBuffer();

    // Emit loading:complete with download size
    this.#onLoadingEvent?.('loading:complete', {
      type,
      name,
      size: arrayBuffer.byteLength,
    });

    return arrayBuffer;
  }

  /**
   * Fetch HEAD to get Content-Length
   * @private
   */
  async #fetchHead(url) {
    try {
      const response = await fetch(url, { method: 'HEAD' });
      if (response.ok) {
        const contentLength = response.headers.get('Content-Length');
        return contentLength ? parseInt(contentLength, 10) : null;
      }
      return null;
    } catch {
      // HEAD failed, continue without size
      return null;
    }
  }

  /**
   * Fetch with retry logic
   * @private
   */
  async #fetchWithRetry(url) {
    let lastError;

    for (let attempt = 0; attempt <= this.#maxRetries; attempt++) {
      try {
        const response = await fetch(url);

        // Don't retry 4xx errors - they won't succeed
        if (response.status >= 400 && response.status < 500) {
          throw new Error(`Failed to fetch ${url}: ${response.status} ${response.statusText}`);
        }

        // Retry 5xx errors
        if (!response.ok) {
          throw new Error(`Server error fetching ${url}: ${response.status} ${response.statusText}`);
        }

        return response;
      } catch (error) {
        lastError = error;

        // Don't retry 4xx errors (already thrown above with specific status)
        if (error.message.match(/Failed to fetch .+: 4\d{2} /)) {
          throw error;
        }

        // If we have retries left, wait with exponential backoff
        if (attempt < this.#maxRetries) {
          const delay = this.#baseDelay * Math.pow(2, attempt);
          if (__DEV__) {
            console.log(`[AssetLoader] Retry ${attempt + 1}/${this.#maxRetries} for ${url} after ${delay}ms`);
          }
          await this.#sleep(delay);
        }
      }
    }

    throw lastError;
  }

  /**
   * Sleep helper
   * @private
   */
  #sleep(ms) {
    return new Promise(resolve => setTimeout(resolve, ms));
  }
}

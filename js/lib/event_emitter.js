// SPDX-License-Identifier: MIT OR GPL-3.0-or-later
// Copyright (c) 2025 Sam Aaron

/**
 * Simple event emitter mixin for SuperSonic
 * Provides on/off/once/emit pattern
 */
export class EventEmitter {
  #listeners = new Map();

  /**
   * Subscribe to an event
   * @param {string} event - Event name
   * @param {Function} callback - Function to call when event fires
   * @returns {Function} Unsubscribe function
   */
  on(event, callback) {
    if (typeof callback !== 'function') {
      throw new Error('Callback must be a function');
    }
    if (!this.#listeners.has(event)) {
      this.#listeners.set(event, new Set());
    }
    this.#listeners.get(event).add(callback);
    return () => this.off(event, callback);
  }

  /**
   * Unsubscribe from an event
   * @param {string} event - Event name
   * @param {Function} callback - Function to remove
   * @returns {this} For chaining
   */
  off(event, callback) {
    const listeners = this.#listeners.get(event);
    if (listeners) {
      listeners.delete(callback);
    }
    return this;
  }

  /**
   * Subscribe to an event once (auto-unsubscribes after first call)
   * @param {string} event - Event name
   * @param {Function} callback - Function to call once
   * @returns {Function} Unsubscribe function
   */
  once(event, callback) {
    const wrapper = (...args) => {
      this.off(event, wrapper);
      callback(...args);
    };
    return this.on(event, wrapper);
  }

  /**
   * Remove all listeners for an event, or all listeners entirely
   * @param {string} [event] - Event name. If omitted, removes ALL listeners.
   * @returns {this} For chaining
   */
  removeAllListeners(event) {
    if (event === undefined) {
      this.#listeners.clear();
    } else {
      this.#listeners.delete(event);
    }
    return this;
  }

  /**
   * Check if there are listeners for an event
   * @param {string} event - Event name
   * @returns {boolean}
   */
  hasListeners(event) {
    const listeners = this.#listeners.get(event);
    return listeners ? listeners.size > 0 : false;
  }

  /**
   * Emit an event to all listeners
   * @param {string} event - Event name
   * @param {...*} args - Arguments to pass to listeners
   */
  emit(event, ...args) {
    const listeners = this.#listeners.get(event);
    if (listeners) {
      for (const callback of listeners) {
        try {
          callback(...args);
        } catch (error) {
          console.error(`[EventEmitter] Error in ${event} listener:`, error);
        }
      }
    }
  }

  /**
   * Emit an event and await all listeners (for async handlers)
   * @param {string} event - Event name
   * @param {...*} args - Arguments to pass to listeners
   */
  async emitAsync(event, ...args) {
    const listeners = this.#listeners.get(event);
    if (listeners) {
      for (const callback of listeners) {
        try {
          await callback(...args);
        } catch (error) {
          console.error(`[EventEmitter] Error in ${event} listener:`, error);
        }
      }
    }
  }

  /**
   * Clear all listeners (for cleanup)
   */
  clearAllListeners() {
    this.#listeners.clear();
  }
}

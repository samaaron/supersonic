// SPDX-License-Identifier: MIT OR GPL-3.0-or-later
// Copyright (c) 2025 Sam Aaron

export { Transport } from './transport.js';
export { SABTransport } from './sab_transport.js';
export { PostMessageTransport } from './postmessage_transport.js';

import { SABTransport } from './sab_transport.js';
import { PostMessageTransport } from './postmessage_transport.js';

/**
 * Create a transport based on mode
 *
 * @param {'sab' | 'postMessage'} mode - Transport mode
 * @param {Object} config - Configuration for the transport
 * @returns {Transport}
 */
export function createTransport(mode, config) {
    if (mode === 'sab') {
        return new SABTransport(config);
    } else if (mode === 'postMessage') {
        return new PostMessageTransport(config);
    } else {
        throw new Error(`Unknown transport mode: ${mode}. Use 'sab' or 'postMessage'`);
    }
}

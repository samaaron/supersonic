// SPDX-License-Identifier: MIT OR GPL-3.0-or-later
// Copyright (c) 2025 Sam Aaron
//
// Slim entry point for OscChannel — safe to import in AudioWorkletGlobalScope.
// Only pulls in OscChannel and its dependencies (ring buffer, classifier, offsets).
// No TextDecoder, no Worker, no window/document/fetch.

export { OscChannel } from './lib/osc_channel.js';

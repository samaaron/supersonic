// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2025 Sam Aaron

// CDN path helper for supersonic-scsynth-core
// This is the base URL for loading WASM and worker files from CDN

export const CORE_CDN = 'https://unpkg.com/supersonic-scsynth-core@0.89.0/';
export const WASM_CDN = `${CORE_CDN}wasm/`;
export const WORKLET_CDN = `${CORE_CDN}workers/clockwork_audio_worklet.js`;

// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2025 Sam Aaron

// CDN path helper for supersonic-scsynth-core
// This is the base URL for loading WASM and worker files from CDN

export const CORE_CDN = 'https://unpkg.com/supersonic-scsynth-core@latest/';
export const WASM_CDN = 'https://unpkg.com/supersonic-scsynth-core@latest/wasm/';
export const WORKLET_CDN = 'https://unpkg.com/supersonic-scsynth-core@latest/workers/scsynth_audio_worklet.js';

/** @deprecated Use WORKLET_CDN instead */
export const WORKERS_CDN = 'https://unpkg.com/supersonic-scsynth-core@latest/workers/';

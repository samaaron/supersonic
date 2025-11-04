// SuperSonic Samples - All 206 Sonic Pi audio samples
// License: CC0-1.0 (Public Domain)

import { fileURLToPath } from 'url';
import { dirname, join } from 'path';

const __filename = fileURLToPath(import.meta.url);
const __dirname = dirname(__filename);

// Path to samples directory
export const SAMPLES_DIR = join(__dirname, 'samples');

// CDN URLs for convenience
export const UNPKG_BASE = 'https://unpkg.com/supersonic-scsynth-samples@0.1.5/samples/';
export const JSDELIVR_BASE = 'https://cdn.jsdelivr.net/npm/supersonic-scsynth-samples@0.1.5/samples/';

// Helper to get sample path
export function getSamplePath(filename) {
  return join(SAMPLES_DIR, filename);
}

// Helper to get CDN URL
export function getSampleURL(filename, cdn = 'unpkg') {
  const base = cdn === 'jsdelivr' ? JSDELIVR_BASE : UNPKG_BASE;
  return base + filename;
}

export default {
  SAMPLES_DIR,
  UNPKG_BASE,
  JSDELIVR_BASE,
  getSamplePath,
  getSampleURL
};

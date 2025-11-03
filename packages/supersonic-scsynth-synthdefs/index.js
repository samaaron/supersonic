// Sonic Pi SynthDefs for SuperSonic
// All 120 binary synthdef files from Sonic Pi

import { fileURLToPath } from 'url';
import { dirname, join } from 'path';

const __filename = fileURLToPath(import.meta.url);
const __dirname = dirname(__filename);

export const SYNTHDEFS_DIR = join(__dirname, 'synthdefs');

// Helper to get full path for a synthdef
export function getSynthDefPath(name) {
    return join(SYNTHDEFS_DIR, `${name}.scsyndef`);
}

// CDN URL for browser usage (unpkg)
export const CDN_BASE = 'https://unpkg.com/supersonic-scsynth-synthdefs@0.1.1';
export const SYNTHDEFS_CDN = `${CDN_BASE}/synthdefs`;

export default {
    SYNTHDEFS_DIR,
    getSynthDefPath,
    CDN_BASE,
    SYNTHDEFS_CDN
};

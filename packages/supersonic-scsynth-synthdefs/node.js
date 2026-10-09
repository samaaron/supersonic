// Sonic Pi SynthDefs for SuperSonic, under Node: everything index.js has, and
// where the files are on disk. package.json's "node" condition picks this file.

import { fileURLToPath } from 'node:url';
import { join } from 'node:path';
import helper from './index.js';

export * from './index.js';

export const SYNTHDEFS_DIR = fileURLToPath(new URL('./synthdefs', import.meta.url));

// Helper to get full path for a synthdef
export function getSynthDefPath(name) {
    return join(SYNTHDEFS_DIR, `${name}.scsyndef`);
}

export default {
    ...helper,
    SYNTHDEFS_DIR,
    getSynthDefPath
};

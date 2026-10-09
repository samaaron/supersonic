// SuperSonic Samples, under Node: everything index.js has, and where the
// files are on disk. package.json's "node" condition picks this file.
// License: CC0-1.0 (Public Domain)

import { fileURLToPath } from 'node:url';
import { join } from 'node:path';
import helper from './index.js';

export * from './index.js';

// Path to samples directory
export const SAMPLES_DIR = fileURLToPath(new URL('./samples', import.meta.url));

// Helper to get sample path
export function getSamplePath(filename) {
  return join(SAMPLES_DIR, filename);
}

export default {
  ...helper,
  SAMPLES_DIR,
  getSamplePath
};

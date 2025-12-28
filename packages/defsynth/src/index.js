/**
 * defsynth - A language and framework for designing SuperCollider SynthDefs
 *
 * Homage to Overtone's defsynth macro.
 */

import { createRequire } from 'module';
const require = createRequire(import.meta.url);

// UGen metadata parsed from SuperCollider source
export const ugenMetadata = require('./ugens/metadata.json');

// Re-export ugens for convenience
export const ugens = ugenMetadata.ugens;

// Get a specific UGen spec
export function getUGen(name) {
  return ugens[name] || null;
}

// List all available UGen names
export function listUGens() {
  return Object.keys(ugens);
}

// List UGens by category
export function listUGensByCategory(category) {
  return Object.entries(ugens)
    .filter(([_, ugen]) => ugen.categories?.includes(category))
    .map(([name, _]) => name);
}

// List all categories
export function listCategories() {
  const categories = new Set();
  for (const ugen of Object.values(ugens)) {
    if (ugen.categories) {
      categories.add(ugen.categories);
    }
  }
  return [...categories].sort();
}

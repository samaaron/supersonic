#!/usr/bin/env node

// Generate SYNTHDEF_NAMES and SAMPLE_NAMES manifest arrays
// and write them into the respective index.js files.
//
// Usage: node generate-manifests.mjs

import { readdirSync, readFileSync, writeFileSync } from 'fs';
import { join, dirname } from 'path';
import { fileURLToPath } from 'url';

const __dirname = dirname(fileURLToPath(import.meta.url));

function getNames(dir, extension) {
  return readdirSync(dir)
    .filter(f => f.endsWith(extension))
    .map(f => f.replace(extension, ''))
    .sort();
}

function updateIndexFile(filePath, exportName, names) {
  let content = readFileSync(filePath, 'utf-8');

  const arrayStr = 'export const ' + exportName + ' = [\n'
    + names.map(n => `  "${n}",`).join('\n')
    + '\n];\n';

  // Replace existing manifest or append before default export
  const marker = `export const ${exportName}`;
  if (content.includes(marker)) {
    // Replace from marker to closing ];\n
    const regex = new RegExp(`export const ${exportName} = \\[[\\s\\S]*?\\];\\n`);
    content = content.replace(regex, arrayStr);
  } else {
    // Insert before the default export
    const defaultExportIndex = content.indexOf('\nexport default');
    if (defaultExportIndex !== -1) {
      content = content.slice(0, defaultExportIndex) + '\n' + arrayStr + content.slice(defaultExportIndex);
    } else {
      content += '\n' + arrayStr;
    }
  }

  writeFileSync(filePath, content);
  return names.length;
}

// Synthdefs
const synthdefsDir = join(__dirname, 'packages/supersonic-scsynth-synthdefs/synthdefs');
const synthdefNames = getNames(synthdefsDir, '.scsyndef');
const synthdefIndex = join(__dirname, 'packages/supersonic-scsynth-synthdefs/index.js');
const synthCount = updateIndexFile(synthdefIndex, 'SYNTHDEF_NAMES', synthdefNames);
console.log(`Wrote ${synthCount} synthdef names to ${synthdefIndex}`);

// Samples
const samplesDir = join(__dirname, 'packages/supersonic-scsynth-samples/samples');
const sampleNames = getNames(samplesDir, '.flac');
const samplesIndex = join(__dirname, 'packages/supersonic-scsynth-samples/index.js');
const sampleCount = updateIndexFile(samplesIndex, 'SAMPLE_NAMES', sampleNames);
console.log(`Wrote ${sampleCount} sample names to ${samplesIndex}`);

// Contract test: supersonic.d.ts declares every metric Clockwork's schema
// names, so a metric getMetrics() returns is never missing from the types.
//
// The ring-buffer words are the exception: the declarations group them as
// `inBufferUsed`, `outBufferUsed` and `nrtOutBufferUsed` (RingBufferUsage).
import { test, expect } from '@playwright/test';
import { readFileSync } from 'fs';
import { fileURLToPath, pathToFileURL } from 'url';
import { dirname, join } from 'path';

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..');

const GROUPED = new Set([
  'inBufferUsedBytes', 'outBufferUsedBytes', 'nrtOutBufferUsedBytes',
  'inBufferPeakBytes', 'outBufferPeakBytes', 'nrtOutBufferPeakBytes',
  'inBufferCapacity', 'outBufferCapacity', 'nrtOutBufferCapacity',
]);

test('supersonic.d.ts declares every metric in Clockwork\'s schema', async () => {
  const types = readFileSync(join(ROOT, 'supersonic.d.ts'), 'utf8');
  const { METRICS_SCHEMA } = await import(pathToFileURL(join(ROOT, 'clockwork/js/lib/metrics_schema.js')).href);
  const keys = Object.keys(METRICS_SCHEMA.metrics);
  expect(keys.length, 'no metrics read from the schema').toBeGreaterThan(40);

  const body = types.slice(types.indexOf('export interface SuperSonicMetrics'));
  const missing = keys.filter((k) => !GROUPED.has(k) && !new RegExp(`\\b${k}\\??:`).test(body));
  expect(missing, 'metrics in the schema and not in SuperSonicMetrics').toEqual([]);
});

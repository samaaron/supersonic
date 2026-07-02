// metrics_schema_header.test.mjs — src/metrics_schema.h is generated from
// js/lib/metrics_schema.js (the canonical metrics schema shared by every
// GUI). This test regenerates the header text in memory and requires it to
// be byte-identical to the committed file, so the two cannot drift apart
// silently — same pattern as ring_wire_conformance.test.mjs.
//
// Run: npm run test:unit
// After an INTENDED schema change:  npm run gen:metrics-header  (and commit)

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { renderHeader } from '../../scripts/gen-metrics-schema-header.mjs';

test('src/metrics_schema.h matches js/lib/metrics_schema.js', () => {
  const committed = readFileSync(
    new URL('../../src/metrics_schema.h', import.meta.url), 'utf8');
  assert.equal(committed, renderHeader(),
    'src/metrics_schema.h is stale — regenerate with: npm run gen:metrics-header');
});

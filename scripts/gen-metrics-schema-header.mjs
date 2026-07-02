#!/usr/bin/env node
// SPDX-License-Identifier: MIT OR GPL-3.0-or-later
// Copyright (c) 2025 Sam Aaron
//
// Generates src/metrics_schema.h from js/lib/metrics_schema.js so native
// GUIs (Sonic Pi's Qt metrics panel, tau-state, ...) can render the same
// metric names and descriptions as the <supersonic-metrics> web component.
//
// Usage:  npm run gen:metrics-header
// Commit the regenerated header alongside changes to metrics_schema.js —
// test/unit/metrics_schema_header.test.mjs fails if the two drift.

import { writeFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { METRICS_SCHEMA } from '../js/lib/metrics_schema.js';

const cEscape = (s) => s.replace(/\\/g, '\\\\').replace(/"/g, '\\"');

// Render the C++ header text from the schema. Exported so the staleness
// test can compare against the committed src/metrics_schema.h.
export function renderHeader(schema = METRICS_SCHEMA) {
  const metrics = Object.entries(schema.metrics)
    .map(([key, def]) => ({ key, ...def }))
    .sort((a, b) => a.offset - b.offset);

  const nativeStats = Object.entries(schema.nativeStats)
    .map(([key, def]) => ({ key, ...def }))
    .sort((a, b) => a.index - b.index);

  const fieldLines = metrics.map((m) =>
    `    { ${String(m.offset).padStart(2)}, "${cEscape(m.key)}", "${cEscape(m.unit ?? '')}", "${cEscape(m.description)}" },`);

  const nativeLines = nativeStats.map((m) =>
    `    { ${m.index}, "${cEscape(m.key)}", "${cEscape(m.unit ?? '')}", "${cEscape(m.description)}" },`);

  return `// SPDX-License-Identifier: MIT OR GPL-3.0-or-later
// Copyright (c) 2025 Sam Aaron
//
// GENERATED FILE — DO NOT EDIT.
// Source of truth: js/lib/metrics_schema.js
// Regenerate with:  npm run gen:metrics-header
// Staleness is enforced by test/unit/metrics_schema_header.test.mjs.
//
// Metric names, units and human-readable descriptions for SuperSonic's
// performance metrics, for use by native GUIs. Offsets index the
// PerformanceMetrics struct (see shared_memory.h); native-stat indices
// address the separate NATIVE_STATS segment.

#pragma once

#include <cstdint>

namespace supersonic {
namespace metrics_schema {

struct FieldInfo
{
    uint32_t offset;         // index into the metrics Uint32 array
    const char* key;         // schema key (stable identifier)
    const char* unit;        // unit of measurement ("" if none)
    const char* description; // human-readable description
};

inline constexpr FieldInfo kFields[] = {
${fieldLines.join('\n')}
};

struct NativeStatInfo
{
    uint32_t index;          // u32 slot within the NATIVE_STATS segment
    const char* key;
    const char* unit;
    const char* description;
};

inline constexpr NativeStatInfo kNativeStats[] = {
${nativeLines.join('\n')}
};

inline const char* descriptionForOffset(uint32_t offset)
{
    for (const FieldInfo& f : kFields)
        if (f.offset == offset)
            return f.description;
    return nullptr;
}

inline const char* descriptionForNativeStat(uint32_t index)
{
    for (const NativeStatInfo& f : kNativeStats)
        if (f.index == index)
            return f.description;
    return nullptr;
}

} // namespace metrics_schema
} // namespace supersonic
`;
}

// CLI entry: write the header in place.
if (process.argv[1] && fileURLToPath(import.meta.url) === process.argv[1]) {
  const out = join(dirname(fileURLToPath(import.meta.url)), '..', 'src', 'metrics_schema.h');
  writeFileSync(out, renderHeader());
  console.log(`wrote ${out}`);
}

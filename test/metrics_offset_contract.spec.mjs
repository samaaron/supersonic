// Contract test: js/lib/metrics_offsets.js must mirror the authoritative C++
// PerformanceMetrics layout in src/shared_memory.h.
//
// shared_memory.h is the source of truth: its SS_ASSERT_METRIC(field, idx)
// static_asserts enforce, at compile time, that each struct field sits at its
// declared index. This test enforces the other half of the contract — that the
// JS offset constants the workers/readers use match those same indices — so a
// renumber on one side can never silently drift from the other (the failure that
// otherwise only surfaces as a wrong metric value at runtime).
//
// If you add a C++ metric, add its JS mapping here; the test fails until you do.
import { test, expect } from '@playwright/test';
import { readFileSync } from 'fs';
import { fileURLToPath } from 'url';
import { dirname, join } from 'path';

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..');

// C++ PerformanceMetrics field  →  metrics_offsets.js constant.
const CPP_TO_JS = {
  process_count:                 'SCSYNTH_PROCESS_COUNT',
  messages_processed:            'SCSYNTH_MESSAGES_PROCESSED',
  messages_dropped:              'SCSYNTH_MESSAGES_DROPPED',
  scheduler_queue_depth:         'SCSYNTH_SCHEDULER_DEPTH',
  scheduler_queue_max:           'SCSYNTH_SCHEDULER_PEAK_DEPTH',
  scheduler_queue_dropped:       'SCSYNTH_SCHEDULER_DROPPED',
  messages_sequence_gaps:        'SCSYNTH_SEQUENCE_GAPS',
  wasm_errors:                   'SCSYNTH_WASM_ERRORS',
  scheduler_lates:               'SCSYNTH_SCHEDULER_LATES',
  osc_out_messages_sent:         'OSC_OUT_MESSAGES_SENT',
  osc_out_bytes_sent:            'OSC_OUT_BYTES_SENT',
  osc_in_messages_received:      'OSC_IN_MESSAGES_RECEIVED',
  osc_in_bytes_received:         'OSC_IN_BYTES_RECEIVED',
  osc_in_dropped_messages:       'OSC_IN_DROPPED_MESSAGES',
  osc_in_corrupted:              'OSC_IN_CORRUPTED',
  debug_messages_received:       'DEBUG_MESSAGES_RECEIVED',
  debug_bytes_received:          'DEBUG_BYTES_RECEIVED',
  in_buffer_used_bytes:          'IN_BUFFER_USED_BYTES',
  out_buffer_used_bytes:         'OUT_BUFFER_USED_BYTES',
  nrt_out_buffer_used_bytes:       'NRT_OUT_BUFFER_USED_BYTES',
  in_buffer_peak_bytes:          'IN_BUFFER_PEAK_BYTES',
  out_buffer_peak_bytes:         'OUT_BUFFER_PEAK_BYTES',
  nrt_out_buffer_peak_bytes:       'NRT_OUT_BUFFER_PEAK_BYTES',
  scheduler_max_late_ms:         'SCSYNTH_SCHEDULER_MAX_LATE_MS',
  scheduler_last_late_ms:        'SCSYNTH_SCHEDULER_LAST_LATE_MS',
  scheduler_last_late_tick:      'SCSYNTH_SCHEDULER_LAST_LATE_TICK',
  ring_buffer_direct_write_fails:'RING_BUFFER_DIRECT_WRITE_FAILS',
  supersonic_version_major:      'SUPERSONIC_VERSION_MAJOR',
  supersonic_version_minor:      'SUPERSONIC_VERSION_MINOR',
  supersonic_version_patch:      'SUPERSONIC_VERSION_PATCH',
  audio_sample_rate:             'AUDIO_SAMPLE_RATE',
  audio_block_size:              'AUDIO_BLOCK_SIZE',
  audio_output_channels:         'AUDIO_OUTPUT_CHANNELS',
  audio_input_channels:          'AUDIO_INPUT_CHANNELS',
  clock_tempo_mbpm:              'CLOCK_TEMPO_MBPM',
  clock_beat_centi:              'CLOCK_BEAT_CENTI',
  clock_phase_centi:             'CLOCK_PHASE_CENTI',
  clock_playing:                 'CLOCK_PLAYING',
};

// Pure padding slots — asserted in C++ for alignment but have no JS counterpart.
const CPP_PADDING = new Set(['_metrics_reserved', '_metrics_reserved2']);

test('metrics_offsets.js mirrors the authoritative C++ PerformanceMetrics layout', () => {
  const hdr = readFileSync(join(ROOT, 'src/shared_memory.h'), 'utf8');
  const js  = readFileSync(join(ROOT, 'js/lib/metrics_offsets.js'), 'utf8');

  const cpp = {};
  for (const m of hdr.matchAll(/SS_ASSERT_METRIC\(\s*(\w+)\s*,\s*(\d+)\s*\)/g))
    cpp[m[1]] = Number(m[2]);

  const jsOff = {};
  for (const m of js.matchAll(/export const (\w+)\s*=\s*(\d+)\s*;/g))
    jsOff[m[1]] = Number(m[2]);

  expect(Object.keys(cpp).length, 'no SS_ASSERT_METRIC lines parsed').toBeGreaterThan(10);

  const unmapped = [];
  const mismatches = [];
  for (const [field, idx] of Object.entries(cpp)) {
    if (CPP_PADDING.has(field)) continue;
    const jsName = CPP_TO_JS[field];
    if (!jsName) { unmapped.push(`${field} (C++ @${idx})`); continue; }
    if (jsOff[jsName] === undefined) mismatches.push(`${field}: JS const ${jsName} not found`);
    else if (jsOff[jsName] !== idx)
      mismatches.push(`${field}: C++ @${idx} but ${jsName} @${jsOff[jsName]}`);
  }

  // A new/renamed C++ metric with no JS mapping must be added to CPP_TO_JS.
  expect(unmapped, 'C++ metrics with no JS mapping — add them to CPP_TO_JS').toEqual([]);
  // The core invariant: every mapped offset agrees.
  expect(mismatches, 'C++/JS metric offset drift').toEqual([]);
});

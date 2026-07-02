// Contract test: the positional world-options block has THREE writers/readers
// that must agree on every index — the worklet JS (writeWorldOptionsToMemory),
// the C++ WorldOpts enum (src/audio_config.h) with init_memory's reads, and
// ss_init's struct-based writes (src/lanes/lanes.cpp).
//
// Indices 0-15 are shared across runtimes. From 16 the meanings diverge per
// runtime, so each meaning must have a named constant and every C++ site must
// use the name. The hazard this pins down: ss_init writes EVERY slot for
// struct-based hosts, and index 16 is web-owned (RT pool offset) — a slot
// that is unnamed, or named for a meaning nothing reads, lets those two
// writers collide silently.
import { test, expect } from '@playwright/test';
import { readFileSync } from 'fs';
import { fileURLToPath } from 'url';
import { dirname, join } from 'path';

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..');

const audioConfig = readFileSync(join(ROOT, 'src/audio_config.h'), 'utf8');
const audioProcessor = readFileSync(join(ROOT, 'src/audio_processor.cpp'), 'utf8');
const lanes = readFileSync(join(ROOT, 'src/lanes/lanes.cpp'), 'utf8');
const worklet = readFileSync(
  join(ROOT, 'js/workers/scsynth_audio_worklet.js'), 'utf8');

// Parse `kName = N,` pairs out of the WorldOpts enum.
function enumIndices(src) {
  const out = {};
  for (const m of src.matchAll(/\bk(\w+)\s*=\s*(\d+)\s*,/g))
    out['k' + m[1]] = Number(m[2]);
  return out;
}

test.describe('world-options block contract', () => {
  const idx = enumIndices(audioConfig);

  test('runtime-divergent indices all have named constants', () => {
    // Index 16: web RT pool offset. There is no native meaning — the name
    // must say what the slot actually carries, not invent one.
    expect(idx.kWebRtPoolOffset).toBe(16);
    expect(idx.kMode).toBeUndefined();

    // Index 17: native shm id / web transport flag (dual-named on purpose).
    expect(idx.kNativeSharedMemoryID).toBe(17);
    expect(idx.kWebTransportFlag).toBe(17);
  });

  test('worklet JS writes the divergent indices where the enum says', () => {
    // Scope to the options writer — uint32View is reused by unrelated
    // functions (e.g. buffer-constants parsing).
    const body = worklet.match(
      /writeWorldOptionsToMemory\(\)\s*{([\s\S]*?)\n    }/)?.[1];
    expect(body, 'writeWorldOptionsToMemory not found').toBeTruthy();

    // uint32View[16] = ...rtPoolOffset...
    expect(body).toMatch(
      /uint32View\[16\]\s*=\s*this\.worldOptions\.rtPoolOffset/);
    // uint32View[17] = transport flag
    expect(body).toMatch(/uint32View\[17\]\s*=\s*this\.mode\s*===/);
    // Index 18 has no meaning on any runtime; nothing may write it.
    expect(body).not.toMatch(/uint32View\[18\]/);
  });

  test('init_memory reads divergent indices via their named constants', () => {
    // Raw magic indices >= 16 are banned in C++ readers/writers.
    expect(audioProcessor).not.toMatch(/worldOptionsPtr\[1[678]\]/);
    expect(audioProcessor).toMatch(
      /worldOptionsPtr\[sonicpi::WorldOpts::kWebRtPoolOffset\]/);
    expect(audioProcessor).toMatch(
      /worldOptionsPtr\[sonicpi::WorldOpts::kWebTransportFlag\]/);
    expect(audioProcessor).toMatch(
      /worldOptionsPtr\[sonicpi::WorldOpts::kNativeSharedMemoryID\]/);
  });

  test('ss_init never writes web-owned slots to a live value', () => {
    // ss_init writes the whole positional block for struct-based hosts.
    // Struct hosts have no external RT pool, so the web-owned slot must be
    // an explicit zero, written under its real name.
    expect(lanes).not.toMatch(/\bkMode\b/);
    expect(lanes).toMatch(/a\[kWebRtPoolOffset\]\s*=\s*0/);
  });
});

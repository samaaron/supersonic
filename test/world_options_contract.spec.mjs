// Contract test: the guest's config block has ONE reader and TWO writers, and
// every field they share has to sit at the same slot in all three.
//
//   reader   dsp/scsynth/scsynth_dsp.cpp        slot(i, fallback) / cfg[i]
//   writer   js/scsynth_options.js              encodeScsynthOptions, u32[i]
//   writer   clockwork/src/native/GuestConfigBlock.h   clockwork::guest_config
//
// THIS FILE USED TO PIN A DIFFERENT DESIGN. It checked a `WorldOpts` enum in
// clockwork/src/audio_config.h against init_memory's reads and ss_init's writes,
// with named constants required for every index from 16 up because the
// meanings diverged per runtime there. All of it is gone: clockwork stopped
// reading the block at all on 2026-08-31, the enum moved out of guest-agnostic
// code, and slot 16 — the RT pool offset the web client passed to the engine —
// went with the side channel that read it. Every symbol this file named
// (`worldOptionsPtr`, `kWebRtPoolOffset`, `kNativeSharedMemoryID`,
// `kWebTransportFlag`) now appears nowhere in the tree, so it was pinning a
// design rather than the code, and could not run at all once the enum moved.
//
// What replaced it is a sharper hazard and the reason this file still exists.
// Clockwork treats the block as opaque bytes, so NOTHING in C++ or JS
// type-checks it: the numbers are the only agreement there is. Two hosts write
// it and one guest reads it, and when the native host packed its fields into
// slots 0-8 while the guest read the web layout, the first five lined up by
// coincidence and the rest silently did not — the guest took numRGens as its
// control-bus count and fell back to a compile-time default for its real-time
// memory. 985 native cases and 1387 web cases passed over that, because every
// wrong slot held either a plausible number or a zero indistinguishable from
// "the host did not specify this".
//
// Source text, not a running engine, on purpose: a booted engine cannot tell
// you that two writers disagree, only that it got a number it was willing to
// use.
import { test, expect } from '@playwright/test';
import { readFileSync } from 'fs';
import { fileURLToPath } from 'url';
import { dirname, join } from 'path';

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..');
const read = (p) => readFileSync(join(ROOT, p), 'utf8');

const guestReader = read('dsp/scsynth/scsynth_dsp.cpp');
const webEncoder  = read('js/scsynth_options.js');
const nativeBlock = read('clockwork/src/native/GuestConfigBlock.h');

// ── Parsing the three sources ───────────────────────────────────────────────

/* `kName = N,` out of clockwork::guest_config. */
function nativeSlots(src) {
  const body = src.match(/namespace guest_config \{[\s\S]*?\n\}/)?.[0] ?? '';
  const out = {};
  for (const m of body.matchAll(/\bk(\w+)\s*=\s*(\d+)\s*,/g)) out[m[1]] = Number(m[2]);
  return out;
}

/* `u32[N] = o.name ...` out of encodeScsynthOptions. */
function webSlots(src) {
  const body = src.match(/export function encodeScsynthOptions\([\s\S]*?\n}/)?.[0] ?? '';
  const out = {};
  for (const m of body.matchAll(/u32\[(\d+)\]\s*=\s*o\.(\w+)/g)) out[m[2]] = Number(m[1]);
  return out;
}

/* `options.mField = slot(N, ...)` and `= cfg[N]` out of the guest's decoder. */
function guestSlots(src) {
  const out = {};
  for (const m of src.matchAll(/options\.m(\w+)\s*=\s*slot\((\d+)\s*,/g))
    out[m[1]] = Number(m[2]);
  for (const m of src.matchAll(/options\.m(\w+)\s*=\s*static_cast<[^>]+>\(cfg\[(\d+)\]\)/g))
    out[m[1]] = Number(m[2]);
  // `options.mField = static_cast<T>(raw(N))` — the reader's other spelling.
  for (const m of src.matchAll(/options\.m(\w+)\s*=\s*static_cast<[^>]+>\(raw\((\d+)\)\)/g))
    out[m[1]] = Number(m[2]);
  return out;
}

// The one name each field goes by in each source. Only fields that actually
// cross the seam are listed: clockwork's own geometry (channel counts, block
// size, sample rate) travels as clockwork_init arguments and is deliberately absent
// from the native writer, so it is not part of this agreement.
const SHARED = [
  { guest: 'NumBuffers',            web: 'numBuffers',            native: 'NumBuffers' },
  { guest: 'MaxNodes',              web: 'maxNodes',              native: 'MaxNodes' },
  { guest: 'MaxGraphDefs',          web: 'maxGraphDefs',          native: 'MaxGraphDefs' },
  { guest: 'MaxWireBufs',           web: 'maxWireBufs',           native: 'MaxWireBufs' },
  { guest: 'NumAudioBusChannels',   web: 'numAudioBusChannels',   native: 'NumAudioBusChannels' },
  { guest: 'NumControlBusChannels', web: 'numControlBusChannels', native: 'NumControlBusChannels' },
  { guest: 'RealTimeMemorySize',    web: 'realTimeMemorySize',    native: 'RealTimeMemorySize' },
  { guest: 'NumRGens',              web: 'numRGens',              native: 'NumRGens' },
  { guest: 'LoadGraphDefs',         web: 'loadGraphDefs',         native: 'LoadGraphDefs' },
];

test.describe('guest config block contract', () => {
  const native = nativeSlots(nativeBlock);
  const web    = webSlots(webEncoder);
  const guest  = guestSlots(guestReader);

  // A parser that matched nothing would make every case below vacuously true,
  // which is the failure mode of the file this replaced: its regex went on
  // looking for an enum that had moved, found none, and half its assertions
  // were satisfied by an empty map.
  test('all three sources parsed', () => {
    expect(Object.keys(native).length).toBeGreaterThanOrEqual(SHARED.length);
    expect(Object.keys(web).length).toBeGreaterThanOrEqual(SHARED.length);
    expect(Object.keys(guest).length).toBeGreaterThanOrEqual(SHARED.length);
  });

  test('every shared field sits at the same slot in all three', () => {
    const disagreements = [];
    for (const f of SHARED) {
      const g = guest[f.guest], w = web[f.web], n = native[f.native];
      if (g === undefined || w === undefined || n === undefined
          || g !== w || g !== n) {
        disagreements.push(
          `${f.guest}: guest=${g} web=${w} native=${n}`);
      }
    }
    expect(disagreements).toEqual([]);
  });

  test('no two fields claim the same slot in one writer', () => {
    for (const [label, map] of [['native', native], ['web', web]]) {
      const seen = new Map();
      for (const [name, slot] of Object.entries(map)) {
        if (name === 'SlotCount') continue;   // a count, not a slot
        expect(seen.has(slot), `${label}: ${name} and ${seen.get(slot)} both claim slot ${slot}`)
          .toBe(false);
        seen.set(slot, name);
      }
    }
  });

  test('the block the guest reads fits the region reserved for it', () => {
    // GUEST_CONFIG_SIZE is a ceiling, not a shape — but a writer that ran past
    // it would be truncated by clockwork_init, and a truncated config is a different
    // config the guest cannot tell it was given.
    const shared = read('clockwork/src/shared_memory.h');
    const slots = Number(shared.match(/GUEST_CONFIG_SIZE\s*=\s*(\d+)\s*\*\s*sizeof\(uint32_t\)/)?.[1]);
    expect(slots).toBeGreaterThan(0);

    const highest = Math.max(
      ...Object.values(native), ...Object.values(web), ...Object.values(guest));
    expect(highest).toBeLessThan(slots);
    expect(native.SlotCount).toBeLessThanOrEqual(slots);
  });

  test('clockwork never reads a slot', () => {
    // The block is the guest's. A harness that indexes it has taken a position
    // on what one engine's numbers mean, which is what the demolition removed.
    const harnessSources = [
      'clockwork/src/audio_processor.cpp',
      'clockwork/src/lanes/lanes.cpp',
    ];
    for (const path of harnessSources) {
      const src = read(path);
      expect(src, `${path} indexes the guest config block`)
        .not.toMatch(/guestConfig\w*\[\s*\d+\s*\]|worldOptionsPtr\s*\[/);
    }
  });

  test('the RT pool offset side channel is gone', () => {
    // Slot 16 carried the address of an arena the client carved out of its own
    // guest region, which the engine read back through two file-scope externs
    // — the only allocation on any target that did not come from clockwork::mem.
    // Nothing may reintroduce it: a second way to hand the guest memory is a
    // second thing to keep in step with the first.
    expect(webEncoder).not.toMatch(/u32\[16\]\s*=/);
    expect(guestReader).not.toMatch(/g_rt_pool_ptr|g_rt_pool_size/);
    expect(read('dsp/scsynth/synth/server/SC_World.cpp'))
      .not.toMatch(/g_rt_pool_ptr|g_rt_pool_size/);
  });
});

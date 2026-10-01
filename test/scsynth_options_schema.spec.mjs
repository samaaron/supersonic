// The engine's options are listed ONCE, in dsp/scsynth/scsynth_options.h.
// The client cannot read a C header, so js/lib/scsynth_options_schema.js is
// generated from it and committed. This pins the two together, and pins the
// client's encoder to the text the guest parses: a schema that drifted from
// the header would let the client refuse a value the engine takes, or send a
// name the engine refuses — the two-writers-one-reader bug the positional
// block had, in a new coat.
//
// Source text, not a running engine, on purpose: a booted engine cannot tell
// you that two lists disagree, only that it got a number it was willing to
// use.
import { test, expect } from '@playwright/test';
import { readFileSync } from 'fs';
import { fileURLToPath } from 'url';
import { dirname, join } from 'path';

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..');
const { parseHeader, render } = await import(join(ROOT, 'scripts/gen-scsynth-options.mjs'));
const { scsynthOptionSchema } = await import(join(ROOT, 'js/lib/scsynth_options_schema.js'));
const { defaultScsynthOptions, encodeScsynthOptions, validateScsynthOptions, guestOptionNames } =
  await import(join(ROOT, 'js/scsynth_options.js'));

const header = readFileSync(join(ROOT, 'dsp/scsynth/scsynth_options.h'), 'utf8');
const fromHeader = parseHeader(header);

test.describe('scsynth options schema', () => {
  test('the header lists options', () => {
    expect(fromHeader.length).toBeGreaterThanOrEqual(8);
    for (const o of fromHeader) {
      expect(o.min).toBeLessThanOrEqual(o.default);
      expect(o.default).toBeLessThanOrEqual(o.max);
    }
  });

  test('the generated module is what the header says (run scripts/gen-scsynth-options.mjs)', () => {
    const committed = readFileSync(join(ROOT, 'js/lib/scsynth_options_schema.js'), 'utf8');
    expect(committed).toBe(render(fromHeader));
    expect(scsynthOptionSchema.map((o) => o.name)).toEqual(fromHeader.map((o) => o.name));
  });

  test('the client defaults to the header\'s defaults', () => {
    for (const o of fromHeader) expect(defaultScsynthOptions[o.name]).toBe(o.default);
    expect([...guestOptionNames]).toEqual(fromHeader.map((o) => o.name));
  });

  test('the encoder sends every option, by name, as the guest reads it', () => {
    const text = new TextDecoder().decode(new Uint8Array(encodeScsynthOptions(defaultScsynthOptions)));
    expect(text.endsWith('\n\0')).toBe(true);
    const lines = text.slice(0, -1).trimEnd().split('\n');
    expect(lines).toEqual(fromHeader.map((o) => `${o.name}=${o.default}`));
    // Nothing of the host's own leaks into the guest's block. By line, not by
    // substring: `realTime` is the head of `realTimeMemorySize`, which is the
    // guest's.
    const sent = new Set(lines.map((l) => l.slice(0, l.indexOf('='))));
    for (const hostOnly of ['numInputBusChannels', 'numOutputBusChannels', 'bufLength',
                            'realTime', 'memoryLocking', 'preferredSampleRate']) {
      expect(sent.has(hostOnly)).toBe(false);
    }
  });

  test('the client refuses what the header\'s ranges refuse, naming the option', () => {
    for (const o of fromHeader) {
      const below = { ...defaultScsynthOptions, [o.name]: o.min - 1 };
      expect(() => validateScsynthOptions(below)).toThrow(o.name);
      const above = { ...defaultScsynthOptions, [o.name]: o.max + 1 };
      expect(() => validateScsynthOptions(above)).toThrow(o.name);
      const ok = { ...defaultScsynthOptions, [o.name]: o.max };
      expect(() => validateScsynthOptions(ok)).not.toThrow();
    }
  });

  test('the native host honours the same flags the header names', () => {
    // host/EngineHost.cpp generates its scsynth-shaped flags from the header;
    // it must therefore not spell any of them by hand any more.
    const host = readFileSync(join(ROOT, 'host/EngineHost.cpp'), 'utf8');
    for (const o of fromHeader) {
      if (!o.flag) continue;
      expect(host, `EngineHost.cpp names -${o.flag} by hand`).not.toMatch(new RegExp(`case '${o.flag}':`));
    }
    expect(host).toContain('scsynthOptionForFlag');
  });
});

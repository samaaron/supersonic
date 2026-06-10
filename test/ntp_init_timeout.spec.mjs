// init() awaits NTPTiming.initialize(), which polls the AudioContext for
// contextTime > 0 ("audio is flowing"). An autoplay-blocked or suspended context
// never advances contextTime, and the poll was an unbounded `while (true)` — so
// init() would hang forever with no error. initialize() must bound the wait and
// reject so the caller gets an actionable failure instead of a wedge.
//
// Node-level: a mock AudioContext whose getOutputTimestamp() never advances makes
// this deterministic (no browser, no real audio).
import { test, expect } from '@playwright/test';
import { NTPTiming } from '../js/lib/ntp_timing.js';

test('initialize() rejects (does not hang) when the AudioContext never starts', async () => {
  test.setTimeout(8000);

  // contextTime stuck at 0 forever — autoplay-blocked / suspended context.
  const stuckContext = {
    getOutputTimestamp: () => ({ contextTime: 0, performanceTime: 0 }),
  };
  const ntp = new NTPTiming({
    mode: 'sab',
    audioContext: stuckContext,
    audioStartTimeoutMs: 200, // short bound for the test; prod default is seconds
  });

  let settled = false;
  let rejected = false;
  const p = ntp.initialize().then(
    () => { settled = true; },
    () => { settled = true; rejected = true; },
  );

  // Give it generously longer than the bound; it must settle well before this.
  await Promise.race([p, new Promise((r) => setTimeout(r, 3000))]);

  expect(settled).toBe(true);   // must not hang
  expect(rejected).toBe(true);  // and must reject, not silently resolve
});

test('initialize() returns immediately when there is no AudioContext', async () => {
  // Existing early-out: no context → resolve (nothing to time).
  const ntp = new NTPTiming({ mode: 'sab', audioContext: undefined });
  await expect(ntp.initialize()).resolves.toBeUndefined();
});

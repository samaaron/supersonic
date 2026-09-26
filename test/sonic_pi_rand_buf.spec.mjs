// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron

/**
 * Sonic Pi's coin-tossing FX and the buffer they toss with
 *
 * slicer, panslicer and wobble take probability: — the chance each slice
 * (each pan, each wobble) keeps its shape rather than holding at prob_pos.
 * The coin is not a UGen's noise but a buffer of values in 0..1, read one
 * per slice from seed on (fx.clj buffered-coin-gate):
 *
 *   keep = BufRd.kr(1, rand_buf, seed + PulseCount(slice), 1) < probability
 *
 * so the same seed plays the same pattern, run after run. rand_buf defaults
 * to 0. Sonic Pi loads etc/buffers/rand-stream.wav at boot and passes its
 * number to these FX (synthinfo.rb on_start); SuperSonic loads no such
 * buffer, so a caller that leaves rand_buf out tosses with whatever buffer 0
 * holds: nothing (each read 0, every slice kept, probability: ignored) or
 * someone's sample.
 *
 * Each FX runs over a saw at 25 slices a second, a second captured, and the
 * coin buffer filled with constants rather than noise so what is kept is
 * exact: all 0.9 (probability: 0.5 keeps none), all 0.1 (keeps all),
 * alternating (keeps every other one).
 */

import { test, expect, skipIfPostMessage } from "./fixtures.mjs";

// Heard only by the capture: a second of saw per test, kept off the speakers
test.use({
  launchOptions: {
    args: ["--use-fake-ui-for-media-stream", "--use-fake-device-for-media-stream",
      "--autoplay-policy=no-user-gesture-required", "--mute-audio"],
  },
});

test.beforeEach(async ({ sonicMode }) => {
  skipIfPostMessage(sonicMode, "Audio capture requires SAB mode");
});

const COIN = 900;        // the coin buffer's number: well clear of 0
const PHASE = 0.04;      // s: 25 slices in the second the capture holds
const BLOCK = 240;       // frames: 5ms, a quarter of a slice's on half

const constant = (v) => Array(64).fill(v);
const alternating = Array.from({ length: 64 }, (_, i) => (i % 2 ? 0.9 : 0.1));

// One FX over a saw, a second of it captured. buffers: { bufnum: values }
// written before it starts. Per 5ms block from the first sound: the level of
// each side, and how bright it is (the first difference's level over the
// level: a saw through a low cutoff is dull, through a high one bright).
async function play(page, config, fx, params, buffers = {}) {
  return await page.evaluate(async ([config, fx, params, buffers, BLOCK]) => {
    const sonic = new window.SuperSonic(config);
    await sonic.init();
    await sonic.loadSynthDef("sonic-pi-saw");
    await sonic.loadSynthDef(`sonic-pi-fx_${fx}`);
    for (const [bufnum, values] of Object.entries(buffers)) {
      await sonic.send("/b_alloc", +bufnum, values.length, 1);
      await sonic.sync();
      await sonic.send("/b_setn", +bufnum, 0, values.length, ...values);
    }
    await sonic.sync();

    sonic.startCapture();
    await sonic.send("/s_new", "sonic-pi-saw", 2000, 0, 0,
      "note", 48, "amp", 0.5, "attack", 0, "sustain", 2, "release", 0, "out_bus", 16);
    await sonic.send("/s_new", `sonic-pi-fx_${fx}`, 2001, 1, 0,
      "in_bus", 16, "out_bus", 0, ...Object.entries(params).flat());
    await new Promise((r) => setTimeout(r, 1000));
    const cap = sonic.stopCapture();
    await sonic.send("/n_free", 2000, 2001);
    await sonic.destroy();

    const L = cap.left, R = cap.right;
    let from = 0;
    while (from < cap.frames && L[from] === 0 && R[from] === 0) from++;
    if (from === cap.frames) from = 0;   // silent throughout: every slice held at prob_pos, the slicer's silence
    const blocks = [];
    for (let i = from; i + BLOCK <= cap.frames; i += BLOCK) {
      let l = 0, r = 0, d = 0;
      for (let j = i; j < i + BLOCK; j++) {
        l += L[j] * L[j]; r += R[j] * R[j];
        const m = (L[j] + R[j]) / 2, prev = j > 0 ? (L[j - 1] + R[j - 1]) / 2 : m;
        d += (m - prev) * (m - prev);
      }
      const level = Math.sqrt((l + r) / (2 * BLOCK));
      blocks.push({ l: Math.sqrt(l / BLOCK), r: Math.sqrt(r / BLOCK), bright: level ? Math.sqrt(d / BLOCK) / level : 0 });
    }
    return { blocks, frames: cap.frames };
  }, [config, fx, params, buffers, BLOCK]);
}

// The share of blocks in a slice's on state: sounding (slicer), panned right
// (panslicer), bright (wobble, against a threshold between its two states).
const share = (blocks, on) => blocks.filter(on).length / blocks.length;
const sounding = (blocks) => {
  const top = Math.max(...blocks.map((b) => Math.max(b.l, b.r)));
  return (b) => Math.max(b.l, b.r) > top * 0.1;
};
const right = () => (b) => b.r > b.l;

const FX = {
  slicer: { params: { phase: PHASE, wave: 1 }, on: sounding },
  panslicer: { params: { phase: PHASE, wave: 1 }, on: right },
  wobble: { params: { phase: PHASE, wave: 1, cutoff_min: 50, cutoff_max: 120, res: 0 } },
};

for (const [fx, { params, on }] of Object.entries(FX)) {
  test.describe(`sonic-pi-fx_${fx} probability:`, () => {
    // wobble's on state is bright; the line between bright and dull is drawn
    // from its run without a coin (half of each), and held for the others
    let isOn = on;
    let baseline;

    test.beforeEach(async ({ page, sonicConfig }) => {
      await page.goto("/test/harness.html");
      if (baseline) return;
      const { blocks } = await play(page, sonicConfig, fx, params);
      if (!isOn) {
        const b = blocks.map((x) => x.bright).sort((a, c) => a - c);
        const line = Math.sqrt(b[Math.floor(b.length * 0.1)] * b[Math.floor(b.length * 0.9)]);
        isOn = () => (x) => x.bright > line;
      }
      baseline = share(blocks, isOn(blocks));
    });

    const run = async (page, sonicConfig, more, buffers) => {
      const { blocks } = await play(page, sonicConfig, fx, { ...params, ...more }, buffers);
      return share(blocks, isOn(blocks));
    };

    test("without a coin, a square wave is on half the time", async () => {
      expect(baseline).toBeGreaterThan(0.35);
      expect(baseline).toBeLessThan(0.65);
    });

    test("tosses with rand_buf: all 0.9 keeps no slice", async ({ page, sonicConfig }) => {
      const s = await run(page, sonicConfig, { probability: 0.5, rand_buf: COIN }, { [COIN]: constant(0.9) });
      expect(s).toBeLessThan(0.08);
    });

    test("tosses with rand_buf: all 0.1 keeps every slice", async ({ page, sonicConfig }) => {
      const s = await run(page, sonicConfig, { probability: 0.5, rand_buf: COIN }, { [COIN]: constant(0.1) });
      expect(Math.abs(s - baseline)).toBeLessThan(0.1);
    });

    test("tosses with rand_buf: alternating keeps every other slice", async ({ page, sonicConfig }) => {
      const s = await run(page, sonicConfig, { probability: 0.5, rand_buf: COIN }, { [COIN]: alternating });
      expect(Math.abs(s - baseline / 2)).toBeLessThan(0.1);
    });

    test("probability: 0 tosses no coin, whatever rand_buf holds", async ({ page, sonicConfig }) => {
      const s = await run(page, sonicConfig, { probability: 0, rand_buf: COIN }, { [COIN]: constant(0.9) });
      expect(Math.abs(s - baseline)).toBeLessThan(0.1);
    });

    // rand_buf left out: the coin is buffer 0. SuperSonic puts nothing there,
    // so every read is 0, below any probability: every slice is kept, and the
    // opt does nothing. A caller wanting it must load a coin buffer and pass it.
    test("without rand_buf and nothing in buffer 0, probability: does nothing", async ({ page, sonicConfig }) => {
      const s = await run(page, sonicConfig, { probability: 0.1 });
      expect(Math.abs(s - baseline)).toBeLessThan(0.1);
    });

    test("without rand_buf, the coin is whatever buffer 0 holds", async ({ page, sonicConfig }) => {
      const s = await run(page, sonicConfig, { probability: 0.5 }, { 0: constant(0.9) });
      expect(s).toBeLessThan(0.08);
    });
  });
}

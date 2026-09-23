// SPDX-License-Identifier: MIT OR GPL-3.0-or-later
// Copyright (c) 2026 Sam Aaron
/*
 * Samples across the engine's lifecycle.
 *
 * The buffer manager is built once, against the engine it first meets: its audio context (decoding, the default
 * sample rate) and its memory (where the pool lives). reload() makes a new context when the engine made the old one;
 * reset() makes a new engine altogether. A manager still holding the old ones decodes through a closed context, or
 * lists buffers the engine it now talks to was never given.
 */
import { test, expect } from "./fixtures.mjs";

test.beforeEach(async ({ page }) => {
  await page.goto("/test/harness.html");
  await page.waitForFunction(() => window.supersonicReady === true, { timeout: 10000 });
});

test("a sample loads after a reload that made a new audio context", async ({ page, sonicConfig }) => {
  const r = await page.evaluate(async (config) => {
    const sonic = new window.SuperSonic(config);
    await sonic.init();
    const before = sonic.audioContext;
    await sonic.loadSample(1, "bd_haus.flac");
    const reloaded = await sonic.reload();
    const out = { reloaded, newContext: sonic.audioContext !== before, oldState: before.state };
    try { await sonic.loadSample(2, "sn_dub.flac"); await sonic.sync(); out.loaded = true; }
    catch (e) { out.loaded = false; out.error = String(e?.message ?? e); }
    out.buffers = sonic.getLoadedBuffers().map((b) => b.bufnum).sort();
    await sonic.destroy();
    return out;
  }, sonicConfig);
  expect(r.reloaded).toBe(true);
  expect(r.error ?? null).toBeNull();
  expect(r.loaded).toBe(true);
  expect(r.buffers).toEqual([1, 2]);
});

test("reset() forgets the buffers the old engine had, and a sample loads into the new one", async ({ page, sonicConfig }) => {
  const r = await page.evaluate(async (config) => {
    const sonic = new window.SuperSonic(config);
    await sonic.init();
    await sonic.loadSample(1, "bd_haus.flac");
    await sonic.reset();
    const out = { afterReset: sonic.getLoadedBuffers().map((b) => b.bufnum) };
    try { await sonic.loadSample(3, "sn_dub.flac"); await sonic.sync(); out.loaded = true; }
    catch (e) { out.loaded = false; out.error = String(e?.message ?? e); }
    out.buffers = sonic.getLoadedBuffers().map((b) => b.bufnum).sort();
    await sonic.destroy();
    return out;
  }, sonicConfig);
  expect(r.afterReset, "buffers the engine that came back was never given").toEqual([]);
  expect(r.error ?? null).toBeNull();
  expect(r.loaded).toBe(true);
  expect(r.buffers).toEqual([3]);
});

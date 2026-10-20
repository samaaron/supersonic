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

/*
 * A reload is audio coming back, and the engine does the least that gets it back where it was (clockwork's
 * docs/RESTARTS.md). Over the memory it ran in (SAB) the instance carries on when nothing it was built for changed:
 * every synth, definition and sample still there, nothing sent again. Onto a rate scsynth cannot take, a new instance
 * is built: the engine hands it every sample the last one held, and the client puts the definitions back. Into new
 * memory (postMessage, whose worklet took its memory with it) everything is put back from the client's own record.
 */

// One frame of a buffer, as the engine reads it: what came back is the sample, not just a buffer of the right shape.
const frameOf = `async (sonic, bufnum, frame) => {
  const m = await sonic.request("/b_get", [bufnum, frame], { reply: "/b_set", match: (r) => r[1] === bufnum, timeoutMs: 5000 });
  return m[3];
}`;

test("a reload over the same memory keeps the synths that were playing, and sends nothing again", async ({ page, sonicConfig }) => {
  test.skip(sonicConfig.mode !== "sab", "a reload keeps the memory only in SAB mode");
  const r = await page.evaluate(async (config) => {
    const sonic = new window.SuperSonic(config);
    await sonic.init();
    await sonic.loadSynthDef("sonic-pi-beep");
    // Registered for node notices once, before: an instance that carried on still has the registration.
    await sonic.request("/notify", [1], { reply: "/done", match: (m) => m[1] === "/notify" });
    sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 0, "amp", 0, "sustain", 1000);
    await sonic.sync();
    const sent = [];
    sonic.on("out", (m) => sent.push(m[0]));
    let resumed = null;
    sonic.on("reload:complete", (d) => { resumed = d?.resumed ?? null; });
    const reloaded = await sonic.reload();
    const out = { reloaded, resumed, sent: sent.filter((a) => a === "/d_recv" || a === "/clockwork/asset/commit") };
    try { out.node = await sonic.request("/n_query", [1000], { reply: "/n_info", match: (m) => m[1] === 1000, timeoutMs: 5000 }); }
    catch (e) { out.error = String(e?.message ?? e); }
    await sonic.destroy();
    return out;
  }, sonicConfig);
  expect(r.reloaded).toBe(true);
  expect(r.resumed).toBe(true);
  expect(r.error ?? null).toBeNull();
  expect(r.node?.[1]).toBe(1000);
  expect(r.sent, "definitions or samples sent again to an instance that has them").toEqual([]);
});

test("a reload onto a rate the engine cannot take keeps the samples it held, and the definitions come back", async ({ page, sonicConfig }) => {
  test.skip(sonicConfig.mode !== "sab", "a reload keeps the memory only in SAB mode");
  const r = await page.evaluate(async ([config, frameSrc]) => {
    const frame = eval(frameSrc);
    const sonic = new window.SuperSonic(config);
    await sonic.init();
    await sonic.loadSynthDef("sonic-pi-beep");
    await sonic.loadSample(1, "bd_haus.flac");
    await sonic.sync();
    const before = await frame(sonic, 1, 2000);
    let resumed = null;
    sonic.on("reload:complete", (d) => { resumed = d?.resumed ?? null; });
    const other = sonic.audioContext.sampleRate === 44100 ? 48000 : 44100;
    const reloaded = await sonic.reload({ audioContext: new AudioContext({ sampleRate: other }) });
    const out = { reloaded, resumed, before, rate: sonic.audioContext.sampleRate, other };
    try {
      out.after = await frame(sonic, 1, 2000);
      // A new instance's notify list starts empty: a client registers again, as on setup.
      await sonic.request("/notify", [1], { reply: "/done", match: (m) => m[1] === "/notify" });
      out.def = await sonic.request("/s_new", ["sonic-pi-beep", 1001, 0, 0, "amp", 0],
                                    { reply: "/n_go", match: (m) => m[1] === 1001, timeoutMs: 5000 });
    } catch (e) { out.error = String(e?.message ?? e); }
    out.buffers = sonic.getLoadedBuffers().map((b) => b.bufnum);
    await sonic.destroy();
    return out;
  }, [sonicConfig, frameOf]);
  expect(r.reloaded).toBe(true);
  expect(r.rate).toBe(r.other);
  expect(r.resumed).toBe(false);
  expect(r.error ?? null).toBeNull();
  expect(r.before).not.toBe(0);
  expect(r.after, "the sample the new instance was handed").toBe(r.before);
  expect(r.buffers).toEqual([1]);
});

test("a sample loaded from bytes comes back after a reload", async ({ page, sonicConfig }) => {
  const r = await page.evaluate(async ([config, frameSrc]) => {
    const frame = eval(frameSrc);
    const sonic = new window.SuperSonic(config);
    await sonic.init();
    const bytes = await (await fetch(new URL("bd_haus.flac", new URL(config.sampleBaseURL, location.href)))).arrayBuffer();
    await sonic.loadSample(3, bytes);
    await sonic.sync();
    const before = await frame(sonic, 3, 2000);
    const reloaded = await sonic.reload();
    const out = { reloaded, before };
    try { out.after = await frame(sonic, 3, 2000); } catch (e) { out.error = String(e?.message ?? e); }
    await sonic.destroy();
    return out;
  }, [sonicConfig, frameOf]);
  expect(r.reloaded).toBe(true);
  expect(r.error ?? null).toBeNull();
  expect(r.before).not.toBe(0);
  expect(r.after).toBe(r.before);
});

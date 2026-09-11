// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * /supersonic/piano/wavetable on the web: the same verb, fed the same way a
 * sample is. The table is loaded as a buffer through loadSample — a decoded
 * blob in the inbox, /b_allocPtr — and the verb points MdaPiano at it. On the
 * SAB transport the output is captured to show the piano silent before and
 * sounding after.
 */
import { test, expect } from "./fixtures.mjs";

// A 16-bit mono WAV of `frames` frames, built in the page: the table need
// only be long enough, and a sine gives a played piano something to sound.
const MAKE_WAV = `(frames) => {
  const rate = 44100, bytes = 44 + frames * 2;
  const buf = new ArrayBuffer(bytes), v = new DataView(buf);
  const str = (o, s) => { for (let i = 0; i < s.length; i++) v.setUint8(o + i, s.charCodeAt(i)); };
  str(0, "RIFF"); v.setUint32(4, bytes - 8, true); str(8, "WAVE");
  str(12, "fmt "); v.setUint32(16, 16, true); v.setUint16(20, 1, true); v.setUint16(22, 1, true);
  v.setUint32(24, rate, true); v.setUint32(28, rate * 2, true); v.setUint16(32, 2, true); v.setUint16(34, 16, true);
  str(36, "data"); v.setUint32(40, frames * 2, true);
  for (let i = 0; i < frames; i++) v.setInt16(44 + i * 2, Math.round(16000 * Math.sin((i % 100) / 100 * 2 * Math.PI)), true);
  return buf;
}`;

test("the table is loaded like a sample and the verb answers /done", async ({ page, sonicConfig }) => {
  await page.goto("/test/harness.html");
  const r = await page.evaluate(async ({ config, makeWav }) => {
    const sonic = new window.SuperSonic(config);
    await sonic.init();
    const replies = [];
    sonic.on("in", (m) => replies.push(m));
    const waitFor = (addr, pred) => new Promise((resolve, reject) => {
      const t = setTimeout(() => reject(new Error(`no ${addr}`)), 5000);
      const h = (m) => { if (m[0] === addr && pred(m)) { clearTimeout(t); sonic.off("in", h); resolve(m); } };
      sonic.on("in", h);
    });
    const wav = (0, eval)(makeWav)(586348 + 2);
    await sonic.loadSample(30, wav);

    const done = waitFor("/done", (m) => m[1] === "/supersonic/piano/wavetable");
    sonic.send("/supersonic/piano/wavetable", 30);
    await done;

    // Too short: refused.
    const fail = waitFor("/fail", (m) => m[1] === "/supersonic/piano/wavetable");
    // The client library rewrites /b_alloc into /b_allocPtr over the inbox,
    // so the /done names that verb.
    sonic.send("/b_alloc", 31, 1000, 1);
    await waitFor("/done", (m) => m[2] === 31);
    sonic.send("/supersonic/piano/wavetable", 31);
    const f = await fail;
    return { failText: f[2] };
  }, { config: sonicConfig, makeWav: MAKE_WAV });
  expect(r.failText).toContain("shorter");
});

test("the piano is silent without a table and sounds with one", async ({ page, sonicConfig }) => {
  test.skip(sonicConfig.mode !== "sab", "audio capture is SAB-only");
  await page.goto("/test/harness.html");
  const r = await page.evaluate(async ({ config, makeWav }) => {
    const sonic = new window.SuperSonic(config);
    await sonic.init();
    await sonic.loadSynthDefs(["sonic-pi-piano"]);
    const wait = (ms) => new Promise((r) => setTimeout(r, ms));
    const peak = (cap) => { let m = 0; for (const x of cap.left) m = Math.max(m, Math.abs(x)); return m; };
    const play = async () => {
      sonic.startCapture();
      sonic.send("/s_new", "sonic-pi-piano", -1, 0, 0, "note", 60, "amp", 1);
      await wait(400);
      const cap = sonic.stopCapture();
      sonic.send("/g_freeAll", 0);
      await wait(100);
      return peak(cap);
    };
    sonic.send("/supersonic/piano/wavetable", -1);
    await wait(100);
    const silent = await play();
    const wav = (0, eval)(makeWav)(586348 + 2);
    await sonic.loadSample(30, wav);
    sonic.send("/supersonic/piano/wavetable", 30);
    await wait(200);
    const sounding = await play();
    return { silent, sounding };
  }, { config: sonicConfig, makeWav: MAKE_WAV });
  expect(r.silent).toBe(0);
  expect(r.sounding).toBeGreaterThan(0.01);
});

/**
 * UGen Unit Test: EnvGen squared/cubed shapes with negative levels
 * (sonic-pi#169)
 *
 * The \sqr and \cub envelope warps are computed in root-space:
 * classic scsynth takes sqrt/pow(1/3) of the segment levels, so any
 * negative level (e.g. a pan slide from -1) produced NaN and the
 * segment never moved. The signed forms keep non-negative envelopes
 * bit-identical and make negative ranges work.
 *
 * Probes (compiled by test/synthdefs/compile_envgen_shape_synthdefs.scd)
 * write the envelope value straight to the output bus:
 *   envgen_sqr_probe:     Env([-1, 1], [0.25], \sqr)
 *   envgen_cub_probe:     Env([-1, 1], [0.25], \cub)
 *   envgen_sqr_pos_probe: Env([0, 1],  [0.25], \sqr)  (legacy guard)
 */

import { test, expect, skipIfPostMessage } from "./fixtures.mjs";

test.beforeEach(async ({ sonicMode }) => {
  skipIfPostMessage(sonicMode, "Audio capture requires SAB mode");
});

async function captureProbe(page, sonicConfig, defName) {
  return await page.evaluate(async ([config, name]) => {
    const sonic = new window.SuperSonic(config);
    await sonic.init();
    const r = await fetch(`/test/synthdefs/${name}.scsyndef`);
    await sonic.loadSynthDef(new Uint8Array(await r.arrayBuffer()));
    await sonic.sync();

    sonic.startCapture();
    await sonic.send("/s_new", name, 9200, 0, 0);
    await new Promise((res) => setTimeout(res, 600));
    const cap = sonic.stopCapture();
    await sonic.send("/n_free", 9200);
    await sonic.destroy();

    const L = Array.from(cap.left);
    const sr = cap.sampleRate;
    const nan = L.some((v) => Number.isNaN(v));
    // The envelope runs for 0.25s from node start; the tail of the
    // capture is firmly post-envelope, so its mean is the end level.
    const tail = L.slice(Math.floor(0.4 * sr));
    const tailMean = tail.reduce((a, b) => a + b, 0) / tail.length;
    const minVal = Math.min(...L);
    // A real 0.25s ramp spends thousands of samples in each mid window.
    // The broken warps NaN or clamp mid-segment then jump to the end
    // level, and K2A's inter-block interpolation only sweeps ~64 samples
    // through each window on the jumps, so counts (not booleans) are the
    // discriminator.
    const midNeg = L.filter((v) => v > -0.7 && v < -0.2).length;
    const midPos = L.filter((v) => v > 0.2 && v < 0.7).length;
    return { nan, tailMean, minVal, midNeg, midPos, sampleRate: sr, len: L.length };
  }, [sonicConfig, defName]);
}

test.describe("EnvGen signed squared/cubed shapes", () => {
  test("\\sqr envelope slides -1 -> 1 without NaN", async ({ page, sonicConfig }) => {
    await page.goto("/test/harness.html");
    const r = await captureProbe(page, sonicConfig, "envgen_sqr_probe");
    console.log("sqr probe:", JSON.stringify(r));
    expect(r.nan, "no NaN samples").toBe(false);
    expect(r.minVal, "starts from the negative level").toBeLessThan(-0.8);
    expect(r.midNeg, "ramps through the negative range").toBeGreaterThan(500);
    expect(r.midPos, "ramps through the positive range").toBeGreaterThan(500);
    expect(r.tailMean, "arrives at the end level").toBeGreaterThan(0.9);
  });

  test("\\cub envelope slides -1 -> 1 without NaN", async ({ page, sonicConfig }) => {
    await page.goto("/test/harness.html");
    const r = await captureProbe(page, sonicConfig, "envgen_cub_probe");
    console.log("cub probe:", JSON.stringify(r));
    expect(r.nan, "no NaN samples").toBe(false);
    expect(r.minVal, "starts from the negative level").toBeLessThan(-0.8);
    expect(r.midNeg, "ramps through the negative range").toBeGreaterThan(500);
    expect(r.midPos, "ramps through the positive range").toBeGreaterThan(500);
    expect(r.tailMean, "arrives at the end level").toBeGreaterThan(0.9);
  });

  test("\\sqr envelope 0 -> 1 unchanged (legacy guard)", async ({ page, sonicConfig }) => {
    await page.goto("/test/harness.html");
    const r = await captureProbe(page, sonicConfig, "envgen_sqr_pos_probe");
    console.log("sqr pos probe:", JSON.stringify(r));
    expect(r.nan, "no NaN samples").toBe(false);
    expect(r.minVal, "never dips below start level").toBeGreaterThan(-0.05);
    expect(r.midPos, "ramps through the positive range").toBeGreaterThan(500);
    expect(r.tailMean, "arrives at the end level").toBeGreaterThan(0.9);
  });
});

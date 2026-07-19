/**
 * UGen Unit Test: EnvGen exponential shape with zero endpoints
 * (sonic-pi#881)
 *
 * The \exp warp computes grow = pow(end/start, 1/n) then level *= grow,
 * so a zero start level sticks the whole segment at silence and the
 * level jumps at the segment boundary — the "click instead of synth"
 * from sonic-pi#881 (every amp envelope starts and ends at 0).
 * The fix substitutes a tiny epsilon for zero endpoints so the segment
 * ramps from/to -120dB instead; non-zero envelopes are untouched.
 *
 * Probes (compiled by test/synthdefs/compile_envgen_exp_synthdefs.scd):
 *   envgen_exp_probe:     Env([0, 1, 0], [0.25, 0.25], \exp)
 *   envgen_exp_pos_probe: Env([0.001, 1], [0.25], \exp)  (legacy guard)
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
    await sonic.send("/s_new", name, 9300, 0, 0);
    await new Promise((res) => setTimeout(res, 800));
    const cap = sonic.stopCapture();
    await sonic.send("/n_free", 9300);
    await sonic.destroy();

    const L = Array.from(cap.left);
    const sr = cap.sampleRate;
    const nan = L.some((v) => Number.isNaN(v));
    const maxVal = Math.max(...L);
    const minVal = Math.min(...L);
    // A real exponential ramp spends thousands of samples between the
    // endpoints; the broken zero-endpoint case sits at 0 then jumps, so
    // only K2A's ~64-sample inter-block interpolation lands in the
    // window. Counts, not booleans (K2A lesson from the #169 spec).
    const mid = L.filter((v) => v > 0.1 && v < 0.9).length;
    const tail = L.slice(Math.floor(0.65 * sr));
    const tailMean = tail.reduce((a, b) => a + b, 0) / tail.length;
    return { nan, maxVal, minVal, mid, tailMean, sampleRate: sr, len: L.length };
  }, [sonicConfig, defName]);
}

test.describe("EnvGen exponential shape with zero endpoints", () => {
  test("\\exp amp-style envelope 0 -> 1 -> 0 ramps instead of clicking", async ({ page, sonicConfig }) => {
    await page.goto("/test/harness.html");
    const r = await captureProbe(page, sonicConfig, "envgen_exp_probe");
    console.log("exp probe:", JSON.stringify(r));
    expect(r.nan, "no NaN samples").toBe(false);
    expect(r.maxVal, "reaches the peak").toBeGreaterThan(0.9);
    expect(r.mid, "ramps through intermediate levels").toBeGreaterThan(500);
    expect(r.tailMean, "returns to silence").toBeLessThan(0.05);
    expect(r.minVal, "never goes negative").toBeGreaterThan(-0.001);
  });

  test("\\exp envelope with non-zero endpoints unchanged (legacy guard)", async ({ page, sonicConfig }) => {
    await page.goto("/test/harness.html");
    const r = await captureProbe(page, sonicConfig, "envgen_exp_pos_probe");
    console.log("exp pos probe:", JSON.stringify(r));
    expect(r.nan, "no NaN samples").toBe(false);
    expect(r.mid, "exponential rise dwell time").toBeGreaterThan(2000);
    expect(r.mid, "exponential rise dwell time (upper band)").toBeLessThan(5000);
    expect(r.tailMean, "arrives at the end level").toBeGreaterThan(0.9);
  });
});

/**
 * UGen Unit Test: Clip and RLPF behavior
 *
 * Tests whether the Clip UGen correctly handles inverted bounds (lo > hi),
 * and whether the RLPF's resonance parameter (rq) actually affects output.
 *
 * Uses sonic-pi-tech_saws which has:
 *   res.linlin(1, 0, 0, 1)  →  Clip(res, lo=1.0, hi=0.0)  →  1.0 - result  →  rq
 *
 * If Clip(x, 1.0, 0.0) always returns 1.0, then rq is always 0.0,
 * meaning RLPF runs with maximum resonance regardless of the res parameter.
 *
 * Test approach:
 *   1. Create tech_saws at cutoff=100 with different res values
 *   2. Capture audio and measure energy at the resonance frequency (2637Hz)
 *   3. If res has no effect on resonance energy → Clip is broken
 *   4. Compare L vs R channels for asymmetric filtering
 */

import { test, expect, skipIfPostMessage } from "./fixtures.mjs";

test.beforeEach(async ({ sonicMode }) => {
  skipIfPostMessage(sonicMode, "Audio capture requires SAB mode");
});

// Inline spectral analysis - measures energy in frequency bands
const SPECTRAL_HELPERS = `
function bandEnergy(samples, sampleRate, fLo, fHi) {
  const N = 4096;
  if (samples.length < N) return 0;
  // Skip first 0.1s transient
  const skip = Math.floor(0.1 * sampleRate);
  const sig = samples.slice(skip);
  if (sig.length < N) return 0;

  const hop = N >> 1;
  const nWindows = Math.max(1, Math.floor((sig.length - N) / hop));
  const spectrum = new Float64Array(N / 2 + 1);

  for (let w = 0; w < nWindows; w++) {
    const start = w * hop;
    const re = new Float64Array(N);
    const im = new Float64Array(N);
    for (let i = 0; i < N; i++) {
      re[i] = sig[start + i] * (0.5 - 0.5 * Math.cos(2 * Math.PI * i / N));
    }
    // Radix-2 FFT
    for (let i = 1, j = 0; i < N; i++) {
      let bit = N >> 1;
      for (; j & bit; bit >>= 1) j ^= bit;
      j ^= bit;
      if (i < j) { [re[i], re[j]] = [re[j], re[i]]; [im[i], im[j]] = [im[j], im[i]]; }
    }
    for (let len = 2; len <= N; len <<= 1) {
      const half = len >> 1;
      const ang = -2 * Math.PI / len;
      const wRe = Math.cos(ang), wIm = Math.sin(ang);
      for (let i = 0; i < N; i += len) {
        let cRe = 1, cIm = 0;
        for (let j = 0; j < half; j++) {
          const uRe = re[i+j], uIm = im[i+j];
          const vRe = re[i+j+half]*cRe - im[i+j+half]*cIm;
          const vIm = re[i+j+half]*cIm + im[i+j+half]*cRe;
          re[i+j] = uRe+vRe; im[i+j] = uIm+vIm;
          re[i+j+half] = uRe-vRe; im[i+j+half] = uIm-vIm;
          const t = cRe*wRe - cIm*wIm; cIm = cRe*wIm + cIm*wRe; cRe = t;
        }
      }
    }
    for (let i = 0; i <= N/2; i++) {
      spectrum[i] += Math.sqrt(re[i]*re[i] + im[i]*im[i]);
    }
  }
  for (let i = 0; i <= N/2; i++) spectrum[i] /= nWindows;

  const binHz = sampleRate / N;
  const iLo = Math.max(1, Math.floor(fLo / binHz));
  const iHi = Math.min(N/2, Math.ceil(fHi / binHz));
  let sum = 0, count = 0;
  for (let i = iLo; i <= iHi; i++) { sum += spectrum[i] * spectrum[i]; count++; }
  return count > 0 ? Math.sqrt(sum / count) : 0;
}

function rms(samples, startSec, endSec, sampleRate) {
  const s = Math.floor(startSec * sampleRate);
  const e = Math.min(Math.floor(endSec * sampleRate), samples.length);
  let sum = 0;
  for (let i = s; i < e; i++) sum += samples[i] * samples[i];
  return Math.sqrt(sum / (e - s));
}
`;

test.describe("Clip UGen with inverted bounds", () => {

  test("res parameter should affect RLPF resonance in tech_saws", async ({ page, sonicConfig }) => {
    await page.goto("/test/harness.html");
    const result = await page.evaluate(async ([config, helpers]) => {
      eval(helpers);

      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-tech_saws");
      await sonic.sync();

      const captures = {};
      let nodeId = 9000;

      // Test with different res values — if Clip works correctly,
      // res=0.01 should give wide bandwidth (low resonance)
      // res=0.99 should give narrow bandwidth (high resonance)
      for (const resVal of [0.01, 0.3, 0.7, 0.99]) {
        sonic.startCapture();
        await sonic.send("/s_new", "sonic-pi-tech_saws", nodeId, 0, 0,
          "note", 43.0, "cutoff", 100.0, "amp", 0.5,
          "sustain", 6.0, "release", 1.0, "res", resVal, "out_bus", 0);
        await new Promise(r => setTimeout(r, 2000));
        const cap = sonic.stopCapture();
        await sonic.send("/n_free", nodeId);
        await new Promise(r => setTimeout(r, 300));

        const L = cap.left;
        const R = cap.right;
        const sr = cap.sampleRate;

        // Resonance zone: cutoff=100 MIDI = 2637Hz
        const resEnergyL = bandEnergy(Array.from(L), sr, 2400, 2900);
        const resEnergyR = bandEnergy(Array.from(R), sr, 2400, 2900);
        const bassEnergyL = bandEnergy(Array.from(L), sr, 50, 500);
        const bassEnergyR = bandEnergy(Array.from(R), sr, 50, 500);

        captures[resVal] = {
          resEnergyL, resEnergyR, bassEnergyL, bassEnergyR,
          rmsL: rms(Array.from(L), 0.2, 1.5, sr),
          rmsR: rms(Array.from(R), 0.2, 1.5, sr),
        };
        nodeId++;
      }

      await sonic.destroy();
      return captures;
    }, [sonicConfig, SPECTRAL_HELPERS]);

    // The key test: does changing res actually change the resonance energy?
    const lowRes = result[0.01];   // rq should be 0.99 → LOW resonance
    const highRes = result[0.99];  // rq should be 0.01 → HIGH resonance

    console.log("res=0.01 → expected low resonance:");
    console.log("  L resonance energy:", lowRes.resEnergyL.toFixed(3));
    console.log("  L bass energy:", lowRes.bassEnergyL.toFixed(3));
    console.log("res=0.99 → expected high resonance:");
    console.log("  L resonance energy:", highRes.resEnergyL.toFixed(3));
    console.log("  L bass energy:", highRes.bassEnergyL.toFixed(3));

    // If Clip works: res=0.01 should have LESS resonance than res=0.99
    // The resonance ratio should change significantly (at least 2x)
    const ratio = highRes.resEnergyL / (lowRes.resEnergyL || 0.0001);
    console.log("Resonance energy ratio (high/low res):", ratio.toFixed(3));
    console.log("Expected: >> 1.0 if res affects resonance");
    console.log("If ~1.0: res has no effect → Clip UGen broken with inverted bounds");

    expect(ratio, "res parameter should significantly affect resonance energy").toBeGreaterThan(1.5);
  });

  test("RLPF resonance should not dominate over fundamental", async ({ page, sonicConfig }) => {
    await page.goto("/test/harness.html");
    const result = await page.evaluate(async ([config, helpers]) => {
      eval(helpers);

      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-tech_saws");
      await sonic.sync();

      // tech_saws with cutoff=100, default res=0.7
      sonic.startCapture();
      await sonic.send("/s_new", "sonic-pi-tech_saws", 9100, 0, 0,
        "note", 43.0, "cutoff", 100.0, "amp", 0.5,
        "sustain", 6.0, "release", 1.0, "out_bus", 0);
      await new Promise(r => setTimeout(r, 2000));
      const cap = sonic.stopCapture();
      await sonic.send("/n_free", 9100);

      const L = Array.from(cap.left);
      const sr = cap.sampleRate;

      // G2 fundamental = 98Hz, resonance at cutoff = 2637Hz
      const fundamentalEnergy = bandEnergy(L, sr, 80, 120);
      const resonanceEnergy = bandEnergy(L, sr, 2400, 2900);

      await sonic.destroy();
      return { fundamentalEnergy, resonanceEnergy, sampleRate: sr };
    }, [sonicConfig, SPECTRAL_HELPERS]);

    console.log("Fundamental (98Hz) energy:", result.fundamentalEnergy.toFixed(3));
    console.log("Resonance (2637Hz) energy:", result.resonanceEnergy.toFixed(3));
    const ratio = result.fundamentalEnergy / (result.resonanceEnergy || 0.0001);
    console.log("Fundamental/Resonance ratio:", ratio.toFixed(3));
    console.log("Expected: > 1.0 (fundamental should be louder than resonance peak)");
    console.log("OG scsynth reference: ratio ~7.6");

    // In OG scsynth, fundamental energy is ~8x the resonance energy
    // If ratio < 1, the resonance dominates (the "whistle" bug)
    expect(ratio, "fundamental should be louder than resonance peak").toBeGreaterThan(1.0);
  });

  test("L and R channels should have similar spectral profile", async ({ page, sonicConfig }) => {
    await page.goto("/test/harness.html");
    const result = await page.evaluate(async ([config, helpers]) => {
      eval(helpers);

      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-tech_saws");
      await sonic.sync();

      sonic.startCapture();
      await sonic.send("/s_new", "sonic-pi-tech_saws", 9200, 0, 0,
        "note", 43.0, "cutoff", 100.0, "amp", 0.5,
        "sustain", 6.0, "release", 1.0, "out_bus", 0);
      await new Promise(r => setTimeout(r, 2000));
      const cap = sonic.stopCapture();
      await sonic.send("/n_free", 9200);

      const L = Array.from(cap.left);
      const R = Array.from(cap.right);
      const sr = cap.sampleRate;

      const lBass = bandEnergy(L, sr, 50, 500);
      const rBass = bandEnergy(R, sr, 50, 500);
      const lRes = bandEnergy(L, sr, 2400, 2900);
      const rRes = bandEnergy(R, sr, 2400, 2900);
      const lHfRatio = lRes / (lBass + lRes || 0.0001);
      const rHfRatio = rRes / (rBass + rRes || 0.0001);

      await sonic.destroy();
      return { lBass, rBass, lRes, rRes, lHfRatio, rHfRatio };
    }, [sonicConfig, SPECTRAL_HELPERS]);

    console.log("L bass:", result.lBass.toFixed(3), " R bass:", result.rBass.toFixed(3));
    console.log("L resonance:", result.lRes.toFixed(3), " R resonance:", result.rRes.toFixed(3));
    console.log("L HF ratio:", result.lHfRatio.toFixed(3), " R HF ratio:", result.rHfRatio.toFixed(3));

    // L and R will differ due to Splay stereo spread (different random detuning),
    // but their HF ratios should be in the same ballpark — within 2x of each other
    const hfDelta = Math.abs(result.lHfRatio - result.rHfRatio);
    console.log("HF ratio delta:", hfDelta.toFixed(3));

    expect(hfDelta, "L and R HF ratios should be similar").toBeLessThan(0.3);
  });
});

/**
 * Tests for SynthDef Reblock() and Resample() (upstream PR #7402).
 *
 * Strategy: probe synthdefs write `BlockSize.ir` and `SampleRate.ir` to two
 * consecutive control buses. We read them via `/c_get` and assert the actual
 * per-Graph rate values match the requested Reblock/Resample configuration.
 *
 * This is what proves the feature is wired up — a synth with a broken or no-op
 * Reblock would still produce audio. Only the rate observation distinguishes
 * "feature is active" from "feature was silently ignored".
 *
 * Server defaults (SuperSonic): blockSize=128, sampleRate=48000.
 *
 * Fixtures: test/synthdefs/compile_reblock_resample_synthdefs.scd
 */
import { test, expect, skipIfPostMessage } from "./fixtures.mjs";

const FIX_DIR = "/test/synthdefs/reblock_resample";

// Each probe writes [BlockSize.ir, SampleRate.ir] to two consecutive control buses.
// Verifies the per-Graph rate the synth actually runs at.
const PROBES = [
  { name: "rr_probe_baseline",         expectBlock: 128, expectRate: 48000 },
  { name: "rr_probe_reblock32",        expectBlock:  32, expectRate: 48000 },
  { name: "rr_probe_reblock64",        expectBlock:  64, expectRate: 48000 },
  { name: "rr_probe_resample2",        expectBlock: 128, expectRate: 96000 },
  { name: "rr_probe_resample4",        expectBlock: 128, expectRate: 192000 },
  { name: "rr_probe_reblock_resample", expectBlock:  32, expectRate: 96000 },
];

// Helper run inside page.evaluate. Creates the synth, reads two control buses
// via /c_get, returns [blockSize, sampleRate]. Assumes a `messages` array has
// already been wired up to receive 'in' events.
const PROBE_HARNESS = `
async function probeRates(sonic, messages, fixtureName, probeBus, sNewArgs = []) {
  const NID = 7000 + Math.floor(Math.random() * 1000);
  await sonic.send("/s_new", fixtureName, NID, 0, 0, "bus", probeBus, ...sNewArgs);
  // Eager-init in /s_new makes BlockSize/SampleRate Ctors run, but the first
  // Out.kr that writes them onto the bus happens on the next calc cycle.
  await new Promise((r) => setTimeout(r, 60));

  messages.length = 0;
  await sonic.send("/c_get", probeBus, probeBus + 1);
  await sonic.sync(99);
  await new Promise((r) => setTimeout(r, 30));
  await sonic.send("/n_free", NID);

  // /c_get with multiple indices returns ONE /c_set reply with alternating
  // (busIndex, value) pairs: ["/c_set", b1, v1, b2, v2, ...].
  const reply = messages.find((m) => m[0] === "/c_set");
  if (!reply) return [undefined, undefined];
  const byBus = new Map();
  for (let i = 1; i < reply.length; i += 2) byBus.set(reply[i], reply[i + 1]);
  return [byBus.get(probeBus), byBus.get(probeBus + 1)];
}
`;

test.describe("Reblock/Resample — per-Graph rate observation", () => {
  for (const probe of PROBES) {
    test(`${probe.name} → BlockSize=${probe.expectBlock}, SampleRate=${probe.expectRate}`, async ({ page, sonicConfig }) => {
      await page.goto("/test/harness.html");
      const result = await page.evaluate(async ({ config, fixDir, harness, name }) => {
        const sonic = new window.SuperSonic(config);
        const messages = [];
        sonic.on("in", (m) => messages.push(Array.from(m)));
        await sonic.init();

        const bytes = new Uint8Array(await (await fetch(`${fixDir}/${name}.scsyndef`)).arrayBuffer());
        await sonic.loadSynthDef(bytes);

        eval(harness);
        const [block, rate] = await probeRates(sonic, messages, name, 100);
        return { block, rate };
      }, { config: sonicConfig, fixDir: FIX_DIR, harness: PROBE_HARNESS, name: probe.name });

      expect(result.block).toBe(probe.expectBlock);
      expect(result.rate).toBe(probe.expectRate);
    });
  }

  test("rr_probe_reblock_ctrl with default blockSize=16 → BlockSize=16", async ({ page, sonicConfig }) => {
    await page.goto("/test/harness.html");
    const result = await page.evaluate(async ({ config, fixDir, harness }) => {
      const sonic = new window.SuperSonic(config);
      const messages = [];
      sonic.on("in", (m) => messages.push(Array.from(m)));
      await sonic.init();
      const bytes = new Uint8Array(await (await fetch(`${fixDir}/rr_probe_reblock_ctrl.scsyndef`)).arrayBuffer());
      await sonic.loadSynthDef(bytes);
      eval(harness);
      const [block, rate] = await probeRates(sonic, messages, "rr_probe_reblock_ctrl", 110);
      return { block, rate };
    }, { config: sonicConfig, fixDir: FIX_DIR, harness: PROBE_HARNESS });

    expect(result.block).toBe(16);
    expect(result.rate).toBe(48000);
  });

  test("rr_probe_reblock_ctrl with custom blockSize=64 → BlockSize=64", async ({ page, sonicConfig }) => {
    await page.goto("/test/harness.html");
    const result = await page.evaluate(async ({ config, fixDir, harness }) => {
      const sonic = new window.SuperSonic(config);
      const messages = [];
      sonic.on("in", (m) => messages.push(Array.from(m)));
      await sonic.init();
      const bytes = new Uint8Array(await (await fetch(`${fixDir}/rr_probe_reblock_ctrl.scsyndef`)).arrayBuffer());
      await sonic.loadSynthDef(bytes);
      eval(harness);
      const [block, rate] = await probeRates(sonic, messages, "rr_probe_reblock_ctrl", 120, ["blockSize", 64]);
      return { block, rate };
    }, { config: sonicConfig, fixDir: FIX_DIR, harness: PROBE_HARNESS });

    expect(result.block).toBe(64);
    expect(result.rate).toBe(48000);
  });

  test("rr_probe_resample_ctrl with default factor=2 → SampleRate=96000", async ({ page, sonicConfig }) => {
    await page.goto("/test/harness.html");
    const result = await page.evaluate(async ({ config, fixDir, harness }) => {
      const sonic = new window.SuperSonic(config);
      const messages = [];
      sonic.on("in", (m) => messages.push(Array.from(m)));
      await sonic.init();
      const bytes = new Uint8Array(await (await fetch(`${fixDir}/rr_probe_resample_ctrl.scsyndef`)).arrayBuffer());
      await sonic.loadSynthDef(bytes);
      eval(harness);
      const [block, rate] = await probeRates(sonic, messages, "rr_probe_resample_ctrl", 130);
      return { block, rate };
    }, { config: sonicConfig, fixDir: FIX_DIR, harness: PROBE_HARNESS });

    expect(result.block).toBe(128);
    expect(result.rate).toBe(96000);
  });

  test("rr_probe_resample_ctrl with custom factor=4 → SampleRate=192000", async ({ page, sonicConfig }) => {
    await page.goto("/test/harness.html");
    const result = await page.evaluate(async ({ config, fixDir, harness }) => {
      const sonic = new window.SuperSonic(config);
      const messages = [];
      sonic.on("in", (m) => messages.push(Array.from(m)));
      await sonic.init();
      const bytes = new Uint8Array(await (await fetch(`${fixDir}/rr_probe_resample_ctrl.scsyndef`)).arrayBuffer());
      await sonic.loadSynthDef(bytes);
      eval(harness);
      const [block, rate] = await probeRates(sonic, messages, "rr_probe_resample_ctrl", 140, ["factor", 4]);
      return { block, rate };
    }, { config: sonicConfig, fixDir: FIX_DIR, harness: PROBE_HARNESS });

    expect(result.block).toBe(128);
    expect(result.rate).toBe(192000);
  });
});

// ---------------------------------------------------------------------------
// End-to-end smoke: a Reblock+Resample synth still produces audible 440 Hz.
// ---------------------------------------------------------------------------
test.describe("Reblock/Resample — audio still flows (SAB only)", () => {
  test.beforeEach(async ({ sonicMode }) => {
    skipIfPostMessage(sonicMode, "Audio capture requires SAB mode");
  });

  test("rr_audio_reblock_resample produces ~440 Hz sine", async ({ page, sonicConfig }) => {
    await page.goto("/test/harness.html");
    const result = await page.evaluate(async ({ config, fixDir }) => {
      function calculateRMS(s) { let x = 0; for (let i = 0; i < s.length; i++) x += s[i]*s[i]; return Math.sqrt(x/s.length); }
      function findPeak(s) { let p = 0; for (let i = 0; i < s.length; i++) if (Math.abs(s[i]) > p) p = Math.abs(s[i]); return p; }
      function estimateFrequency(s, sr) {
        let c = 0;
        for (let i = 1; i < s.length; i++) {
          if ((s[i-1] >= 0 && s[i] < 0) || (s[i-1] < 0 && s[i] >= 0)) c++;
        }
        return c / (2 * (s.length / sr));
      }
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      const bytes = new Uint8Array(await (await fetch(`${fixDir}/rr_audio_reblock_resample.scsyndef`)).arrayBuffer());
      await sonic.loadSynthDef(bytes);

      sonic.startCapture();
      await sonic.send("/s_new", "rr_audio_reblock_resample", 5500, 0, 0);
      await new Promise((r) => setTimeout(r, 200));
      const captured = sonic.stopCapture();
      await sonic.send("/n_free", 5500);

      const samples = captured.left.subarray(2400, captured.frames);
      return {
        rms: calculateRMS(samples),
        peak: findPeak(samples),
        freq: estimateFrequency(samples, captured.sampleRate),
      };
    }, { config: sonicConfig, fixDir: FIX_DIR });

    expect(result.rms).toBeGreaterThan(0.05);
    expect(result.peak).toBeLessThanOrEqual(1.0);
    expect(Math.abs(result.freq - 440)).toBeLessThanOrEqual(30);
  });
});

/**
 * SynthDef Reblock() and Resample() (upstream PR #7402) in the browser.
 *
 * The rates each graph runs at, and the sound of every reblock/resample
 * combination, are proved in test/native/test_reblock_resample.cpp, which
 * renders every block itself and gets the same result on every run. This
 * spec checks only that the web build's worklet plays a reblocked,
 * resampled graph.
 *
 * Fixtures: test/synthdefs/compile_reblock_resample_synthdefs.scd
 */
import { test, expect, skipIfPostMessage, WAIT_FOR_PROGRESS_HELPER } from "./fixtures.mjs";

const FIX_DIR = "/test/synthdefs/reblock_resample";

// ---------------------------------------------------------------------------
// End-to-end smoke: a Reblock+Resample synth still produces audible 440 Hz.
// ---------------------------------------------------------------------------
test.describe("Reblock/Resample — audio still flows (SAB only)", () => {
  test.beforeEach(async ({ sonicMode }) => {
    skipIfPostMessage(sonicMode, "Audio capture requires SAB mode");
  });

  // rr_audio_reblock_resample: Reblock(32) + Resample(2).
  const AUDIO = [
    { name: "rr_audio_reblock_resample", args: [] },
  ];
  for (const a of AUDIO) {
    test(`${a.name}${a.args.length ? ` ${a.args.join(" ")}` : ""} produces ~440 Hz sine`, async ({ page, sonicConfig }) => {
      await page.goto("/test/harness.html");
      const result = await page.evaluate(async ({ config, fixDir, a, progress }) => {
        eval(progress);
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
        const bytes = new Uint8Array(await (await fetch(`${fixDir}/${a.name}.scsyndef`)).arrayBuffer());
        await sonic.loadSynthDef(bytes);

        // 50 ms skipped while the synth starts, then 200 ms analysed: frames,
        // not wall time, so a slow machine captures the same audio.
        sonic.startCapture();
        await sonic.send("/s_new", a.name, 5500, 0, 0, ...a.args);
        await waitForCaptureFrames(sonic, 2400 + 9600);
        const captured = sonic.stopCapture();
        await sonic.send("/n_free", 5500);

        const samples = captured.left.subarray(2400, captured.frames);
        return {
          rms: calculateRMS(samples),
          peak: findPeak(samples),
          freq: estimateFrequency(samples, captured.sampleRate),
        };
      }, { config: sonicConfig, fixDir: FIX_DIR, a, progress: WAIT_FOR_PROGRESS_HELPER });

      expect(result.rms).toBeGreaterThan(0.05);
      expect(result.peak).toBeLessThanOrEqual(1.0);
      expect(Math.abs(result.freq - 440)).toBeLessThanOrEqual(30);
    });
  }
});

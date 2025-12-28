/**
 * Input Bus Passthrough Test
 *
 * Tests that audio connected to the worklet's input passes through
 * scsynth's In.ar UGen correctly.
 *
 * BUG BEING TESTED:
 * - audio_processor.cpp zeros ALL audio buses, wiping input data
 * - mAudioBusTouched isn't marked for inputs, so In.ar outputs silence
 */

import { test, expect } from "./fixtures.mjs";

// Audio analysis helpers (injected into browser context)
const AUDIO_HELPERS = `
  function hasAudio(samples, threshold = 0.001) {
    if (!samples || samples.length === 0) return false;
    for (let i = 0; i < samples.length; i++) {
      if (Math.abs(samples[i]) > threshold) return true;
    }
    return false;
  }

  function calculateRMS(samples) {
    if (!samples || samples.length === 0) return 0;
    let sum = 0;
    for (let i = 0; i < samples.length; i++) {
      sum += samples[i] * samples[i];
    }
    return Math.sqrt(sum / samples.length);
  }

  function findPeak(samples) {
    if (!samples || samples.length === 0) return 0;
    let peak = 0;
    for (let i = 0; i < samples.length; i++) {
      const abs = Math.abs(samples[i]);
      if (abs > peak) peak = abs;
    }
    return peak;
  }
`;

test.describe("Input Bus Passthrough", () => {
  test.beforeEach(async ({ page }) => {
    page.on("console", (msg) => {
      if (msg.type() === "error") {
        console.error("Browser console error:", msg.text());
      }
    });

    await page.goto("/test/harness.html");
    await page.waitForFunction(() => window.supersonicReady === true, {
      timeout: 10000,
    });
  });

  test("oscillator connected to worklet input passes through In.ar", async ({ page, sonicConfig }) => {

    const result = await page.evaluate(async ({ config, helpers }) => {
      eval(helpers);

      const sonic = new window.SuperSonic(config);
      await sonic.init();

      // Get the worklet node
      const wrapper = sonic.node;
      const audioContext = wrapper.context;

      // Load passthrough synthdef (reads from in_bus, outputs to out)
      await sonic.loadSynthDef("simple_passthrough");

      // Create oscillator at 440Hz
      const osc = audioContext.createOscillator();
      osc.frequency.value = 440;
      osc.type = "sine";
      osc.start();

      // Connect oscillator directly to worklet node's input
      osc.connect(wrapper.input);

      // Wait for audio to flow
      await new Promise(r => setTimeout(r, 100));

      // Create passthrough synth that reads from input bus 2 (first input bus)
      // Bus layout: [0-1 output][2-3 input][4+ private]
      const inputBus = 2;
      await sonic.send("/s_new", "simple_passthrough", 1000, 0, 0, "in_bus", inputBus, "out", 0);

      // Use AnalyserNode to detect audio output (works in both SAB and postMessage modes)
      const analyser = audioContext.createAnalyser();
      analyser.fftSize = 2048;
      wrapper.connect(analyser);

      // Wait for audio to pass through
      await new Promise(r => setTimeout(r, 300));

      // Check for audio using AnalyserNode
      const dataArray = new Float32Array(analyser.fftSize);
      analyser.getFloatTimeDomainData(dataArray);

      const hasAudioOutput = hasAudio(dataArray, 0.01);
      const rms = calculateRMS(dataArray);
      const peak = findPeak(dataArray);

      // Also try SAB capture if available
      let sabCapture = null;
      if (config.mode === 'sab') {
        sonic.startCapture();
        await new Promise(r => setTimeout(r, 200));
        sabCapture = sonic.stopCapture();
      }

      // Clean up
      await sonic.send("/n_free", 1000);
      osc.stop();
      osc.disconnect();

      await sonic.destroy();

      return {
        hasAudio: hasAudioOutput,
        rms,
        peak,
        mode: config.mode,
        sabCapture: sabCapture ? { hasAudio: hasAudio(sabCapture.left, 0.01), rms: calculateRMS(sabCapture.left) } : null,
      };
    }, { config: sonicConfig, helpers: AUDIO_HELPERS });

    console.log("Result:", result);

    // The oscillator should pass through scsynth and produce audio output
    expect(result.hasAudio).toBe(true);
    expect(result.rms).toBeGreaterThan(0.01);
    expect(result.peak).toBeGreaterThan(0.05);
  });

  test("In.ar without external input produces silence", async ({ page, sonicConfig }) => {
    // Skip in postMessage mode
    if (sonicConfig.mode === 'postMessage') {
      test.skip();
      return;
    }

    const result = await page.evaluate(async ({ config, helpers }) => {
      eval(helpers);

      const sonic = new window.SuperSonic(config);
      await sonic.init();

      await sonic.loadSynthDef("simple_passthrough");

      sonic.startCapture();

      // Create passthrough synth but don't connect any input
      // Bus layout: [0-1 output][2-3 input][4+ private]
      const inputBus = 2;
      await sonic.send("/s_new", "simple_passthrough", 1000, 0, 0, "in_bus", inputBus, "out", 0);

      await new Promise(r => setTimeout(r, 200));

      const captured = sonic.stopCapture();
      await sonic.send("/n_free", 1000);

      const hasAudioOutput = hasAudio(captured.left, 0.01);
      const rms = calculateRMS(captured.left);

      await sonic.destroy();

      return { hasAudio: hasAudioOutput, rms };
    }, { config: sonicConfig, helpers: AUDIO_HELPERS });

    // With nothing connected, should be silence
    expect(result.hasAudio).toBe(false);
    expect(result.rms).toBeLessThan(0.001);
  });
});

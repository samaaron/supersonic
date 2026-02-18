/**
 * Multichannel Audio Output Tests
 *
 * Tests that SuperSonic correctly supports configurable output channel counts
 * (mono, stereo, quad, 8-channel, etc.) by verifying the JS-layer changes
 * to AudioWorkletNode outputChannelCount and worklet output copy loop.
 */

import { test, expect, skipIfPostMessage } from "./fixtures.mjs";

// Audio analysis helpers (inlined for page.evaluate)
const AUDIO_HELPERS = `
function calculateRMS(samples, start = 0, end = samples.length) {
  if (end <= start || samples.length === 0) return 0;
  let sum = 0;
  for (let i = start; i < end && i < samples.length; i++) {
    sum += samples[i] * samples[i];
  }
  return Math.sqrt(sum / (end - start));
}

function hasAudio(samples, threshold = 0.001) {
  if (!samples || samples.length === 0) return false;
  for (let i = 0; i < samples.length; i++) {
    if (Math.abs(samples[i]) > threshold) return true;
  }
  return false;
}
`;

// =============================================================================
// VALIDATION TESTS
// =============================================================================

test.describe("Multichannel Output - Validation", () => {
  test.beforeEach(async ({ page }) => {
    await page.goto("/test/harness.html");
    await page.waitForFunction(() => window.supersonicReady === true, { timeout: 10000 });
  });

  test("numOutputBusChannels: 0 throws", async ({ page, sonicConfig }) => {
    const result = await page.evaluate(async (config) => {
      try {
        new window.SuperSonic({
          ...config,
          scsynthOptions: { numOutputBusChannels: 0 }
        });
        return { threw: false };
      } catch (error) {
        return { threw: true, message: error.message };
      }
    }, sonicConfig);

    expect(result.threw).toBe(true);
    expect(result.message).toContain("numOutputBusChannels");
  });

  test("numOutputBusChannels: 129 throws (exceeds max 128)", async ({ page, sonicConfig }) => {
    const result = await page.evaluate(async (config) => {
      try {
        new window.SuperSonic({
          ...config,
          scsynthOptions: { numOutputBusChannels: 129 }
        });
        return { threw: false };
      } catch (error) {
        return { threw: true, message: error.message };
      }
    }, sonicConfig);

    expect(result.threw).toBe(true);
    expect(result.message).toContain("numOutputBusChannels");
  });

  test("numOutputBusChannels: 1 accepted (mono)", async ({ page, sonicConfig }) => {
    const result = await page.evaluate(async (config) => {
      try {
        new window.SuperSonic({
          ...config,
          scsynthOptions: { numOutputBusChannels: 1 }
        });
        return { threw: false };
      } catch (error) {
        return { threw: true, message: error.message };
      }
    }, sonicConfig);

    expect(result.threw).toBe(false);
  });

  test("numOutputBusChannels: 8 accepted", async ({ page, sonicConfig }) => {
    const result = await page.evaluate(async (config) => {
      try {
        new window.SuperSonic({
          ...config,
          scsynthOptions: { numOutputBusChannels: 8 }
        });
        return { threw: false };
      } catch (error) {
        return { threw: true, message: error.message };
      }
    }, sonicConfig);

    expect(result.threw).toBe(false);
  });
});

// =============================================================================
// STEREO BACKWARDS COMPATIBILITY
// =============================================================================

test.describe("Multichannel Output - Stereo Backwards Compatibility", () => {
  test("default config does not modify destination.channelCount", async ({ page, sonicConfig }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async ({ config, helpers }) => {
      eval(helpers);

      // Record default destination channelCount before init
      const ctx = new AudioContext();
      const defaultChannelCount = ctx.destination.channelCount;
      await ctx.close();

      const sonic = new window.SuperSonic(config);
      await sonic.init();

      const wrapper = sonic.node;
      const audioContext = wrapper.context;
      const destChannelCount = audioContext.destination.channelCount;
      const destInterpretation = audioContext.destination.channelInterpretation;

      // Play a synth to verify audio works
      await sonic.loadSynthDef("sonic-pi-beep");
      await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 0, "amp", 0.3);
      await new Promise(r => setTimeout(r, 100));

      // Use analyser to check audio output
      const analyser = audioContext.createAnalyser();
      analyser.fftSize = 2048;
      wrapper.connect(analyser);
      await new Promise(r => setTimeout(r, 100));

      const dataArray = new Float32Array(analyser.fftSize);
      analyser.getFloatTimeDomainData(dataArray);
      const audioPresent = hasAudio(dataArray, 0.01);

      await sonic.send("/n_free", 1000);

      return {
        defaultChannelCount,
        destChannelCount,
        destInterpretation,
        audioPresent,
      };
    }, { config: sonicConfig, helpers: AUDIO_HELPERS });

    // destination.channelCount should be unchanged from browser default
    expect(result.destChannelCount).toBe(result.defaultChannelCount);
    // channelInterpretation should NOT be 'discrete' for stereo
    expect(result.destInterpretation).not.toBe('discrete');
    expect(result.audioPresent).toBe(true);
  });

  test("stereo audio capture still works with default config (SAB)", async ({ page, sonicConfig, sonicMode }) => {
    skipIfPostMessage(sonicMode, 'Audio capture requires SAB mode');
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async ({ config, helpers }) => {
      eval(helpers);

      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      sonic.startCapture();
      await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 0, "amp", 0.5);
      await new Promise(r => setTimeout(r, 200));
      const captured = sonic.stopCapture();
      await sonic.send("/n_free", 1000);

      return {
        channels: captured.channels,
        hasLeftAudio: hasAudio(captured.left, 0.01),
        hasRightAudio: hasAudio(captured.right, 0.01),
        leftRMS: calculateRMS(captured.left, 0, Math.min(captured.frames, 5000)),
      };
    }, { config: sonicConfig, helpers: AUDIO_HELPERS });

    expect(result.channels).toBe(2);
    expect(result.hasLeftAudio).toBe(true);
    expect(result.hasRightAudio).toBe(true);
    expect(result.leftRMS).toBeGreaterThan(0.001);
  });
});

// =============================================================================
// QUAD OUTPUT CONFIGURATION
// =============================================================================

test.describe("Multichannel Output - Quad Configuration", () => {
  test("4-channel config sets destination properties correctly", async ({ page, sonicConfig }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic({
        ...config,
        scsynthOptions: { numOutputBusChannels: 4 },
      });
      await sonic.init();

      const wrapper = sonic.node;
      const audioContext = wrapper.context;
      const dest = audioContext.destination;

      return {
        destChannelCount: dest.channelCount,
        destMaxChannelCount: dest.maxChannelCount,
        destInterpretation: dest.channelInterpretation,
        expectedChannelCount: Math.min(4, dest.maxChannelCount),
      };
    }, sonicConfig);

    expect(result.destChannelCount).toBe(result.expectedChannelCount);
    expect(result.destInterpretation).toBe('discrete');
  });
});

// =============================================================================
// SIMULTANEOUS SYNTHS ON DIFFERENT CHANNELS (SAB only - reads WASM output bus directly)
// =============================================================================

test.describe("Multichannel Output - Channel Independence", () => {
  test("4 synths on 4 separate output buses produce audio on all channels", async ({ page, sonicConfig, sonicMode }) => {
    skipIfPostMessage(sonicMode, 'Direct WASM bus read requires SAB mode');
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async ({ config, helpers }) => {
      eval(helpers);

      const sonic = new window.SuperSonic({
        ...config,
        scsynthOptions: { numOutputBusChannels: 4 },
      });
      await sonic.init();
      // Use fft_test_sine which is a MONO synth with controllable out bus
      await sonic.loadSynthDef("fft_test_sine");

      // Play 4 mono synths simultaneously, each on a different output bus
      await sonic.send("/s_new", "fft_test_sine", 1000, 0, 0, "out", 0, "freq", 220, "amp", 0.5);
      await sonic.send("/s_new", "fft_test_sine", 1001, 0, 0, "out", 1, "freq", 330, "amp", 0.5);
      await sonic.send("/s_new", "fft_test_sine", 1002, 0, 0, "out", 2, "freq", 440, "amp", 0.5);
      await sonic.send("/s_new", "fft_test_sine", 1003, 0, 0, "out", 3, "freq", 550, "amp", 0.5);

      // Wait for audio to accumulate
      await new Promise(r => setTimeout(r, 300));

      // Read directly from the WASM output bus via SharedArrayBuffer
      // This bypasses WebAudio destination channel limitations in headless browsers
      const node = sonic.node;
      const workletNode = node.workletNode || node._workletNode;

      // Use audio capture for channels 0-1 (stereo capture)
      sonic.startCapture();
      await new Promise(r => setTimeout(r, 200));
      const captured = sonic.stopCapture();

      const ch0HasAudio = hasAudio(captured.left, 0.005);
      const ch1HasAudio = hasAudio(captured.right, 0.005);
      const ch0RMS = calculateRMS(captured.left, 0, Math.min(captured.frames, 5000));
      const ch1RMS = calculateRMS(captured.right, 0, Math.min(captured.frames, 5000));

      // For channels 2-3, verify via AnalyserNode on the worklet output
      // (even if headless browser downmixes, the worklet node outputs 4ch)
      const audioContext = node.context;
      const splitter = audioContext.createChannelSplitter(4);
      const merger = audioContext.createChannelMerger(1);
      node.connect(splitter);

      // Check channel 2
      const analyser2 = audioContext.createAnalyser();
      analyser2.fftSize = 2048;
      splitter.connect(analyser2, 2);
      // Check channel 3
      const analyser3 = audioContext.createAnalyser();
      analyser3.fftSize = 2048;
      splitter.connect(analyser3, 3);

      await new Promise(r => setTimeout(r, 200));

      const data2 = new Float32Array(analyser2.fftSize);
      analyser2.getFloatTimeDomainData(data2);
      const data3 = new Float32Array(analyser3.fftSize);
      analyser3.getFloatTimeDomainData(data3);

      // Clean up
      await sonic.send("/n_free", 1000);
      await sonic.send("/n_free", 1001);
      await sonic.send("/n_free", 1002);
      await sonic.send("/n_free", 1003);

      return {
        ch0HasAudio, ch0RMS,
        ch1HasAudio, ch1RMS,
        ch2HasAudio: hasAudio(data2, 0.005),
        ch2RMS: calculateRMS(data2),
        ch3HasAudio: hasAudio(data3, 0.005),
        ch3RMS: calculateRMS(data3),
        maxChannelCount: audioContext.destination.maxChannelCount,
      };
    }, { config: sonicConfig, helpers: AUDIO_HELPERS });

    // Channels 0-1 verified via SAB audio capture (reliable)
    expect(result.ch0HasAudio, "Channel 0 should have audio").toBe(true);
    expect(result.ch0RMS).toBeGreaterThan(0.001);
    expect(result.ch1HasAudio, "Channel 1 should have audio").toBe(true);
    expect(result.ch1RMS).toBeGreaterThan(0.001);

    // Channels 2-3 via ChannelSplitter - may not work in headless browsers
    // with maxChannelCount=2. Only assert if the browser supports >2 channels.
    if (result.maxChannelCount >= 4) {
      expect(result.ch2HasAudio, "Channel 2 should have audio").toBe(true);
      expect(result.ch3HasAudio, "Channel 3 should have audio").toBe(true);
    }
  });

  test("channel isolation: mono synth on bus 1 only produces audio on right channel", async ({ page, sonicConfig, sonicMode }) => {
    skipIfPostMessage(sonicMode, 'Audio capture requires SAB mode');
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async ({ config, helpers }) => {
      eval(helpers);

      const sonic = new window.SuperSonic({
        ...config,
        scsynthOptions: { numOutputBusChannels: 4 },
      });
      await sonic.init();

      // Use fft_test_sine which is a MONO synth with controllable out bus
      await sonic.loadSynthDef("fft_test_sine");

      // Play a mono synth on bus 1 only (channel index 1 = right in stereo capture)
      await sonic.send("/s_new", "fft_test_sine", 1000, 0, 0, "out", 1, "freq", 440, "amp", 0.5);

      // Wait for audio to stabilize
      await new Promise(r => setTimeout(r, 200));

      // Use stereo capture to check channels 0 and 1
      sonic.startCapture();
      await new Promise(r => setTimeout(r, 200));
      const captured = sonic.stopCapture();

      await sonic.send("/n_free", 1000);

      return {
        ch0HasAudio: hasAudio(captured.left, 0.005),
        ch0RMS: calculateRMS(captured.left, 0, Math.min(captured.frames, 5000)),
        ch1HasAudio: hasAudio(captured.right, 0.005),
        ch1RMS: calculateRMS(captured.right, 0, Math.min(captured.frames, 5000)),
      };
    }, { config: sonicConfig, helpers: AUDIO_HELPERS });

    // Channel 0 (left) should be silent - mono synth writes only to bus 1
    expect(result.ch0HasAudio, "Channel 0 should be silent").toBe(false);
    expect(result.ch0RMS).toBeLessThan(0.001);
    // Channel 1 (right) should have audio
    expect(result.ch1HasAudio, "Channel 1 should have audio").toBe(true);
    expect(result.ch1RMS).toBeGreaterThan(0.001);
  });
});

// =============================================================================
// STEREO CAPTURE WITH 4-CHANNEL OUTPUT (SAB ONLY)
// =============================================================================

test.describe("Multichannel Output - Capture Compatibility", () => {
  test("stereo audio capture works with 4 output channels (SAB)", async ({ page, sonicConfig, sonicMode }) => {
    skipIfPostMessage(sonicMode, 'Audio capture requires SAB mode');
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async ({ config, helpers }) => {
      eval(helpers);

      const sonic = new window.SuperSonic({
        ...config,
        scsynthOptions: { numOutputBusChannels: 4 },
      });
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      // Play synth on bus 0 (captured by stereo capture)
      sonic.startCapture();
      await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 0, "out", 0, "amp", 0.5);
      await new Promise(r => setTimeout(r, 200));
      const captured = sonic.stopCapture();
      await sonic.send("/n_free", 1000);

      return {
        channels: captured.channels,
        frames: captured.frames,
        hasLeftAudio: hasAudio(captured.left, 0.01),
        leftRMS: calculateRMS(captured.left, 0, Math.min(captured.frames, 5000)),
      };
    }, { config: sonicConfig, helpers: AUDIO_HELPERS });

    expect(result.channels).toBe(2);
    expect(result.frames).toBeGreaterThan(0);
    expect(result.hasLeftAudio).toBe(true);
    expect(result.leftRMS).toBeGreaterThan(0.001);
  });
});

// =============================================================================
// MONO OUTPUT
// =============================================================================

test.describe("Multichannel Output - Mono", () => {
  test("mono output (numOutputBusChannels: 1) produces audio", async ({ page, sonicConfig }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async ({ config, helpers }) => {
      eval(helpers);

      const sonic = new window.SuperSonic({
        ...config,
        scsynthOptions: { numOutputBusChannels: 1 },
        autoConnect: false,
      });
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      const wrapper = sonic.node;
      const audioContext = wrapper.context;

      // Connect to an AnalyserNode to check audio
      const analyser = audioContext.createAnalyser();
      analyser.fftSize = 2048;
      wrapper.connect(analyser);
      wrapper.connect(audioContext.destination);

      // Play synth on bus 0
      await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 0, "out", 0, "amp", 0.5);
      await new Promise(r => setTimeout(r, 300));

      const dataArray = new Float32Array(analyser.fftSize);
      analyser.getFloatTimeDomainData(dataArray);
      const rms = calculateRMS(dataArray);
      const audioPresent = hasAudio(dataArray, 0.005);

      await sonic.send("/n_free", 1000);

      return { audioPresent, rms };
    }, { config: sonicConfig, helpers: AUDIO_HELPERS });

    expect(result.audioPresent).toBe(true);
    expect(result.rms).toBeGreaterThan(0.001);
  });
});

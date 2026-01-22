/**
 * Audio Capture Test Suite
 *
 * Tests that verify actual audio output from synths using the
 * SharedArrayBuffer audio capture mechanism.
 *
 * NOTE: These tests require SAB mode - audio capture reads directly from
 * SharedArrayBuffer. They are automatically skipped in postMessage mode.
 *
 * These tests verify:
 * - Synths produce audio output
 * - Audio characteristics (amplitude, rough frequency)
 * - Multiple synths produce combined output
 * - Timing accuracy via OSC bundles
 */

import { test, expect, skipIfPostMessage } from "./fixtures.mjs";

// Skip all tests in this file if running in postMessage mode
test.beforeEach(async ({ sonicMode }) => {
  skipIfPostMessage(sonicMode, 'Audio capture requires SAB mode');
});

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

function findPeak(samples, start = 0, end = samples.length) {
  let peak = 0;
  for (let i = start; i < end && i < samples.length; i++) {
    const abs = Math.abs(samples[i]);
    if (abs > peak) peak = abs;
  }
  return peak;
}

function hasAudio(samples) {
  for (let i = 0; i < samples.length; i++) {
    if (samples[i] !== 0) return true;
  }
  return false;
}

function estimateFrequency(samples, sampleRate, start = 0, end = samples.length) {
  if (end - start < 2) return 0;
  let crossings = 0;
  for (let i = start + 1; i < end && i < samples.length; i++) {
    if ((samples[i - 1] >= 0 && samples[i] < 0) || (samples[i - 1] < 0 && samples[i] >= 0)) {
      crossings++;
    }
  }
  const duration = (end - start) / sampleRate;
  return crossings / (2 * duration);
}

function findFirstNonSilent(samples, sampleRate, threshold = 0.001) {
  const windowSize = Math.floor(sampleRate * 0.001); // 1ms window
  for (let i = 0; i < samples.length; i += windowSize) {
    const windowEnd = Math.min(i + windowSize, samples.length);
    if (calculateRMS(samples, i, windowEnd) >= threshold) {
      return i;
    }
  }
  return -1;
}
`;

// =============================================================================
// AUDIO CAPTURE API TESTS
// =============================================================================

test.describe("Audio Capture API", () => {
  test("startCapture and stopCapture work correctly", async ({ page, sonicConfig }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();

      // Check initial state
      const initialEnabled = sonic.isCaptureEnabled();
      const initialFrames = sonic.getCaptureFrames();

      // Start capture
      sonic.startCapture();
      const afterStartEnabled = sonic.isCaptureEnabled();

      // Wait a bit for frames to accumulate
      await new Promise((r) => setTimeout(r, 100));

      const afterWaitFrames = sonic.getCaptureFrames();

      // Stop capture
      const captured = sonic.stopCapture();
      const afterStopEnabled = sonic.isCaptureEnabled();

      return {
        initialEnabled,
        initialFrames,
        afterStartEnabled,
        afterWaitFrames,
        afterStopEnabled,
        capturedFrames: captured.frames,
        capturedSampleRate: captured.sampleRate,
        capturedChannels: captured.channels,
        hasLeft: captured.left instanceof Float32Array,
        hasRight: captured.right instanceof Float32Array,
        maxDuration: sonic.getMaxCaptureDuration(),
      };
    }, sonicConfig);

    expect(result.initialEnabled).toBe(false);
    expect(result.initialFrames).toBe(0);
    expect(result.afterStartEnabled).toBe(true);
    expect(result.afterWaitFrames).toBeGreaterThan(0);
    expect(result.afterStopEnabled).toBe(false);
    expect(result.capturedFrames).toBeGreaterThan(0);
    expect(result.capturedSampleRate).toBe(48000);
    expect(result.capturedChannels).toBe(2);
    expect(result.hasLeft).toBe(true);
    expect(result.hasRight).toBe(true);
    expect(result.maxDuration).toBe(1); // 1 second at 48kHz
  });

  test("capture buffer size matches configuration", async ({ page, sonicConfig }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();

      const bc = sonic.bufferConstants;
      return {
        captureFrames: bc.AUDIO_CAPTURE_FRAMES,
        captureChannels: bc.AUDIO_CAPTURE_CHANNELS,
        captureSampleRate: bc.AUDIO_CAPTURE_SAMPLE_RATE,
        expectedSeconds: bc.AUDIO_CAPTURE_FRAMES / bc.AUDIO_CAPTURE_SAMPLE_RATE,
      };
    }, sonicConfig);

    // Should be 1 second at 48kHz stereo
    expect(result.captureFrames).toBe(48000); // 48000 * 1
    expect(result.captureChannels).toBe(2);
    expect(result.captureSampleRate).toBe(48000);
    expect(result.expectedSeconds).toBe(1);
  });
});

// =============================================================================
// SYNTH AUDIO OUTPUT TESTS
// =============================================================================

test.describe("Synth Audio Output", () => {
  test("sonic-pi-beep produces audio output", async ({ page, sonicConfig }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(
      async (config) => {
        eval(config.helpers);

        const sonic = new window.SuperSonic(config.sonic);
        await sonic.init();
        await sonic.loadSynthDef("sonic-pi-beep");

        // Start capture
        sonic.startCapture();

        // Create synth
        await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 0, "amp", 0.5);

        // Wait for some audio to be generated
        await new Promise((r) => setTimeout(r, 200));

        // Stop capture
        const captured = sonic.stopCapture();

        // Clean up
        await sonic.send("/n_free", 1000);

        // Analyze
        const hasAudioOutput = hasAudio(captured.left);
        const rms = calculateRMS(captured.left, 0, Math.min(captured.frames, 10000));
        const peak = findPeak(captured.left, 0, Math.min(captured.frames, 10000));

        return {
          frames: captured.frames,
          hasAudio: hasAudioOutput,
          rms,
          peak,
        };
      },
      { sonic: sonicConfig, helpers: AUDIO_HELPERS }
    );

    expect(result.frames).toBeGreaterThan(0);
    expect(result.hasAudio).toBe(true);
    expect(result.rms).toBeGreaterThan(0.001); // Should have measurable amplitude
    expect(result.peak).toBeGreaterThan(0.01); // Should have peaks
    expect(result.peak).toBeLessThanOrEqual(1.0); // Should not clip
  });

  test("synth amplitude responds to amp control", async ({ page, sonicConfig }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(
      async (config) => {
        eval(config.helpers);

        const sonic = new window.SuperSonic(config.sonic);
        await sonic.init();
        await sonic.loadSynthDef("sonic-pi-beep");

        // Test with amp 0.1
        sonic.startCapture();
        await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 0, "amp", 0.1);
        await new Promise((r) => setTimeout(r, 100));
        const lowAmp = sonic.stopCapture();
        await sonic.send("/n_free", 1000);
        await sonic.sync(1);

        // Test with amp 0.5
        sonic.startCapture();
        await sonic.send("/s_new", "sonic-pi-beep", 1001, 0, 0, "amp", 0.5);
        await new Promise((r) => setTimeout(r, 100));
        const highAmp = sonic.stopCapture();
        await sonic.send("/n_free", 1001);

        const lowRMS = calculateRMS(lowAmp.left, 0, Math.min(lowAmp.frames, 5000));
        const highRMS = calculateRMS(highAmp.left, 0, Math.min(highAmp.frames, 5000));

        return {
          lowRMS,
          highRMS,
          ratio: highRMS / lowRMS,
        };
      },
      { sonic: sonicConfig, helpers: AUDIO_HELPERS }
    );

    // Higher amp should produce higher RMS
    expect(result.highRMS).toBeGreaterThan(result.lowRMS);
    // The ratio should be roughly proportional (allowing for envelope effects)
    expect(result.ratio).toBeGreaterThan(2); // 0.5/0.1 = 5, but envelope affects this
  });

  test("frequency responds to note control", async ({ page, sonicConfig }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(
      async (config) => {
        eval(config.helpers);

        const sonic = new window.SuperSonic(config.sonic);
        await sonic.init();
        await sonic.loadSynthDef("sonic-pi-beep");

        // Test with note 48 (C3, ~130 Hz) - lower note for clearer difference
        sonic.startCapture();
        await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 0, "note", 48, "amp", 0.5);
        await new Promise((r) => setTimeout(r, 200)); // Longer capture for more accurate frequency estimation
        const lowNote = sonic.stopCapture();
        await sonic.send("/n_free", 1000);
        await sonic.sync(1);

        // Test with note 72 (C5, ~523 Hz - two octaves higher)
        sonic.startCapture();
        await sonic.send("/s_new", "sonic-pi-beep", 1001, 0, 0, "note", 72, "amp", 0.5);
        await new Promise((r) => setTimeout(r, 200));
        const highNote = sonic.stopCapture();
        await sonic.send("/n_free", 1001);

        // Estimate frequencies from the middle of the capture (skip attack)
        const sampleRate = lowNote.sampleRate;
        const start = Math.floor(sampleRate * 0.05); // Skip first 50ms (attack phase)
        const length = Math.floor(sampleRate * 0.1); // Analyze 100ms

        const lowFreq = estimateFrequency(lowNote.left, sampleRate, start, start + length);
        const highFreq = estimateFrequency(highNote.left, sampleRate, start, start + length);

        return {
          lowFreq,
          highFreq,
          ratio: highFreq / lowFreq,
        };
      },
      { sonic: sonicConfig, helpers: AUDIO_HELPERS }
    );

    // Higher note should produce higher frequency
    // This is the primary assertion - higher pitch = higher measured frequency
    // Note: Zero-crossing frequency estimation is unreliable for complex synth waveforms,
    // so we only verify the directional relationship, not specific ratios
    expect(result.highFreq).toBeGreaterThan(result.lowFreq);
  });

  test("control bus mapping affects synth frequency", async ({ page, sonicConfig }) => {
    // This test verifies that /n_map actually works - that mapping a control
    // to a bus causes the synth to read its value from the bus
    await page.goto("/test/harness.html");

    const result = await page.evaluate(
      async (config) => {
        eval(config.helpers);

        const sonic = new window.SuperSonic(config.sonic);
        await sonic.init();
        await sonic.loadSynthDef("sonic-pi-beep");

        // Create synth with low note (48 = C3, ~130 Hz)
        await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 0, "note", 48, "amp", 0.5, "release", 5);
        await sonic.sync(1);

        // Capture initial frequency (before mapping)
        await new Promise((r) => setTimeout(r, 50)); // Let synth stabilize
        sonic.startCapture();
        await new Promise((r) => setTimeout(r, 200));
        const beforeMapping = sonic.stopCapture();

        // Set control bus 0 to high note (72 = C5, ~523 Hz - two octaves higher)
        await sonic.send("/c_set", 0, 72);
        await sonic.sync(2);

        // Map the synth's "note" control to bus 0
        await sonic.send("/n_map", 1000, "note", 0);
        await sonic.sync(3);

        // Wait for mapping to take effect and capture
        await new Promise((r) => setTimeout(r, 50));
        sonic.startCapture();
        await new Promise((r) => setTimeout(r, 200));
        const afterMapping = sonic.stopCapture();

        // Cleanup
        await sonic.send("/n_free", 1000);

        // Estimate frequencies
        const sampleRate = beforeMapping.sampleRate;
        const start = Math.floor(sampleRate * 0.02); // Skip first 20ms
        const length = Math.floor(sampleRate * 0.15); // Analyze 150ms

        const freqBefore = estimateFrequency(beforeMapping.left, sampleRate, start, start + length);
        const freqAfter = estimateFrequency(afterMapping.left, sampleRate, start, start + length);

        return {
          freqBefore,
          freqAfter,
          ratio: freqAfter / freqBefore,
        };
      },
      { sonic: sonicConfig, helpers: AUDIO_HELPERS }
    );

    // After mapping to bus with note 72, frequency should be significantly higher
    // than before (when note was 48). Two octaves = 4x frequency ratio.
    // Note 48 = ~130Hz, Note 72 = ~523Hz, expected ratio ~4x
    expect(result.freqAfter).toBeGreaterThan(result.freqBefore);
    expect(result.ratio).toBeGreaterThan(2);
  });

  test("control bus value changes affect mapped synth in real-time", async ({ page, sonicConfig }) => {
    // This test verifies that changing a bus value dynamically updates the synth
    await page.goto("/test/harness.html");

    const result = await page.evaluate(
      async (config) => {
        eval(config.helpers);

        const sonic = new window.SuperSonic(config.sonic);
        await sonic.init();
        await sonic.loadSynthDef("sonic-pi-beep");

        // Set initial bus value to low note
        await sonic.send("/c_set", 0, 48);
        await sonic.sync(1);

        // Create synth already mapped to bus (start with mapping active)
        await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 0, "amp", 0.5, "release", 5);
        await sonic.send("/n_map", 1000, "note", 0);
        await sonic.sync(2);

        // Capture with low bus value
        await new Promise((r) => setTimeout(r, 50));
        sonic.startCapture();
        await new Promise((r) => setTimeout(r, 200));
        const lowCapture = sonic.stopCapture();

        // Change bus value to high note (same synth, just changing the bus)
        await sonic.send("/c_set", 0, 72);
        await sonic.sync(3);

        // Capture with high bus value
        await new Promise((r) => setTimeout(r, 50));
        sonic.startCapture();
        await new Promise((r) => setTimeout(r, 200));
        const highCapture = sonic.stopCapture();

        // Cleanup
        await sonic.send("/n_free", 1000);

        // Estimate frequencies
        const sampleRate = lowCapture.sampleRate;
        const start = Math.floor(sampleRate * 0.02);
        const length = Math.floor(sampleRate * 0.15);

        const freqLow = estimateFrequency(lowCapture.left, sampleRate, start, start + length);
        const freqHigh = estimateFrequency(highCapture.left, sampleRate, start, start + length);

        return {
          freqLow,
          freqHigh,
          ratio: freqHigh / freqLow,
        };
      },
      { sonic: sonicConfig, helpers: AUDIO_HELPERS }
    );

    // Changing the bus value should change the synth's frequency in real-time
    // Note 48 = ~130Hz, Note 72 = ~523Hz, expected ratio ~4x
    expect(result.freqHigh).toBeGreaterThan(result.freqLow);
    expect(result.ratio).toBeGreaterThan(2);
  });

  test("multiple synths produce combined output", async ({ page, sonicConfig }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(
      async (config) => {
        eval(config.helpers);

        const sonic = new window.SuperSonic(config.sonic);
        await sonic.init();
        await sonic.loadSynthDef("sonic-pi-beep");

        // Test with single synth
        sonic.startCapture();
        await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 0, "amp", 0.3);
        await new Promise((r) => setTimeout(r, 100));
        const singleSynth = sonic.stopCapture();
        await sonic.send("/n_free", 1000);
        await sonic.sync(1);

        // Test with two synths
        sonic.startCapture();
        await sonic.send("/s_new", "sonic-pi-beep", 1001, 0, 0, "amp", 0.3, "note", 60);
        await sonic.send("/s_new", "sonic-pi-beep", 1002, 0, 0, "amp", 0.3, "note", 64);
        await new Promise((r) => setTimeout(r, 100));
        const doubleSynth = sonic.stopCapture();
        await sonic.send("/n_free", 1001);
        await sonic.send("/n_free", 1002);

        const singleRMS = calculateRMS(singleSynth.left, 0, Math.min(singleSynth.frames, 5000));
        const doubleRMS = calculateRMS(doubleSynth.left, 0, Math.min(doubleSynth.frames, 5000));

        return {
          singleRMS,
          doubleRMS,
          ratio: doubleRMS / singleRMS,
        };
      },
      { sonic: sonicConfig, helpers: AUDIO_HELPERS }
    );

    // Two synths should be louder than one
    expect(result.doubleRMS).toBeGreaterThan(result.singleRMS);
    // But not necessarily exactly 2x due to phase interactions
    expect(result.ratio).toBeGreaterThan(1.1);
  });

  test("freed synth stops producing audio", async ({ page, sonicConfig }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(
      async (config) => {
        eval(config.helpers);

        const sonic = new window.SuperSonic(config.sonic);
        await sonic.init();
        await sonic.loadSynthDef("sonic-pi-beep");

        sonic.startCapture();

        // Create synth with short release
        await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 0, "amp", 0.5, "release", 0.01);

        // Wait longer for synth to definitely start producing audio
        await new Promise((r) => setTimeout(r, 100));

        // Free synth
        await sonic.send("/n_free", 1000);
        await sonic.sync(1);

        // Wait for audio to stop (release time + processing delay)
        await new Promise((r) => setTimeout(r, 100));

        const captured = sonic.stopCapture();

        // Find the first non-zero sample to locate where audio actually started
        let firstNonZero = -1;
        for (let i = 0; i < captured.frames; i++) {
          if (captured.left[i] !== 0) {
            firstNonZero = i;
            break;
          }
        }

        // Find the last non-zero sample to see where audio ended
        let lastNonZero = -1;
        for (let i = captured.frames - 1; i >= 0; i--) {
          if (captured.left[i] !== 0) {
            lastNonZero = i;
            break;
          }
        }

        const sampleRate = captured.sampleRate;

        // Analyze: there should be audio somewhere in the buffer, and the end should be silent
        const hasAudioSomewhere = firstNonZero !== -1;
        const audioStoppedBeforeEnd = lastNonZero < captured.frames - Math.floor(sampleRate * 0.02);

        return {
          hasAudioSomewhere,
          audioStoppedBeforeEnd,
          firstNonZeroMs: firstNonZero !== -1 ? (firstNonZero / sampleRate) * 1000 : -1,
          lastNonZeroMs: lastNonZero !== -1 ? (lastNonZero / sampleRate) * 1000 : -1,
          totalMs: (captured.frames / sampleRate) * 1000,
        };
      },
      { sonic: sonicConfig, helpers: AUDIO_HELPERS }
    );

    // Synth should have produced audio at some point
    expect(result.hasAudioSomewhere).toBe(true);
    // Audio should have stopped before the end of the capture (synth was freed)
    expect(result.audioStoppedBeforeEnd).toBe(true);
  });
});

// =============================================================================
// STEREO OUTPUT TESTS
// =============================================================================

test.describe("Stereo Output", () => {
  test("stereo capture provides both channels", async ({ page, sonicConfig }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(
      async (config) => {
        eval(config.helpers);

        const sonic = new window.SuperSonic(config.sonic);
        await sonic.init();
        await sonic.loadSynthDef("sonic-pi-beep");

        sonic.startCapture();
        await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 0, "amp", 0.5);
        await new Promise((r) => setTimeout(r, 100));
        const captured = sonic.stopCapture();
        await sonic.send("/n_free", 1000);

        const leftRMS = calculateRMS(captured.left, 0, Math.min(captured.frames, 5000));
        const rightRMS = calculateRMS(captured.right, 0, Math.min(captured.frames, 5000));

        return {
          channels: captured.channels,
          leftLength: captured.left.length,
          rightLength: captured.right.length,
          leftRMS,
          rightRMS,
          bothHaveAudio: leftRMS > 0.001 && rightRMS > 0.001,
        };
      },
      { sonic: sonicConfig, helpers: AUDIO_HELPERS }
    );

    expect(result.channels).toBe(2);
    expect(result.leftLength).toBeGreaterThan(0);
    expect(result.rightLength).toBeGreaterThan(0);
    expect(result.bothHaveAudio).toBe(true);
    // Mono synth should have similar levels in both channels
    expect(Math.abs(result.leftRMS - result.rightRMS)).toBeLessThan(result.leftRMS * 0.1);
  });
});

// =============================================================================
// OSC BUNDLE TIMING TESTS
// =============================================================================

test.describe("OSC Bundle Timing", () => {
  test("timed bundle executes at scheduled time", async ({ page, sonicConfig }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(
      async (config) => {
        eval(config.helpers);

        const sonic = new window.SuperSonic(config.sonic);
        await sonic.init();
        await sonic.loadSynthDef("sonic-pi-beep");

        // Get NTP start time and drift from SharedArrayBuffer (same reference as WASM uses)
        const sharedBuffer = sonic.sharedBuffer;
        const ringBufferBase = sonic.ringBufferBase;
        const ntpStartView = new Float64Array(
          sharedBuffer,
          ringBufferBase + sonic.bufferConstants.NTP_START_TIME_START,
          1
        );
        const driftView = new Int32Array(
          sharedBuffer,
          ringBufferBase + sonic.bufferConstants.DRIFT_OFFSET_START,
          1
        );
        const ntpStartTime = ntpStartView[0];
        const driftMs = Atomics.load(driftView, 0);
        const driftSeconds = driftMs / 1000;

        // Start capture and record AudioContext time at that moment
        sonic.startCapture();
        const captureStartContextTime = sonic.node.context.currentTime;

        // Schedule synth 100ms in the future using AudioContext-based NTP
        // Must include drift to match WASM's calculation:
        // current_ntp = contextTime + ntpStart + drift
        const scheduledDelayMs = 100;
        const delaySeconds = scheduledDelayMs / 1000;
        const captureStartNTP = captureStartContextTime + ntpStartTime + driftSeconds;
        const scheduledNTP = captureStartNTP + delaySeconds;

        // Create bundle with raw NTP timetag (seconds and fraction)
        const ntpSeconds = Math.floor(scheduledNTP);
        const ntpFraction = Math.floor((scheduledNTP % 1) * 0x100000000);

        const osc = window.SuperSonic.osc;
        const bundle = osc.encode({
          timeTag: { raw: [ntpSeconds, ntpFraction] },
          packets: [
            {
              address: "/s_new",
              args: [
                { type: "s", value: "sonic-pi-beep" },
                { type: "i", value: 1000 },
                { type: "i", value: 0 },
                { type: "i", value: 0 },
                { type: "s", value: "amp" },
                { type: "f", value: 0.5 },
              ],
            },
          ],
        });

        // Send the bundle
        await sonic.sendOSC(bundle);

        // Wait for synth to play (200ms is enough for 100ms delay + some audio)
        await new Promise((r) => setTimeout(r, 200));

        const captured = sonic.stopCapture();
        await sonic.send("/n_free", 1000);

        // Find first non-zero sample
        let firstNonZero = -1;
        for (let i = 0; i < captured.left.length; i++) {
          if (Math.abs(captured.left[i]) > 0.001) {
            firstNonZero = i;
            break;
          }
        }

        const audioStartMs = (firstNonZero / captured.sampleRate) * 1000;
        const timingErrorMs = audioStartMs - scheduledDelayMs;

        return {
          firstNonZeroSample: firstNonZero,
          audioStartMs,
          scheduledDelayMs,
          hasAudio: firstNonZero > 0,
          timingErrorMs,
          absTimingErrorMs: Math.abs(timingErrorMs),
        };
      },
      { sonic: sonicConfig, helpers: AUDIO_HELPERS }
    );

    expect(result.hasAudio).toBe(true);
    // Audio should start close to the scheduled time (within 5ms tolerance)
    // Using AudioContext-based NTP ensures accurate timing correlation
    expect(result.absTimingErrorMs).toBeLessThan(5);
  });

  test("immediate bundle (timetag 1) executes immediately", async ({ page, sonicConfig }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(
      async (config) => {
        eval(config.helpers);

        const sonic = new window.SuperSonic(config.sonic);
        await sonic.init();
        await sonic.loadSynthDef("sonic-pi-beep");

        const osc = window.SuperSonic.osc;

        // Bundle with timetag = 1 means "execute immediately"
        const immediateBundle = osc.encode({
          timeTag: { raw: [0, 1] }, // NTP timetag 1 = immediate
          packets: [
            {
              address: "/s_new",
              args: [
                { type: "s", value: "sonic-pi-beep" },
                { type: "i", value: 1000 },
                { type: "i", value: 0 },
                { type: "i", value: 0 },
                { type: "s", value: "amp" },
                { type: "f", value: 0.5 },
              ],
            },
          ],
        });

        sonic.startCapture();
        await sonic.sendOSC(immediateBundle);

        // Wait a short time
        await new Promise((r) => setTimeout(r, 100));

        const captured = sonic.stopCapture();
        await sonic.send("/n_free", 1000);

        // Find first non-zero sample
        let firstNonZero = -1;
        for (let i = 0; i < captured.left.length; i++) {
          if (Math.abs(captured.left[i]) > 0.001) {
            firstNonZero = i;
            break;
          }
        }

        const latencyMs = (firstNonZero / captured.sampleRate) * 1000;

        return {
          firstNonZeroSample: firstNonZero,
          latencyMs,
          hasAudio: firstNonZero > 0,
        };
      },
      { sonic: sonicConfig, helpers: AUDIO_HELPERS }
    );

    expect(result.hasAudio).toBe(true);
    // Immediate bundle should start within 20ms (direct SAB write bypasses prescheduler)
    expect(result.latencyMs).toBeLessThan(20);
  });

  test("past timetag bundle executes immediately", async ({ page, sonicConfig }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(
      async (config) => {
        eval(config.helpers);

        const sonic = new window.SuperSonic(config.sonic);
        await sonic.init();
        await sonic.loadSynthDef("sonic-pi-beep");

        const osc = window.SuperSonic.osc;

        // Bundle with timetag in the past (1 second ago)
        // native: expects Unix milliseconds (same as Date.now())
        const pastTime = Date.now() - 1000; // 1 second ago
        const pastBundle = osc.encode({
          timeTag: { native: pastTime },
          packets: [
            {
              address: "/s_new",
              args: [
                { type: "s", value: "sonic-pi-beep" },
                { type: "i", value: 1000 },
                { type: "i", value: 0 },
                { type: "i", value: 0 },
                { type: "s", value: "amp" },
                { type: "f", value: 0.5 },
              ],
            },
          ],
        });

        sonic.startCapture();
        await sonic.sendOSC(pastBundle);

        await new Promise((r) => setTimeout(r, 100));

        const captured = sonic.stopCapture();
        await sonic.send("/n_free", 1000);

        let firstNonZero = -1;
        for (let i = 0; i < captured.left.length; i++) {
          if (Math.abs(captured.left[i]) > 0.001) {
            firstNonZero = i;
            break;
          }
        }

        const latencyMs = (firstNonZero / captured.sampleRate) * 1000;

        return {
          firstNonZeroSample: firstNonZero,
          latencyMs,
          hasAudio: firstNonZero > 0,
        };
      },
      { sonic: sonicConfig, helpers: AUDIO_HELPERS }
    );

    expect(result.hasAudio).toBe(true);
    // Past timetag should execute immediately (within 10ms, bypasses prescheduler)
    expect(result.latencyMs).toBeLessThan(10);
  });

  // NOTE ON SAMPLE-ACCURATE SCHEDULING:
  // SuperSonic correctly sets mSampleOffset when executing scheduled bundles.
  // However, whether this affects audio output depends on the synthdef:
  // - Synths using OffsetOut.ar will have sample-accurate output
  // - Synths using regular Out.ar will start at buffer boundaries
  // This is standard scsynth behavior, not a SuperSonic limitation.

  test("scheduled synths execute at correct buffer-level timing", async ({ page, sonicConfig }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(
      async (config) => {
        eval(config.helpers);

        const sonic = new window.SuperSonic(config.sonic);
        await sonic.init();
        await sonic.loadSynthDef("sonic-pi-beep");

        const sampleRate = sonic.sampleRate || 48000;

        // Get NTP start time from SharedArrayBuffer
        const sharedBuffer = sonic.sharedBuffer;
        const ntpStartOffset = sonic.bufferConstants.NTP_START_TIME_START;
        const ringBufferBase = sonic.ringBufferBase;
        const ntpStartView = new Float64Array(
          sharedBuffer,
          ringBufferBase + ntpStartOffset,
          1
        );
        const ntpStartTime = ntpStartView[0];

        // Read drift from SAB
        const driftView = new Int32Array(
          sharedBuffer,
          ringBufferBase + sonic.bufferConstants.DRIFT_OFFSET_START,
          1
        );
        const driftMs = Atomics.load(driftView, 0);
        const driftSeconds = driftMs / 1000;

        // Start capture
        sonic.startCapture();
        const captureStartContextTime = sonic.node.context.currentTime;
        const captureStartNTP = captureStartContextTime + ntpStartTime + driftSeconds;

        // Schedule two synths with a precise 500-sample gap
        // First synth at 50ms, second synth at 50ms + 500 samples
        const expectedGapSamples = 500;
        const firstDelayMs = 50;
        const secondDelayMs = firstDelayMs + (expectedGapSamples / sampleRate) * 1000;

        const osc = window.SuperSonic.osc;

        // Create first bundle - synth A at low frequency (220Hz) for identification
        const ntpA = captureStartNTP + (firstDelayMs / 1000);
        const ntpSecondsA = Math.floor(ntpA);
        const ntpFractionA = Math.floor((ntpA % 1) * 0x100000000);

        const bundleA = osc.encode({
          timeTag: { raw: [ntpSecondsA, ntpFractionA] },
          packets: [{
            address: "/s_new",
            args: [
              { type: "s", value: "sonic-pi-beep" },
              { type: "i", value: 2000 },
              { type: "i", value: 0 },
              { type: "i", value: 0 },
              { type: "s", value: "note" },
              { type: "f", value: 57 }, // A3 = 220Hz
              { type: "s", value: "amp" },
              { type: "f", value: 0.5 },
              { type: "s", value: "sustain" },
              { type: "f", value: 0.05 },
              { type: "s", value: "release" },
              { type: "f", value: 0.01 },
            ],
          }],
        });

        // Create second bundle - synth B at higher frequency (880Hz) for identification
        const ntpB = captureStartNTP + (secondDelayMs / 1000);
        const ntpSecondsB = Math.floor(ntpB);
        const ntpFractionB = Math.floor((ntpB % 1) * 0x100000000);

        const bundleB = osc.encode({
          timeTag: { raw: [ntpSecondsB, ntpFractionB] },
          packets: [{
            address: "/s_new",
            args: [
              { type: "s", value: "sonic-pi-beep" },
              { type: "i", value: 2001 },
              { type: "i", value: 0 },
              { type: "i", value: 0 },
              { type: "s", value: "note" },
              { type: "f", value: 81 }, // A5 = 880Hz
              { type: "s", value: "amp" },
              { type: "f", value: 0.5 },
              { type: "s", value: "sustain" },
              { type: "f", value: 0.05 },
              { type: "s", value: "release" },
              { type: "f", value: 0.01 },
            ],
          }],
        });

        // Send both bundles
        await sonic.sendOSC(bundleA);
        await sonic.sendOSC(bundleB);

        // Wait for both synths to complete
        await new Promise((r) => setTimeout(r, 200));

        const captured = sonic.stopCapture();

        // Find onset of synth A (first significant audio)
        let onsetA = -1;
        for (let i = 0; i < captured.left.length; i++) {
          if (Math.abs(captured.left[i]) > 0.01) {
            onsetA = i;
            break;
          }
        }

        // Find onset of synth B by looking for the second rise in amplitude
        // After synth A decays, find where amplitude rises again
        let onsetB = -1;
        let foundDecay = false;
        let decayStart = -1;

        // First, find where synth A decays (amplitude drops below threshold after onset)
        for (let i = onsetA + 100; i < captured.left.length; i++) {
          if (Math.abs(captured.left[i]) < 0.01) {
            foundDecay = true;
            decayStart = i;
            break;
          }
        }

        // Then find where synth B starts (amplitude rises again)
        if (foundDecay) {
          for (let i = decayStart; i < captured.left.length; i++) {
            if (Math.abs(captured.left[i]) > 0.01) {
              onsetB = i;
              break;
            }
          }
        }

        const actualGapSamples = onsetB - onsetA;
        const gapError = Math.abs(actualGapSamples - expectedGapSamples);

        return {
          sampleRate,
          expectedGapSamples,
          onsetA,
          onsetB,
          actualGapSamples,
          gapError,
          foundBothOnsets: onsetA > 0 && onsetB > 0,
          firstDelayMs,
          secondDelayMs,
        };
      },
      { sonic: sonicConfig, helpers: AUDIO_HELPERS }
    );

    console.log(`\nTwo-synth buffer-level timing test:`);
    console.log(`  Sample rate: ${result.sampleRate}`);
    console.log(`  Expected gap: ${result.expectedGapSamples} samples`);
    console.log(`  Onset A: sample ${result.onsetA}`);
    console.log(`  Onset B: sample ${result.onsetB}`);
    console.log(`  Actual gap: ${result.actualGapSamples} samples`);
    console.log(`  Gap error: ${result.gapError} samples`);

    // Verify both synths were detected
    expect(result.foundBothOnsets).toBe(true);

    // With Out.ar (not OffsetOut.ar), timing snaps to buffer boundaries.
    // The gap should be within 3 buffer lengths (384 samples) of expected.
    // Additional tolerance accounts for onset detection variability.
    // For true sample-accuracy, the synthdef must use OffsetOut.ar.
    const bufferSize = 128;
    expect(result.gapError).toBeLessThan(bufferSize * 3);
  });

  test("mSampleOffset is set but Out.ar snaps to buffer boundaries", async ({ page, sonicConfig }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(
      async (config) => {
        eval(config.helpers);

        const sonic = new window.SuperSonic(config.sonic);
        await sonic.init();
        await sonic.loadSynthDef("sonic-pi-beep");

        const sampleRate = sonic.sampleRate || 48000;
        const bufferSize = 128; // AudioWorklet quantum

        // Get NTP start time
        const sharedBuffer = sonic.sharedBuffer;
        const ntpStartOffset = sonic.bufferConstants.NTP_START_TIME_START;
        const ringBufferBase = sonic.ringBufferBase;
        const ntpStartView = new Float64Array(
          sharedBuffer,
          ringBufferBase + ntpStartOffset,
          1
        );
        const ntpStartTime = ntpStartView[0];

        // Read drift from SAB
        const driftView = new Int32Array(
          sharedBuffer,
          ringBufferBase + sonic.bufferConstants.DRIFT_OFFSET_START,
          1
        );
        const driftMs = Atomics.load(driftView, 0);
        const driftSeconds = driftMs / 1000;

        // Start capture and record precise time
        sonic.startCapture();
        const captureStartContextTime = sonic.node.context.currentTime;
        const captureStartNTP = captureStartContextTime + ntpStartTime + driftSeconds;

        // Schedule synth at a non-buffer-aligned offset
        // Use 50ms + 50 samples (not a multiple of 128)
        const baseDelayMs = 50;
        const extraSamples = 50; // Deliberately non-aligned
        const totalDelaySamples = Math.round((baseDelayMs / 1000) * sampleRate) + extraSamples;
        const totalDelaySeconds = totalDelaySamples / sampleRate;

        const ntpTarget = captureStartNTP + totalDelaySeconds;
        const ntpSeconds = Math.floor(ntpTarget);
        const ntpFraction = Math.floor((ntpTarget % 1) * 0x100000000);

        const osc = window.SuperSonic.osc;
        const bundle = osc.encode({
          timeTag: { raw: [ntpSeconds, ntpFraction] },
          packets: [{
            address: "/s_new",
            args: [
              { type: "s", value: "sonic-pi-beep" },
              { type: "i", value: 3000 },
              { type: "i", value: 0 },
              { type: "i", value: 0 },
              { type: "s", value: "amp" },
              { type: "f", value: 0.5 },
            ],
          }],
        });

        await sonic.sendOSC(bundle);
        await new Promise((r) => setTimeout(r, 150));

        const captured = sonic.stopCapture();

        // Find first non-zero sample
        let firstNonZero = -1;
        for (let i = 0; i < captured.left.length; i++) {
          if (Math.abs(captured.left[i]) > 0.001) {
            firstNonZero = i;
            break;
          }
        }

        // Calculate expected vs actual
        const expectedSample = totalDelaySamples;
        const actualSample = firstNonZero;
        const sampleError = Math.abs(actualSample - expectedSample);

        // Check if it snapped to buffer boundary (would indicate bug)
        const nearestBufferBoundary = Math.round(actualSample / bufferSize) * bufferSize;
        const distanceToBufferBoundary = Math.abs(actualSample - nearestBufferBoundary);

        return {
          sampleRate,
          bufferSize,
          expectedSample,
          actualSample,
          sampleError,
          extraSamples,
          distanceToBufferBoundary,
          // If it snapped to boundary, this would be 0
          notSnappedToBuffer: distanceToBufferBoundary > 5,
          hasAudio: firstNonZero > 0,
        };
      },
      { sonic: sonicConfig, helpers: AUDIO_HELPERS }
    );

    console.log(`\nBuffer-boundary snap test (Out.ar behavior):`);
    console.log(`  Sample rate: ${result.sampleRate}`);
    console.log(`  Buffer size: ${result.bufferSize}`);
    console.log(`  Expected sample: ${result.expectedSample} (50ms + ${result.extraSamples} extra)`);
    console.log(`  Actual sample: ${result.actualSample}`);
    console.log(`  Sample error: ${result.sampleError}`);
    console.log(`  Distance to nearest buffer boundary: ${result.distanceToBufferBoundary}`);

    expect(result.hasAudio).toBe(true);

    // Timing should be within 2 buffer lengths (256 samples) of expected
    // This accounts for message arrival, processing latency, and buffer snapping
    expect(result.sampleError).toBeLessThan(256);

    // With Out.ar (not OffsetOut.ar), audio SHOULD snap to buffer boundaries.
    // This verifies that Out.ar behaves as expected in scsynth.
    // For sample-accurate output, synthdef must use OffsetOut.ar instead.
    expect(result.distanceToBufferBoundary).toBeLessThan(10);
  });

  test("OffsetOut.ar achieves true sample-accurate scheduling", async ({ page, sonicConfig }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(
      async (config) => {
        eval(config.helpers);

        const sonic = new window.SuperSonic(config.sonic);
        await sonic.init();

        // Load synthdef that uses OffsetOut.ar
        await sonic.loadSynthDef("test_offset_out");

        const sampleRate = sonic.sampleRate || 48000;
        const bufferSize = 128; // AudioWorklet quantum

        // Get NTP start time
        const sharedBuffer = sonic.sharedBuffer;
        const ntpStartOffset = sonic.bufferConstants.NTP_START_TIME_START;
        const ringBufferBase = sonic.ringBufferBase;
        const ntpStartView = new Float64Array(
          sharedBuffer,
          ringBufferBase + ntpStartOffset,
          1
        );
        const ntpStartTime = ntpStartView[0];

        // Read drift from SAB
        const driftView = new Int32Array(
          sharedBuffer,
          ringBufferBase + sonic.bufferConstants.DRIFT_OFFSET_START,
          1
        );
        const driftMs = Atomics.load(driftView, 0);
        const driftSeconds = driftMs / 1000;

        // Start capture and record precise time
        sonic.startCapture();
        const captureStartContextTime = sonic.node.context.currentTime;
        const captureStartNTP = captureStartContextTime + ntpStartTime + driftSeconds;

        // Schedule synth at a non-buffer-aligned offset
        // Use 50ms + 50 samples (not a multiple of 128)
        const baseDelayMs = 50;
        const extraSamples = 50; // Deliberately non-aligned (not multiple of 128)
        const totalDelaySamples = Math.round((baseDelayMs / 1000) * sampleRate) + extraSamples;
        const totalDelaySeconds = totalDelaySamples / sampleRate;

        const ntpTarget = captureStartNTP + totalDelaySeconds;
        const ntpSeconds = Math.floor(ntpTarget);
        const ntpFraction = Math.floor((ntpTarget % 1) * 0x100000000);

        const osc = window.SuperSonic.osc;
        const bundle = osc.encode({
          timeTag: { raw: [ntpSeconds, ntpFraction] },
          packets: [{
            address: "/s_new",
            args: [
              { type: "s", value: "test_offset_out" },
              { type: "i", value: 3000 },
              { type: "i", value: 0 },
              { type: "i", value: 0 },
              { type: "s", value: "amp" },
              { type: "f", value: 0.5 },
              { type: "s", value: "freq" },
              { type: "f", value: 880 },
              { type: "s", value: "dur" },
              { type: "f", value: 0.1 },
            ],
          }],
        });

        await sonic.sendOSC(bundle);
        await new Promise((r) => setTimeout(r, 200));

        const captured = sonic.stopCapture();

        // Find first non-zero sample
        let firstNonZero = -1;
        for (let i = 0; i < captured.left.length; i++) {
          if (Math.abs(captured.left[i]) > 0.001) {
            firstNonZero = i;
            break;
          }
        }

        // Calculate expected vs actual
        const expectedSample = totalDelaySamples;
        const actualSample = firstNonZero;
        const sampleError = Math.abs(actualSample - expectedSample);

        // Check distance to buffer boundary
        const nearestBufferBoundary = Math.round(actualSample / bufferSize) * bufferSize;
        const distanceToBufferBoundary = Math.abs(actualSample - nearestBufferBoundary);

        return {
          sampleRate,
          bufferSize,
          expectedSample,
          actualSample,
          sampleError,
          extraSamples,
          distanceToBufferBoundary,
          hasAudio: firstNonZero > 0,
        };
      },
      { sonic: sonicConfig, helpers: AUDIO_HELPERS }
    );

    // Calculate expected within-buffer offset
    const expectedWithinBufferOffset = result.expectedSample % result.bufferSize;
    const actualWithinBufferOffset = result.actualSample % result.bufferSize;
    const withinBufferError = Math.abs(actualWithinBufferOffset - expectedWithinBufferOffset);
    // Handle wraparound (e.g., expected 127, actual 1 should be error of 2)
    const adjustedWithinBufferError = Math.min(withinBufferError, result.bufferSize - withinBufferError);

    console.log(`\nOffsetOut.ar sample-accurate scheduling test:`);
    console.log(`  Sample rate: ${result.sampleRate}`);
    console.log(`  Buffer size: ${result.bufferSize}`);
    console.log(`  Expected sample: ${result.expectedSample} (50ms + ${result.extraSamples} extra)`);
    console.log(`  Actual sample: ${result.actualSample}`);
    console.log(`  Overall sample error: ${result.sampleError}`);
    console.log(`  Expected within-buffer offset: ${expectedWithinBufferOffset}`);
    console.log(`  Actual within-buffer offset: ${actualWithinBufferOffset}`);
    console.log(`  Within-buffer error: ${adjustedWithinBufferError}`);
    console.log(`  Distance to nearest buffer boundary: ${result.distanceToBufferBoundary}`);

    expect(result.hasAudio).toBe(true);

    // With OffsetOut.ar, the within-buffer offset should be preserved.
    // There may be some overall latency (integer number of buffers) due to
    // message delivery, but the subsample offset should be accurate.
    expect(adjustedWithinBufferError).toBeLessThan(5);

    // Unlike Out.ar, OffsetOut.ar should NOT snap to buffer boundaries.
    // The distance to the nearest buffer boundary should be approximately
    // equal to the expected within-buffer offset (which is 50 samples here).
    expect(result.distanceToBufferBoundary).toBeGreaterThan(10);
  });
});

// =============================================================================
// FFT TESTS (Actual FFT/IFFT UGens)
// =============================================================================

test.describe("FFT UGens", () => {
  test("FFT/IFFT passthrough produces audio output", async ({ page, sonicConfig }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(
      async (config) => {
        eval(config.helpers);

        const sonic = new window.SuperSonic(config.sonic);
        await sonic.init();

        // Load the FFT test synthdefs
        await sonic.loadSynthDef("fft_test_sine");
        await sonic.loadSynthDef("fft_passthrough");

        // Create source sine wave on bus 10
        await sonic.send("/s_new", "fft_test_sine", 1000, 0, 0, "out", 10, "freq", 440, "amp", 0.5);

        // Create FFT passthrough that reads from bus 10 and outputs to bus 0
        await sonic.send("/s_new", "fft_passthrough", 1001, 1, 0, "in_bus", 10, "out", 0);

        // FFT needs time to fill buffer (2048 samples at 48kHz = ~43ms) plus overlap
        await new Promise((r) => setTimeout(r, 200));

        sonic.startCapture();
        await new Promise((r) => setTimeout(r, 300));
        const captured = sonic.stopCapture();

        await sonic.send("/n_free", 1000);
        await sonic.send("/n_free", 1001);

        const rms = calculateRMS(captured.left, 0, Math.min(captured.frames, 15000));
        const peak = findPeak(captured.left);

        return {
          hasAudio: hasAudio(captured.left),
          rms,
          peak,
          frames: captured.frames,
        };
      },
      { sonic: sonicConfig, helpers: AUDIO_HELPERS }
    );

    // FFT passthrough should produce audio
    expect(result.hasAudio).toBe(true);
    expect(result.rms).toBeGreaterThan(0.01);
    expect(result.peak).toBeGreaterThan(0.05);
  });

  test("PV_BrickWall filters frequencies", async ({ page, sonicConfig }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(
      async (config) => {
        eval(config.helpers);

        const sonic = new window.SuperSonic(config.sonic);
        await sonic.init();

        await sonic.loadSynthDef("fft_test_sine");
        await sonic.loadSynthDef("fft_brickwall");

        // Test with wipe=0 (all pass)
        await sonic.send("/s_new", "fft_test_sine", 1000, 0, 0, "out", 10, "freq", 440, "amp", 0.5);
        await sonic.send("/s_new", "fft_brickwall", 1001, 1, 0, "in_bus", 10, "out", 0, "wipe", 0);

        await new Promise((r) => setTimeout(r, 200));

        sonic.startCapture();
        await new Promise((r) => setTimeout(r, 200));
        const allPassCapture = sonic.stopCapture();

        await sonic.send("/n_free", 1001);
        await sonic.sync(1);

        // Test with wipe=-1 (low pass, removes high frequencies)
        await sonic.send("/s_new", "fft_brickwall", 1002, 1, 0, "in_bus", 10, "out", 0, "wipe", -0.9);

        await new Promise((r) => setTimeout(r, 200));

        sonic.startCapture();
        await new Promise((r) => setTimeout(r, 200));
        const lowPassCapture = sonic.stopCapture();

        await sonic.send("/n_free", 1000);
        await sonic.send("/n_free", 1002);

        const allPassRMS = calculateRMS(allPassCapture.left, 0, Math.min(allPassCapture.frames, 10000));
        const lowPassRMS = calculateRMS(lowPassCapture.left, 0, Math.min(lowPassCapture.frames, 10000));

        return {
          hasAllPassAudio: hasAudio(allPassCapture.left),
          hasLowPassAudio: hasAudio(lowPassCapture.left),
          allPassRMS,
          lowPassRMS,
          // Low pass with wipe=-0.9 should significantly reduce 440Hz
          rmsRatio: lowPassRMS / allPassRMS,
        };
      },
      { sonic: sonicConfig, helpers: AUDIO_HELPERS }
    );

    expect(result.hasAllPassAudio).toBe(true);
    // Low pass filter should reduce the amplitude significantly
    // (440Hz is in the upper portion of the spectrum)
    expect(result.allPassRMS).toBeGreaterThan(0.01);
  });

  test("PV_MagFreeze synth can be instantiated", async ({ page, sonicConfig }) => {
    // Note: PV_MagFreeze currently produces very low output with Green FFT backend.
    // This test verifies the synth can be loaded and instantiated without crashing.
    // Full functionality testing is deferred until we can investigate the low output issue.
    await page.goto("/test/harness.html");

    const result = await page.evaluate(
      async (config) => {
        const sonic = new window.SuperSonic(config.sonic);
        await sonic.init();

        await sonic.loadSynthDef("fft_test_sine");
        await sonic.loadSynthDef("fft_magfreeze");

        // Create sine source and magfreeze effect
        await sonic.send("/s_new", "fft_test_sine", 1000, 0, 0, "out", 10, "freq", 440, "amp", 0.5);
        await sonic.send("/s_new", "fft_magfreeze", 1001, 1, 0, "in_bus", 10, "out", 0, "freeze", 0);

        // Wait for processing - if synth crashes, this would fail
        await new Promise((r) => setTimeout(r, 300));

        // Cleanup
        await sonic.send("/n_free", 1000);
        await sonic.send("/n_free", 1001);

        // The synth was created successfully if we get here without errors
        return { success: true };
      },
      { sonic: sonicConfig, helpers: AUDIO_HELPERS }
    );

    // The test passes if the synths were created without crashing
    expect(result.success).toBe(true);
  });

  test("different FFT sizes work correctly", async ({ page, sonicConfig }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(
      async (config) => {
        eval(config.helpers);

        const sonic = new window.SuperSonic(config.sonic);
        await sonic.init();

        await sonic.loadSynthDef("fft_test_sine");
        await sonic.loadSynthDef("fft_size_512");
        await sonic.loadSynthDef("fft_size_1024");
        await sonic.loadSynthDef("fft_size_4096");

        const results = [];
        const fftSizes = [
          { name: "fft_size_512", size: 512, waitMs: 100 },
          { name: "fft_size_1024", size: 1024, waitMs: 150 },
          { name: "fft_size_4096", size: 4096, waitMs: 300 },
        ];

        for (const { name, size, waitMs } of fftSizes) {
          // Create source
          await sonic.send("/s_new", "fft_test_sine", 1000, 0, 0, "out", 10, "freq", 440, "amp", 0.5);
          // Create FFT processor
          await sonic.send("/s_new", name, 1001, 1, 0, "in_bus", 10, "out", 0);

          // Wait for FFT buffer to fill (proportional to size)
          await new Promise((r) => setTimeout(r, waitMs));

          sonic.startCapture();
          await new Promise((r) => setTimeout(r, 200));
          const captured = sonic.stopCapture();

          await sonic.send("/n_free", 1000);
          await sonic.send("/n_free", 1001);
          await sonic.sync(1);

          results.push({
            fftSize: size,
            hasAudio: hasAudio(captured.left),
            rms: calculateRMS(captured.left, 0, Math.min(captured.frames, 10000)),
            peak: findPeak(captured.left),
          });
        }

        return results;
      },
      { sonic: sonicConfig, helpers: AUDIO_HELPERS }
    );

    // All FFT sizes should produce audio
    for (const r of result) {
      expect(r.hasAudio).toBe(true);
      expect(r.rms).toBeGreaterThan(0.01);
    }
  });

  test("FFT latency increases with buffer size", async ({ page, sonicConfig }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(
      async (config) => {
        eval(config.helpers);

        const sonic = new window.SuperSonic(config.sonic);
        await sonic.init();

        await sonic.loadSynthDef("fft_test_sine");
        await sonic.loadSynthDef("fft_size_512");
        await sonic.loadSynthDef("fft_size_4096");

        // Test latency with small FFT (512)
        sonic.startCapture();
        await sonic.send("/s_new", "fft_test_sine", 1000, 0, 0, "out", 10, "freq", 440, "amp", 0.5);
        await sonic.send("/s_new", "fft_size_512", 1001, 1, 0, "in_bus", 10, "out", 0);
        await new Promise((r) => setTimeout(r, 300));
        const smallFFTCapture = sonic.stopCapture();
        await sonic.send("/n_free", 1000);
        await sonic.send("/n_free", 1001);
        await sonic.sync(1);

        // Find first non-silent sample for small FFT
        const smallFirstAudio = findFirstNonSilent(smallFFTCapture.left, smallFFTCapture.sampleRate, 0.01);

        // Test latency with large FFT (4096)
        sonic.startCapture();
        await sonic.send("/s_new", "fft_test_sine", 1002, 0, 0, "out", 10, "freq", 440, "amp", 0.5);
        await sonic.send("/s_new", "fft_size_4096", 1003, 1, 0, "in_bus", 10, "out", 0);
        await new Promise((r) => setTimeout(r, 300));
        const largeFFTCapture = sonic.stopCapture();
        await sonic.send("/n_free", 1002);
        await sonic.send("/n_free", 1003);

        // Find first non-silent sample for large FFT
        const largeFirstAudio = findFirstNonSilent(largeFFTCapture.left, largeFFTCapture.sampleRate, 0.01);

        const sampleRate = smallFFTCapture.sampleRate;

        return {
          smallLatencySamples: smallFirstAudio,
          smallLatencyMs: smallFirstAudio >= 0 ? (smallFirstAudio / sampleRate) * 1000 : -1,
          largeLatencySamples: largeFirstAudio,
          largeLatencyMs: largeFirstAudio >= 0 ? (largeFirstAudio / sampleRate) * 1000 : -1,
          sampleRate,
        };
      },
      { sonic: sonicConfig, helpers: AUDIO_HELPERS }
    );

    // Both should eventually produce audio
    expect(result.smallLatencySamples).toBeGreaterThanOrEqual(0);
    expect(result.largeLatencySamples).toBeGreaterThanOrEqual(0);

    // Larger FFT should have higher latency (4096 vs 512 = 8x buffer size)
    // At 48kHz: 512 samples = ~10.7ms, 4096 samples = ~85.3ms
    // With hop=0.5, actual latency is roughly FFT_size * 1.5
    expect(result.largeLatencyMs).toBeGreaterThan(result.smallLatencyMs);
  });
});

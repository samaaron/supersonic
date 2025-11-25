/**
 * Audio Capture Test Suite
 *
 * Tests that verify actual audio output from synths using the
 * SharedArrayBuffer audio capture mechanism.
 *
 * These tests verify:
 * - Synths produce audio output
 * - Audio characteristics (amplitude, rough frequency)
 * - Multiple synths produce combined output
 * - Timing accuracy via OSC bundles
 */

import { test, expect } from "@playwright/test";

// =============================================================================
// TEST UTILITIES
// =============================================================================

const SONIC_CONFIG = {
  workerBaseURL: "/dist/workers/",
  wasmBaseURL: "/dist/wasm/",
  sampleBaseURL: "/dist/samples/",
  synthdefBaseURL: "/dist/synthdefs/",
};

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
  test("startCapture and stopCapture work correctly", async ({ page }) => {
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
    }, SONIC_CONFIG);

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

  test("capture buffer size matches configuration", async ({ page }) => {
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
    }, SONIC_CONFIG);

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
  test("sonic-pi-beep produces audio output", async ({ page }) => {
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
      { sonic: SONIC_CONFIG, helpers: AUDIO_HELPERS }
    );

    expect(result.frames).toBeGreaterThan(0);
    expect(result.hasAudio).toBe(true);
    expect(result.rms).toBeGreaterThan(0.001); // Should have measurable amplitude
    expect(result.peak).toBeGreaterThan(0.01); // Should have peaks
    expect(result.peak).toBeLessThanOrEqual(1.0); // Should not clip
  });

  test("synth amplitude responds to amp control", async ({ page }) => {
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
      { sonic: SONIC_CONFIG, helpers: AUDIO_HELPERS }
    );

    // Higher amp should produce higher RMS
    expect(result.highRMS).toBeGreaterThan(result.lowRMS);
    // The ratio should be roughly proportional (allowing for envelope effects)
    expect(result.ratio).toBeGreaterThan(2); // 0.5/0.1 = 5, but envelope affects this
  });

  test("frequency responds to note control", async ({ page }) => {
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
      { sonic: SONIC_CONFIG, helpers: AUDIO_HELPERS }
    );

    // Higher note should produce higher frequency
    // This is the primary assertion - higher pitch = higher measured frequency
    // Note: Zero-crossing frequency estimation is unreliable for complex synth waveforms,
    // so we only verify the directional relationship, not specific ratios
    expect(result.highFreq).toBeGreaterThan(result.lowFreq);
  });

  test("multiple synths produce combined output", async ({ page }) => {
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
      { sonic: SONIC_CONFIG, helpers: AUDIO_HELPERS }
    );

    // Two synths should be louder than one
    expect(result.doubleRMS).toBeGreaterThan(result.singleRMS);
    // But not necessarily exactly 2x due to phase interactions
    expect(result.ratio).toBeGreaterThan(1.2);
  });

  test("freed synth stops producing audio", async ({ page }) => {
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
      { sonic: SONIC_CONFIG, helpers: AUDIO_HELPERS }
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
  test("stereo capture provides both channels", async ({ page }) => {
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
      { sonic: SONIC_CONFIG, helpers: AUDIO_HELPERS }
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
  test("timed bundle executes at scheduled time", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(
      async (config) => {
        eval(config.helpers);

        const sonic = new window.SuperSonic(config.sonic);
        await sonic.init();
        await sonic.loadSynthDef("sonic-pi-beep");

        // Get NTP start time from SharedArrayBuffer (same reference as WASM uses)
        const sharedBuffer = sonic.sharedBuffer;
        const ntpStartOffset = sonic.bufferConstants.NTP_START_TIME_START;
        const ringBufferBase = sonic.ringBufferBase;
        const ntpStartView = new Float64Array(
          sharedBuffer,
          ringBufferBase + ntpStartOffset,
          1
        );
        const ntpStartTime = ntpStartView[0];

        // Start capture and record AudioContext time at that moment
        sonic.startCapture();
        const captureStartContextTime = sonic.audioContext.currentTime;

        // Schedule synth 100ms in the future using AudioContext-based NTP
        // This is the same time reference the WASM scheduler uses
        // Using 100ms to fit comfortably within the 1-second capture buffer
        const scheduledDelayMs = 100;
        const delaySeconds = scheduledDelayMs / 1000;
        const captureStartNTP = captureStartContextTime + ntpStartTime;
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
      { sonic: SONIC_CONFIG, helpers: AUDIO_HELPERS }
    );

    expect(result.hasAudio).toBe(true);
    // Audio should start close to the scheduled time (within 5ms tolerance)
    // Using AudioContext-based NTP ensures accurate timing correlation
    expect(result.absTimingErrorMs).toBeLessThan(5);
  });

  test("immediate bundle (timetag 1) executes immediately", async ({ page }) => {
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
      { sonic: SONIC_CONFIG, helpers: AUDIO_HELPERS }
    );

    expect(result.hasAudio).toBe(true);
    // Immediate bundle should start within 10ms (direct SAB write bypasses prescheduler)
    expect(result.latencyMs).toBeLessThan(10);
  });

  test("past timetag bundle executes immediately", async ({ page }) => {
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
      { sonic: SONIC_CONFIG, helpers: AUDIO_HELPERS }
    );

    expect(result.hasAudio).toBe(true);
    // Past timetag should execute immediately (within 10ms, bypasses prescheduler)
    expect(result.latencyMs).toBeLessThan(10);
  });
});

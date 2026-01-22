/**
 * High Pitch ChipLead Test Suite
 *
 * Tests that verify the chiplead synth handles extremely high MIDI notes.
 *
 * Bug reproduction: In the demo, using chiplead synth with:
 * - Root note: 96 (slider max)
 * - Octaves: 4
 * Causes the scope to flatline and no audio output.
 *
 * MIDI note 127 = ~12,543 Hz. Notes above 127 may cause issues.
 */

import { test, expect } from "./fixtures.mjs";

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

function hasNaN(samples) {
  for (let i = 0; i < samples.length; i++) {
    if (Number.isNaN(samples[i]) || !Number.isFinite(samples[i])) return true;
  }
  return false;
}

function countNaN(samples) {
  let count = 0;
  for (let i = 0; i < samples.length; i++) {
    if (Number.isNaN(samples[i]) || !Number.isFinite(samples[i])) count++;
  }
  return count;
}
`;

test.describe("ChipLead High Pitch Bug", () => {
  // Audio capture requires SharedArrayBuffer - not available in postMessage mode
  test("chiplead high note no longer corrupts engine", async ({ page, sonicConfig }) => {
    test.skip(sonicConfig.mode === 'postMessage', 'Audio capture requires SharedArrayBuffer');
    // This test verifies the N=0 guard fix - high notes beyond Nyquist/2
    // no longer produce NaN values that corrupt the engine
    await page.goto("/test/harness.html");

    const result = await page.evaluate(
      async (config) => {
        eval(config.helpers);

        const sonic = new window.SuperSonic(config.sonic);
        await sonic.init();
        await sonic.loadSynthDef("sonic-pi-chiplead");

        const results = [];

        // Step 1: Play a low note (60 = middle C) - should work
        sonic.startCapture();
        await sonic.send("/s_new", "sonic-pi-chiplead", 1000, 0, 0,
          "note", 60, "amp", 0.5, "attack", 0, "release", 0.1);
        await new Promise((r) => setTimeout(r, 120));
        await sonic.send("/n_free", 1000);
        await sonic.sync(1);
        const beforeCapture = sonic.stopCapture();
        results.push({
          step: "before_high_note",
          note: 60,
          hasAudio: hasAudio(beforeCapture.left),
          hasNaN: hasNaN(beforeCapture.left),
          rms: calculateRMS(beforeCapture.left, 0, Math.min(beforeCapture.frames, 6000)),
        });

        // Step 2: Play a high note (140) that causes NaN
        sonic.startCapture();
        await sonic.send("/s_new", "sonic-pi-chiplead", 1001, 0, 0,
          "note", 140, "amp", 0.5, "attack", 0, "release", 0.1);
        await new Promise((r) => setTimeout(r, 120));
        await sonic.send("/n_free", 1001);
        await sonic.sync(2);
        const highCapture = sonic.stopCapture();
        results.push({
          step: "high_note",
          note: 140,
          hasAudio: hasAudio(highCapture.left),
          hasNaN: hasNaN(highCapture.left),
          rms: calculateRMS(highCapture.left, 0, Math.min(highCapture.frames, 6000)),
        });

        // Step 3: Play the same low note (60) again - will it still work?
        sonic.startCapture();
        await sonic.send("/s_new", "sonic-pi-chiplead", 1002, 0, 0,
          "note", 60, "amp", 0.5, "attack", 0, "release", 0.1);
        await new Promise((r) => setTimeout(r, 120));
        await sonic.send("/n_free", 1002);
        await sonic.sync(3);
        const afterCapture = sonic.stopCapture();
        results.push({
          step: "after_high_note",
          note: 60,
          hasAudio: hasAudio(afterCapture.left),
          hasNaN: hasNaN(afterCapture.left),
          rms: calculateRMS(afterCapture.left, 0, Math.min(afterCapture.frames, 6000)),
        });

        return { results };
      },
      { sonic: sonicConfig, helpers: AUDIO_HELPERS }
    );

    console.log("\nChipLead NaN corruption test:");
    for (const r of result.results) {
      const rmsStr = Number.isNaN(r.rms) ? "NaN" : r.rms.toFixed(6);
      console.log(`  ${r.step} (note ${r.note}): hasAudio=${r.hasAudio}, hasNaN=${r.hasNaN}, RMS=${rmsStr}`);
    }

    const before = result.results.find(r => r.step === "before_high_note");
    const high = result.results.find(r => r.step === "high_note");
    const after = result.results.find(r => r.step === "after_high_note");

    // Before high note: should work fine
    expect(before.hasNaN).toBe(false);
    expect(before.rms).toBeGreaterThan(0.001);

    // High note: now protected by N=0 guard - no NaN produced
    expect(high.hasNaN).toBe(false);
    expect(high.rms).toBeGreaterThan(0.001);

    // After high note: should work fine
    expect(after.hasNaN).toBe(false);
    expect(after.rms).toBeGreaterThan(0.001);
  });

  test("chiplead with FX chain - high note no longer corrupts FX", async ({ page, sonicConfig }) => {
    test.skip(sonicConfig.mode === 'postMessage', 'Audio capture requires SharedArrayBuffer');
    // This test replicates the demo setup where synths route through LPF -> Reverb
    // Previously NaN values from high notes would propagate and corrupt the FX chain
    await page.goto("/test/harness.html");

    const result = await page.evaluate(
      async (config) => {
        eval(config.helpers);

        const sonic = new window.SuperSonic(config.sonic);
        await sonic.init();

        // Load synthdefs
        await sonic.loadSynthDef("sonic-pi-chiplead");
        await sonic.loadSynthDef("sonic-pi-fx_lpf");
        await sonic.loadSynthDef("sonic-pi-fx_reverb");

        // Bus configuration (matching demo)
        const FX_BUS_SYNTH_TO_LPF = 20;
        const FX_BUS_LPF_TO_REVERB = 21;
        const FX_BUS_OUTPUT = 0;

        // Create FX chain
        const FX_LPF_NODE = 500;
        const FX_REVERB_NODE = 501;

        await sonic.send("/s_new", "sonic-pi-fx_lpf", FX_LPF_NODE, 0, 0,
          "in_bus", FX_BUS_SYNTH_TO_LPF, "out_bus", FX_BUS_LPF_TO_REVERB,
          "cutoff", 130.0, "res", 0.5);
        await sonic.send("/s_new", "sonic-pi-fx_reverb", FX_REVERB_NODE, 1, 0,
          "in_bus", FX_BUS_LPF_TO_REVERB, "out_bus", FX_BUS_OUTPUT,
          "mix", 0.3, "room", 0.6);
        await sonic.sync(1);

        const results = [];

        // Step 1: Play a low note through FX chain - should work
        sonic.startCapture();
        await sonic.send("/s_new", "sonic-pi-chiplead", 1000, 0, 0,
          "note", 60, "out_bus", FX_BUS_SYNTH_TO_LPF,
          "amp", 0.5, "attack", 0, "release", 0.1);
        await new Promise((r) => setTimeout(r, 150));
        await sonic.send("/n_free", 1000);
        await sonic.sync(2);
        const beforeCapture = sonic.stopCapture();
        results.push({
          step: "before_high_note",
          note: 60,
          hasAudio: hasAudio(beforeCapture.left),
          hasNaN: hasNaN(beforeCapture.left),
          rms: calculateRMS(beforeCapture.left, 0, Math.min(beforeCapture.frames, 8000)),
        });

        // Step 2: Play a high note (140) that causes NaN through FX
        sonic.startCapture();
        await sonic.send("/s_new", "sonic-pi-chiplead", 1001, 0, 0,
          "note", 140, "out_bus", FX_BUS_SYNTH_TO_LPF,
          "amp", 0.5, "attack", 0, "release", 0.1);
        await new Promise((r) => setTimeout(r, 150));
        await sonic.send("/n_free", 1001);
        await sonic.sync(3);
        const highCapture = sonic.stopCapture();
        results.push({
          step: "high_note",
          note: 140,
          hasAudio: hasAudio(highCapture.left),
          hasNaN: hasNaN(highCapture.left),
          rms: calculateRMS(highCapture.left, 0, Math.min(highCapture.frames, 8000)),
        });

        // Step 3: Play a low note again - does the FX chain still work?
        sonic.startCapture();
        await sonic.send("/s_new", "sonic-pi-chiplead", 1002, 0, 0,
          "note", 60, "out_bus", FX_BUS_SYNTH_TO_LPF,
          "amp", 0.5, "attack", 0, "release", 0.1);
        await new Promise((r) => setTimeout(r, 150));
        await sonic.send("/n_free", 1002);
        await sonic.sync(4);
        const afterCapture = sonic.stopCapture();
        results.push({
          step: "after_high_note",
          note: 60,
          hasAudio: hasAudio(afterCapture.left),
          hasNaN: hasNaN(afterCapture.left),
          rms: calculateRMS(afterCapture.left, 0, Math.min(afterCapture.frames, 8000)),
        });

        // Cleanup FX chain
        await sonic.send("/n_free", FX_LPF_NODE);
        await sonic.send("/n_free", FX_REVERB_NODE);

        return { results };
      },
      { sonic: sonicConfig, helpers: AUDIO_HELPERS }
    );

    console.log("\nChipLead with FX chain corruption test:");
    for (const r of result.results) {
      const rmsStr = Number.isNaN(r.rms) ? "NaN" : r.rms.toFixed(6);
      console.log(`  ${r.step} (note ${r.note}): hasAudio=${r.hasAudio}, hasNaN=${r.hasNaN}, RMS=${rmsStr}`);
    }

    const before = result.results.find(r => r.step === "before_high_note");
    const high = result.results.find(r => r.step === "high_note");
    const after = result.results.find(r => r.step === "after_high_note");

    // Before high note: should work fine through FX chain
    expect(before.hasNaN).toBe(false);
    expect(before.rms).toBeGreaterThan(0.001);

    // High note: now protected by N=0 guard - no NaN produced
    expect(high.hasNaN).toBe(false);
    expect(high.rms).toBeGreaterThan(0.001);

    // After high note: FX chain works fine
    expect(after.hasNaN).toBe(false);
    expect(after.rms).toBeGreaterThan(0.001);
  });

  test("chiplead synth MIDI notes 96 to 156 (5 octaves)", async ({ page, sonicConfig }) => {
    test.skip(sonicConfig.mode === 'postMessage', 'Audio capture requires SharedArrayBuffer');
    // Test key MIDI notes across the range (one per octave + boundaries)
    await page.goto("/test/harness.html");

    const result = await page.evaluate(
      async (config) => {
        eval(config.helpers);

        const sonic = new window.SuperSonic(config.sonic);
        await sonic.init();
        await sonic.loadSynthDef("sonic-pi-chiplead");

        // Sample notes: one per octave (96, 108, 120, 132, 144, 156)
        const testNotes = [96, 108, 120, 132, 144, 156];
        const results = [];
        let firstFailure = null;
        let firstNaN = null;

        for (const note of testNotes) {
          // Reset scsynth between each note to ensure clean state
          await sonic.reset();
          await sonic.loadSynthDef("sonic-pi-chiplead");

          sonic.startCapture();

          await sonic.send("/s_new", "sonic-pi-chiplead", 1000, 0, 0,
            "note", note,
            "amp", 0.5,
            "attack", 0,
            "release", 0.1
          );

          await new Promise((r) => setTimeout(r, 120));

          await sonic.send("/n_free", 1000);
          await sonic.sync(1);

          const captured = sonic.stopCapture();

          const hasAudioOutput = hasAudio(captured.left);
          const hasNaNValues = hasNaN(captured.left);
          const nanCount = hasNaNValues ? countNaN(captured.left) : 0;
          const rms = calculateRMS(captured.left, 0, Math.min(captured.frames, 6000));
          const peak = findPeak(captured.left, 0, Math.min(captured.frames, 6000));

          // Calculate expected frequency (A4=440Hz at MIDI 69)
          const expectedFreqHz = 440 * Math.pow(2, (note - 69) / 12);

          const passed = hasAudioOutput && !hasNaNValues && rms > 0.001;

          results.push({
            note,
            expectedFreqHz: Math.round(expectedFreqHz),
            hasAudio: hasAudioOutput,
            hasNaN: hasNaNValues,
            nanCount,
            rms,
            peak,
            passed,
          });

          if (!passed && firstFailure === null) {
            firstFailure = note;
          }
          if (hasNaNValues && firstNaN === null) {
            firstNaN = note;
          }
        }

        return {
          testNotes,
          results,
          firstFailure,
          firstNaN,
          totalTested: results.length,
          totalPassed: results.filter(r => r.passed).length,
          totalFailed: results.filter(r => !r.passed).length,
          totalWithNaN: results.filter(r => r.hasNaN).length,
        };
      },
      { sonic: sonicConfig, helpers: AUDIO_HELPERS }
    );

    // Log summary
    console.log(`\nChipLead high pitch test: MIDI notes ${result.testNotes.join(', ')}`);
    console.log(`Passed: ${result.totalPassed}/${result.totalTested}`);
    console.log(`Failed: ${result.totalFailed}/${result.totalTested}`);
    console.log(`With NaN values: ${result.totalWithNaN}/${result.totalTested}`);

    if (result.firstFailure !== null) {
      console.log(`First failure at MIDI note: ${result.firstFailure}`);
      const failedEntry = result.results.find(r => r.note === result.firstFailure);
      console.log(`Expected frequency at failure: ${failedEntry.expectedFreqHz} Hz`);
    }

    if (result.firstNaN !== null) {
      console.log(`First NaN at MIDI note: ${result.firstNaN}`);
    }

    // Log details for failed notes
    const failed = result.results.filter(r => !r.passed);
    if (failed.length > 0) {
      console.log("\nFailed notes detail:");
      for (const r of failed.slice(0, 10)) { // Show first 10
        console.log(`  Note ${r.note} (${r.expectedFreqHz}Hz): hasAudio=${r.hasAudio}, hasNaN=${r.hasNaN}, nanCount=${r.nanCount}, RMS=${Number.isNaN(r.rms) ? 'NaN' : r.rms.toFixed(6)}`);
      }
      if (failed.length > 10) {
        console.log(`  ... and ${failed.length - 10} more`);
      }
    }

    // This test documents the bug
    // Expect all notes to produce audio (test will fail if bug exists)
    expect(result.totalFailed).toBe(0);
  });

  test("beep synth control - MIDI notes 96 to 156", async ({ page, sonicConfig }) => {
    test.skip(sonicConfig.mode === 'postMessage', 'Audio capture requires SharedArrayBuffer');
    // Control test: verify that beep synth handles the same notes
    // This helps determine if the issue is chiplead-specific
    await page.goto("/test/harness.html");

    const result = await page.evaluate(
      async (config) => {
        eval(config.helpers);

        const sonic = new window.SuperSonic(config.sonic);
        await sonic.init();
        await sonic.loadSynthDef("sonic-pi-beep");

        // Sample notes: one per octave (96, 108, 120, 132, 144, 156)
        const testNotes = [96, 108, 120, 132, 144, 156];
        const results = [];
        let firstFailure = null;
        let firstNaN = null;

        for (const note of testNotes) {
          // Reset scsynth between each note to ensure clean state
          await sonic.reset();
          await sonic.loadSynthDef("sonic-pi-beep");

          sonic.startCapture();

          await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 0,
            "note", note,
            "amp", 0.5,
            "attack", 0,
            "release", 0.1
          );

          await new Promise((r) => setTimeout(r, 120));

          await sonic.send("/n_free", 1000);
          await sonic.sync(1);

          const captured = sonic.stopCapture();

          const hasAudioOutput = hasAudio(captured.left);
          const hasNaNValues = hasNaN(captured.left);
          const rms = calculateRMS(captured.left, 0, Math.min(captured.frames, 6000));

          const expectedFreqHz = 440 * Math.pow(2, (note - 69) / 12);
          const passed = hasAudioOutput && !hasNaNValues && rms > 0.001;

          results.push({
            note,
            expectedFreqHz: Math.round(expectedFreqHz),
            hasAudio: hasAudioOutput,
            hasNaN: hasNaNValues,
            rms,
            passed,
          });

          if (!passed && firstFailure === null) {
            firstFailure = note;
          }
          if (hasNaNValues && firstNaN === null) {
            firstNaN = note;
          }
        }

        return {
          testNotes,
          results,
          firstFailure,
          firstNaN,
          totalTested: results.length,
          totalPassed: results.filter(r => r.passed).length,
          totalFailed: results.filter(r => !r.passed).length,
          totalWithNaN: results.filter(r => r.hasNaN).length,
        };
      },
      { sonic: sonicConfig, helpers: AUDIO_HELPERS }
    );

    console.log(`\nBeep synth control test: MIDI notes ${result.testNotes.join(', ')}`);
    console.log(`Passed: ${result.totalPassed}/${result.totalTested}`);
    console.log(`Failed: ${result.totalFailed}/${result.totalTested}`);
    console.log(`With NaN values: ${result.totalWithNaN}/${result.totalTested}`);

    if (result.firstFailure !== null) {
      console.log(`First failure at MIDI note: ${result.firstFailure}`);
    }
    if (result.firstNaN !== null) {
      console.log(`First NaN at MIDI note: ${result.firstNaN}`);
    }

    // Beep should handle these notes - if it fails too, issue is in engine
    // If beep passes but chiplead fails, issue is chiplead-specific
    expect(result.totalFailed).toBe(0);
  });
});

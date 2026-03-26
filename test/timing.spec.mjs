import { test, expect, skipIfPostMessage } from './fixtures.mjs';

// These tests require waiting for drift timer (1s interval)
test.describe('Timing and Drift', () => {
  test.setTimeout(30000); // 30s for drift timer tests
  test.beforeEach(async ({ page, sonicConfig }) => {
    page.on('console', (msg) => {
      if (msg.type() === 'error') {
        console.error('Browser console error:', msg.text());
      }
    });

    await page.goto('/test/harness.html');
    await page.waitForFunction(() => window.supersonicReady === true, {
      timeout: 10000,
    });
  });

  test('drift stays within acceptable bounds after boot', async ({ page, sonicConfig }) => {
    // This test verifies that the NTP timing initialization doesn't have a race condition
    // that causes large drift values. The drift should stay close to 0 (within 500ms)
    // after the first drift timer fires (15 seconds).

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);

      await sonic.init();

      // Drift is calculated during init() so it may be non-zero immediately after boot.
      // It should be within acceptable bounds (< 500ms).
      const driftAfterInit = sonic.getMetrics().driftOffsetMs;

      // Wait for the drift timer to fire (it fires every 1 second)
      // Wait 2 seconds to ensure at least one timer fire
      await new Promise(r => setTimeout(r, 2000));

      // Get drift after timer has fired
      const driftAfterTimer = sonic.getMetrics().driftOffsetMs;

      // Wait a bit more to see if drift is growing
      await new Promise(r => setTimeout(r, 5000));
      const driftLater = sonic.getMetrics().driftOffsetMs;

      await sonic.destroy();

      return {
        driftAfterInit,
        driftAfterTimer,
        driftLater,
        // Drift growth rate: if drift is growing, this indicates a problem
        driftGrowth: Math.abs(driftLater - driftAfterTimer)
      };
    }, sonicConfig);

    // After init, drift should be within acceptable bounds (< 500ms)
    expect(Math.abs(result.driftAfterInit)).toBeLessThan(500);

    // After the timer fires, drift should be close to 0
    // Allow up to 500ms of drift due to normal clock variance
    expect(Math.abs(result.driftAfterTimer)).toBeLessThan(500);

    // Drift should not be growing significantly over 5 seconds
    // Allow 100ms of normal drift growth per 5 seconds
    expect(result.driftGrowth).toBeLessThan(100);
  });

  test('drift resets after resume from suspended state', async ({ page, sonicConfig }) => {
    // This test verifies that when AudioContext resumes from suspended state,
    // the timing is properly resynced and drift is reset to near 0

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);

      await sonic.init();

      // Wait for initial drift timer to fire
      await new Promise(r => setTimeout(r, 2000));
      const driftBeforeSuspend = sonic.getMetrics().driftOffsetMs;

      // Suspend the AudioContext
      await sonic.node.context.suspend();

      // Wait a bit in suspended state (clock won't advance)
      await new Promise(r => setTimeout(r, 2000));

      // Resume and recover
      await sonic.recover();

      // Wait for timing to resync
      await new Promise(r => setTimeout(r, 1000));

      const driftAfterResume = sonic.getMetrics().driftOffsetMs;

      await sonic.destroy();

      return {
        driftBeforeSuspend,
        driftAfterResume
      };
    }, sonicConfig);

    // After resume, drift should be reset to near 0
    expect(Math.abs(result.driftAfterResume)).toBeLessThan(500);
  });

  test('engine NTP matches wall clock after drift correction', async ({ page, sonicConfig, sonicMode }) => {
    // Verifies that the WASM engine's NTP calculation (contextTime + ntpStart + drift)
    // produces a value close to wall-clock NTP. If the drift field's unit (ms vs µs) is
    // mismatched between JS writer and WASM reader, this will be off by orders of magnitude.
    skipIfPostMessage(sonicMode, 'Requires SAB for direct drift field access');

    const result = await page.evaluate(async (config) => {
      const NTP_EPOCH_OFFSET = 2208988800;

      const sonic = new window.SuperSonic(config);
      await sonic.init();

      // Wait for drift to stabilize
      await new Promise(r => setTimeout(r, 2000));

      // Take a synchronized snapshot using getOutputTimestamp
      const timestamp = sonic.node.context.getOutputTimestamp();
      const wallClockNTP = (performance.timeOrigin + timestamp.performanceTime) / 1000 + NTP_EPOCH_OFFSET;
      const contextTime = timestamp.contextTime;

      // Read raw values from SAB — same fields WASM reads
      const sharedBuffer = sonic.sharedBuffer;
      const ringBufferBase = sonic.ringBufferBase;
      const bc = sonic.bufferConstants;

      const ntpStartView = new Float64Array(sharedBuffer, ringBufferBase + bc.NTP_START_TIME_START, 1);
      const driftView = new Int32Array(sharedBuffer, ringBufferBase + bc.DRIFT_OFFSET_START, 1);
      const globalView = new Int32Array(sharedBuffer, ringBufferBase + bc.GLOBAL_OFFSET_START, 1);

      const ntpStart = ntpStartView[0];
      const rawDrift = Atomics.load(driftView, 0);
      const rawGlobal = Atomics.load(globalView, 0);

      // Reconstruct what WASM computes: current_ntp = contextTime + ntpStart + drift + global
      // WASM divides drift by its divisor — we try BOTH to see which is correct
      const engineNTP_ms = contextTime + ntpStart + (rawDrift / 1000) + (rawGlobal / 1000);
      const engineNTP_us = contextTime + ntpStart + (rawDrift / 1000000) + (rawGlobal / 1000);

      const errorMs_msDivisor = Math.abs(engineNTP_ms - wallClockNTP) * 1000;
      const errorMs_usDivisor = Math.abs(engineNTP_us - wallClockNTP) * 1000;

      await sonic.destroy();

      return {
        wallClockNTP,
        contextTime,
        ntpStart,
        rawDrift,
        rawGlobal,
        engineNTP_ms,
        engineNTP_us,
        errorMs_msDivisor: Math.round(errorMs_msDivisor * 10) / 10,
        errorMs_usDivisor: Math.round(errorMs_usDivisor * 10) / 10,
      };
    }, sonicConfig);

    console.log('Drift diagnostic:', JSON.stringify(result, null, 2));

    // The engine's NTP (using whichever divisor the WASM actually uses) should be
    // within 10ms of wall clock. If the drift unit is wrong, one of these will be
    // off by orders of magnitude.
    const engineError = Math.min(result.errorMs_msDivisor, result.errorMs_usDivisor);
    expect(engineError).toBeLessThan(10);

    // Critically: the CORRECT divisor should give small error, the WRONG one should give large error.
    // If rawDrift is in ms (e.g. 3), ms divisor gives 0.003s, µs divisor gives 0.000003s — difference ~3ms
    // If rawDrift is in µs (e.g. 3000), ms divisor gives 3s, µs divisor gives 0.003s — difference ~3000s
    // So the wrong divisor should produce error > 100ms for any non-trivial drift.
    // Check that the two divisors don't BOTH give small errors (which would mean drift ≈ 0 and test is useless)
    if (Math.abs(result.rawDrift) > 10) {
      // Non-trivial drift: exactly one divisor should work
      const msDivisorOK = result.errorMs_msDivisor < 10;
      const usDivisorOK = result.errorMs_usDivisor < 10;
      expect(msDivisorOK !== usDivisorOK).toBe(true);
    }
  });
});

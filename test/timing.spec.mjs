import { test, expect } from '@playwright/test';

test.describe('Timing and Drift', () => {
  test.beforeEach(async ({ page }) => {
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

  test('drift stays within acceptable bounds after boot', async ({ page }) => {
    // This test verifies that the NTP timing initialization doesn't have a race condition
    // that causes large drift values. The drift should stay close to 0 (within 500ms)
    // after the first drift timer fires (15 seconds).

    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: '/dist/workers/',
        wasmBaseURL: '/dist/wasm/',
      });

      await sonic.init();

      // Drift is 0 immediately after boot (timer hasn't fired yet)
      const driftBeforeTimer = sonic.getMetrics().driftOffsetMs;

      // Wait for the drift timer to fire (it fires every 15 seconds)
      // Wait 16 seconds to ensure at least one timer fire
      await new Promise(r => setTimeout(r, 16000));

      // Get drift after timer has fired
      const driftAfterTimer = sonic.getMetrics().driftOffsetMs;

      // Wait a bit more to see if drift is growing
      await new Promise(r => setTimeout(r, 5000));
      const driftLater = sonic.getMetrics().driftOffsetMs;

      await sonic.destroy();

      return {
        driftBeforeTimer,
        driftAfterTimer,
        driftLater,
        // Drift growth rate: if drift is growing, this indicates a problem
        driftGrowth: Math.abs(driftLater - driftAfterTimer)
      };
    });

    // Before the timer fires, drift should be 0 (initial value)
    expect(result.driftBeforeTimer).toBe(0);

    // After the timer fires, drift should be close to 0
    // Allow up to 500ms of drift due to normal clock variance
    expect(Math.abs(result.driftAfterTimer)).toBeLessThan(500);

    // Drift should not be growing significantly over 5 seconds
    // Allow 100ms of normal drift growth per 5 seconds
    expect(result.driftGrowth).toBeLessThan(100);
  });

  test('drift resets after resume from suspended state', async ({ page }) => {
    // This test verifies that when AudioContext resumes from suspended state,
    // the timing is properly resynced and drift is reset to near 0

    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: '/dist/workers/',
        wasmBaseURL: '/dist/wasm/',
      });

      await sonic.init();

      // Wait for initial drift timer to fire
      await new Promise(r => setTimeout(r, 16000));
      const driftBeforeSuspend = sonic.getMetrics().driftOffsetMs;

      // Suspend the AudioContext
      await sonic.audioContext.suspend();

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
    });

    // After resume, drift should be reset to near 0
    expect(Math.abs(result.driftAfterResume)).toBeLessThan(500);
  });
});

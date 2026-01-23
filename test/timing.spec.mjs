import { test, expect } from './fixtures.mjs';

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
});

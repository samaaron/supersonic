import { test, expect } from './fixtures.mjs';

// The engine's clock (clockwork/src/clock/TimeSource.h): the audio thread keeps
// it from its render time, converging on the wall clock the worklet hands it
// with every block, and stepping on to it after a gap. The page reads it as
// sonic.clock.now(), and getMetrics() reports it against the wall clock as
// clockDriftUs, with clockAnchors counting the times it was anchored afresh.
test.describe('The engine\'s clock', () => {
  test.setTimeout(30000);
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

  test('is on the wall clock after boot, and stays there', async ({ page, sonicConfig }) => {
    const result = await page.evaluate(async (config) => {
      const NTP_EPOCH_OFFSET = 2208988800;
      const wall = () => (performance.timeOrigin + performance.now()) / 1000 + NTP_EPOCH_OFFSET;
      const sonic = new window.SuperSonic(config);
      await sonic.init();

      const read = () => ({ offWallMs: (sonic.clock.now() - wall()) * 1000, driftUs: sonic.getMetrics().clockDriftUs });
      await new Promise(r => setTimeout(r, 2000));
      const settled = read();
      await new Promise(r => setTimeout(r, 5000));
      const later = read();

      await sonic.destroy();
      return { settled, later };
    }, sonicConfig);

    // Within a few milliseconds of the wall clock, as the page reads it and as the engine measures it — in the same
    // units (a drift in µs reported as ms, or the reverse, is off by a thousand).
    for (const r of [result.settled, result.later]) {
      expect(Math.abs(r.offWallMs)).toBeLessThan(50);
      expect(Math.abs(r.driftUs / 1000)).toBeLessThan(50);
    }
    // And not drifting away: the wall is what it converges on.
    expect(Math.abs(result.later.offWallMs - result.settled.offWallMs)).toBeLessThan(50);
  });

  test('steps on to the wall clock after a suspend, and never goes back', async ({ page, sonicConfig }) => {
    const result = await page.evaluate(async (config) => {
      const NTP_EPOCH_OFFSET = 2208988800;
      const wall = () => (performance.timeOrigin + performance.now()) / 1000 + NTP_EPOCH_OFFSET;
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await new Promise(r => setTimeout(r, 1000));
      const anchorsBefore = sonic.getMetrics().clockAnchors;

      // Every few milliseconds, through the suspend and the resume: the most the clock went back from one reading
      // to the next.
      let last = 0, back = 0;
      const watch = setInterval(() => {
        const t = sonic.clock.now();
        if (last && t < last) back = Math.max(back, last - t);
        last = t;
      }, 2);

      await sonic.node.context.suspend();
      await new Promise(r => setTimeout(r, 2500));   // the clock stands still with the audio
      const stoodStillMs = (sonic.clock.now() - wall()) * 1000;
      await sonic.recover();
      await new Promise(r => setTimeout(r, 300));
      const offWallMs = (sonic.clock.now() - wall()) * 1000;
      clearInterval(watch);
      const anchorsAfter = sonic.getMetrics().clockAnchors;

      await sonic.destroy();
      return { stoodStillMs, offWallMs, back, anchorsBefore, anchorsAfter };
    }, sonicConfig);

    expect(result.stoodStillMs).toBeLessThan(-1500);           // behind the wall by most of the gap
    expect(Math.abs(result.offWallMs)).toBeLessThan(100);      // on the wall again at once, not converging for seconds
    expect(result.anchorsAfter).toBeGreaterThan(result.anchorsBefore);
    expect(result.back).toBe(0);
  });
});

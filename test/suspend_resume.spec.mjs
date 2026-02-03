import { test, expect } from "./fixtures.mjs";

/**
 * Suspend / Resume Tests
 *
 * Tests the suspend() and resume() lifecycle independent of flushAll.
 *
 * 1. suspend() transitions AudioContext to 'suspended'
 * 2. resume() transitions AudioContext back to 'running'
 * 3. resume() returns true and emits 'resumed' event
 * 4. suspend/resume cycle preserves loaded synthdefs
 * 5. OSC can be sent and processed after resume
 * 6. suspend() before init is a no-op (no throw)
 * 7. resume() before init returns false (no throw)
 * 8. Multiple suspend/resume cycles work cleanly
 *
 * Both SAB and postMessage modes are tested via the dual-project config.
 */

test.describe("Suspend / Resume", () => {
  test.beforeEach(async ({ page }) => {
    page.on("console", (msg) => {
      if (msg.type() === "error") {
        console.error("Browser console error:", msg.text());
      }
    });
    page.on("pageerror", (err) => {
      console.error("Page error:", err.message);
    });
    await page.goto("/test/harness.html");
    await page.waitForFunction(() => window.supersonicReady === true, {
      timeout: 10000,
    });
  });

  test("suspend() transitions AudioContext to suspended", async ({ page, sonicConfig }) => {
    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();

      const stateBefore = sonic.getMetrics().audioContextState;

      await sonic.suspend();

      const stateAfter = sonic.getMetrics().audioContextState;

      await sonic.shutdown();
      return { stateBefore, stateAfter };
    }, sonicConfig);

    console.log(`\nsuspend test (${sonicConfig.mode}):`);
    console.log(`  State before: ${result.stateBefore}`);
    console.log(`  State after: ${result.stateAfter}`);

    expect(result.stateBefore).toBe("running");
    expect(result.stateAfter).toBe("suspended");
  });

  test("resume() transitions AudioContext back to running", async ({ page, sonicConfig }) => {
    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();

      await sonic.suspend();
      const stateSuspended = sonic.getMetrics().audioContextState;

      const resumeResult = await sonic.resume();

      const stateResumed = sonic.getMetrics().audioContextState;

      await sonic.shutdown();
      return { stateSuspended, stateResumed, resumeResult };
    }, sonicConfig);

    console.log(`\nresume test (${sonicConfig.mode}):`);
    console.log(`  State after suspend: ${result.stateSuspended}`);
    console.log(`  State after resume: ${result.stateResumed}`);
    console.log(`  resume() returned: ${result.resumeResult}`);

    expect(result.stateSuspended).toBe("suspended");
    expect(result.stateResumed).toBe("running");
    expect(result.resumeResult).toBe(true);
  });

  test("resume() emits resumed event", async ({ page, sonicConfig }) => {
    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();

      let resumedEventFired = false;
      sonic.on('resumed', () => { resumedEventFired = true; });

      await sonic.suspend();
      const resumeResult = await sonic.resume();

      await sonic.shutdown();
      return { resumedEventFired, resumeResult };
    }, sonicConfig);

    console.log(`\nresumed event test (${sonicConfig.mode}):`);
    console.log(`  Event fired: ${result.resumedEventFired}`);

    expect(result.resumedEventFired).toBe(true);
    expect(result.resumeResult).toBe(true);
  });

  test("process count advances after resume", async ({ page, sonicConfig }) => {
    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();

      // Wait for some process() calls to accumulate
      await new Promise(r => setTimeout(r, 300));
      const metricsBefore = sonic.getMetrics();
      const countRunning = metricsBefore.scsynthProcessCount || 0;

      await sonic.suspend();
      // Brief wait to confirm process stops
      await new Promise(r => setTimeout(r, 200));
      const metricsSuspended = sonic.getMetrics();
      const countSuspended = metricsSuspended.scsynthProcessCount || 0;

      // Another wait — count should NOT advance while suspended
      await new Promise(r => setTimeout(r, 200));
      const metricsStillSuspended = sonic.getMetrics();
      const countStillSuspended = metricsStillSuspended.scsynthProcessCount || 0;

      await sonic.resume();
      // Wait for process to run a few times
      await new Promise(r => setTimeout(r, 300));
      const metricsResumed = sonic.getMetrics();
      const countResumed = metricsResumed.scsynthProcessCount || 0;

      await sonic.shutdown();
      return { countRunning, countSuspended, countStillSuspended, countResumed };
    }, sonicConfig);

    console.log(`\nprocess count test (${sonicConfig.mode}):`);
    console.log(`  Running: ${result.countRunning}`);
    console.log(`  After suspend: ${result.countSuspended}`);
    console.log(`  Still suspended: ${result.countStillSuspended}`);
    console.log(`  After resume: ${result.countResumed}`);

    // Process count should not advance while suspended
    expect(result.countStillSuspended).toBe(result.countSuspended);
    // Process count should advance after resume
    expect(result.countResumed).toBeGreaterThan(result.countStillSuspended);
  });

  test("suspend/resume preserves loaded synthdefs", async ({ page, sonicConfig }) => {
    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef('sonic-pi-beep');

      const cachedBefore = sonic.loadedSynthDefs.has('sonic-pi-beep');

      await sonic.suspend();
      await sonic.resume();

      const cachedAfter = sonic.loadedSynthDefs.has('sonic-pi-beep');

      // Verify synthdef actually works — create and free a synth
      let synthWorks = false;
      try {
        await sonic.send('/s_new', 'sonic-pi-beep', 90000, 0, 0, 'amp', 0.0, 'release', 0.01);
        await sonic.send('/n_free', 90000);
        synthWorks = true;
      } catch (e) {
        synthWorks = false;
      }

      await sonic.shutdown();
      return { cachedBefore, cachedAfter, synthWorks };
    }, sonicConfig);

    console.log(`\nsynthdef preservation test (${sonicConfig.mode}):`);
    console.log(`  Cached before: ${result.cachedBefore}`);
    console.log(`  Cached after: ${result.cachedAfter}`);
    console.log(`  Synth works: ${result.synthWorks}`);

    expect(result.cachedBefore).toBe(true);
    expect(result.cachedAfter).toBe(true);
    expect(result.synthWorks).toBe(true);
  });

  test("multiple suspend/resume cycles work cleanly", async ({ page, sonicConfig }) => {
    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef('sonic-pi-beep');

      const results = [];
      for (let i = 0; i < 3; i++) {
        await sonic.suspend();
        const suspended = sonic.getMetrics().audioContextState;

        const resumed = await sonic.resume();
        const running = sonic.getMetrics().audioContextState;

        results.push({ cycle: i, suspended, running, resumed });
      }

      // Final check — engine still functional
      let synthWorks = false;
      try {
        await sonic.send('/s_new', 'sonic-pi-beep', 91000, 0, 0, 'amp', 0.0, 'release', 0.01);
        await sonic.send('/n_free', 91000);
        synthWorks = true;
      } catch (e) {
        synthWorks = false;
      }

      await sonic.shutdown();
      return { results, synthWorks };
    }, sonicConfig);

    console.log(`\nmultiple cycles test (${sonicConfig.mode}):`);
    for (const r of result.results) {
      console.log(`  Cycle ${r.cycle}: suspended=${r.suspended}, running=${r.running}, resumed=${r.resumed}`);
    }
    console.log(`  Synth works after 3 cycles: ${result.synthWorks}`);

    for (const r of result.results) {
      expect(r.suspended).toBe("suspended");
      expect(r.running).toBe("running");
      expect(r.resumed).toBe(true);
    }
    expect(result.synthWorks).toBe(true);
  });

  test("suspend() before init is a no-op", async ({ page, sonicConfig }) => {
    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);

      // Should not throw
      let threw = false;
      try {
        await sonic.suspend();
      } catch (e) {
        threw = true;
      }

      return { threw };
    }, sonicConfig);

    expect(result.threw).toBe(false);
  });

  test("resume() before init returns false", async ({ page, sonicConfig }) => {
    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);

      // Should not throw, should return false
      let threw = false;
      let returnValue;
      try {
        returnValue = await sonic.resume();
      } catch (e) {
        threw = true;
      }

      return { threw, returnValue };
    }, sonicConfig);

    expect(result.threw).toBe(false);
    expect(result.returnValue).toBe(false);
  });
});

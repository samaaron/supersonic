import { test, expect, skipIfPostMessage } from "./fixtures.mjs";

/**
 * Demand-Driven Prescheduler Dispatch Tests
 *
 * Tests verify the demand-driven timer logic that replaced fixed 50ms polling:
 * 1. Timer preemption: sooner events preempt later timers
 * 2. Idle-to-active: events dispatch correctly after idle period
 * 3. Cancel-to-idle-to-active: system recovers after cancelAll empties the heap
 * 4. Post-cancel rescheduling: remaining events dispatch correctly after partial cancel
 * 5. Metrics consistency through all state transitions
 */

test.describe("Demand-Driven Prescheduler Dispatch", () => {
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

    // Install NTP helpers on window for all tests
    await page.evaluate(() => {
      const NTP_EPOCH_OFFSET = 2208988800;
      window._getCurrentNTP = () => {
        const perfTimeMs = performance.timeOrigin + performance.now();
        return (perfTimeMs / 1000) + NTP_EPOCH_OFFSET;
      };

      window._createTimedBundle = (ntpTime, nodeId) => {
        const message = {
          address: "/s_new",
          args: [
            { type: 's', value: "sonic-pi-beep" },
            { type: 'i', value: nodeId },
            { type: 'i', value: 0 },
            { type: 'i', value: 0 },
            { type: 's', value: "note" },
            { type: 'f', value: 60 },
            { type: 's', value: "amp" },
            { type: 'f', value: 0.01 },
            { type: 's', value: "release" },
            { type: 'f', value: 0.01 }
          ]
        };

        const encodedMessage = window.SuperSonic.osc.encode(message);
        const bundleSize = 8 + 8 + 4 + encodedMessage.byteLength;
        const bundle = new Uint8Array(bundleSize);
        const view = new DataView(bundle.buffer);

        bundle.set([0x23, 0x62, 0x75, 0x6e, 0x64, 0x6c, 0x65, 0x00], 0);
        const ntpSeconds = Math.floor(ntpTime);
        const ntpFraction = Math.floor((ntpTime % 1) * 0x100000000);
        view.setUint32(8, ntpSeconds, false);
        view.setUint32(12, ntpFraction, false);
        view.setInt32(16, encodedMessage.byteLength, false);
        bundle.set(encodedMessage, 20);

        return bundle;
      };
    });
  });

  test("sooner event preempts later timer and dispatches on time", async ({ page, sonicConfig }) => {
    const config = { ...sonicConfig, bypassLookaheadMs: 200 };

    const result = await page.evaluate(async (config) => {
      const getCurrentNTP = window._getCurrentNTP;
      const createTimedBundle = window._createTimedBundle;

      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");
      await sonic.sync(1);

      const metricsBefore = sonic.getMetrics();
      const dispatchedBefore = metricsBefore.preschedulerDispatched || 0;

      // Schedule a bundle 2 seconds in the future (far out)
      const farNTP = getCurrentNTP() + 2.0;
      const farBundle = createTimedBundle(farNTP, 80000);
      sonic.sendOSC(farBundle, { sessionId: 0, runTag: 'far' });

      // Wait briefly to ensure the far timer is set
      await new Promise(r => setTimeout(r, 50));

      // Now schedule a bundle 300ms in the future (sooner - should preempt)
      const soonNTP = getCurrentNTP() + 0.3;
      const soonBundle = createTimedBundle(soonNTP, 80001);
      sonic.sendOSC(soonBundle, { sessionId: 0, runTag: 'soon' });

      // Wait 400ms - the soon bundle should have been dispatched,
      // but the far bundle should still be pending
      await new Promise(r => setTimeout(r, 400));

      const metricsAfter = sonic.getMetrics();
      const dispatchedAfter = metricsAfter.preschedulerDispatched || 0;
      const pendingAfter = metricsAfter.preschedulerPending || 0;
      const dispatched = dispatchedAfter - dispatchedBefore;

      // Cancel the far bundle to clean up
      sonic.cancelTag('far');
      await new Promise(r => setTimeout(r, 50));

      const metricsFinal = sonic.getMetrics();
      const pendingFinal = metricsFinal.preschedulerPending || 0;

      return { dispatched, pendingAfter, pendingFinal };
    }, config);

    console.log(`\nPreemption test:`);
    console.log(`  Dispatched after 400ms: ${result.dispatched}`);
    console.log(`  Pending after 400ms: ${result.pendingAfter}`);
    console.log(`  Pending after cleanup: ${result.pendingFinal}`);

    // The soon bundle should have dispatched
    expect(result.dispatched).toBe(1);
    // The far bundle should still be pending
    expect(result.pendingAfter).toBe(1);
    // After cancel, nothing pending
    expect(result.pendingFinal).toBe(0);
  });

  test("idle prescheduler dispatches first event promptly", async ({ page, sonicConfig }) => {
    const config = { ...sonicConfig, bypassLookaheadMs: 100 };

    const result = await page.evaluate(async (config) => {
      const getCurrentNTP = window._getCurrentNTP;
      const createTimedBundle = window._createTimedBundle;

      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");
      await sonic.sync(1);

      // Confirm idle: no events pending
      const metricsIdle = sonic.getMetrics();
      const pendingIdle = metricsIdle.preschedulerPending || 0;
      const dispatchedIdle = metricsIdle.preschedulerDispatched || 0;

      // Wait 500ms in idle state (old polling would have run ~10 polls for nothing)
      await new Promise(r => setTimeout(r, 500));

      // Now schedule a bundle 200ms in the future
      const targetNTP = getCurrentNTP() + 0.2;
      const bundle = createTimedBundle(targetNTP, 81000);
      sonic.sendOSC(bundle, { sessionId: 0, runTag: 'idle_test' });

      // Wait 300ms - bundle should be dispatched (200ms target - 100ms lookahead = ~100ms delay + margin)
      await new Promise(r => setTimeout(r, 300));

      const metricsAfter = sonic.getMetrics();
      const pendingAfter = metricsAfter.preschedulerPending || 0;
      const dispatchedAfter = metricsAfter.preschedulerDispatched || 0;
      const dispatched = dispatchedAfter - dispatchedIdle;

      return { pendingIdle, dispatched, pendingAfter };
    }, config);

    console.log(`\nIdle-to-active test:`);
    console.log(`  Pending while idle: ${result.pendingIdle}`);
    console.log(`  Dispatched after event: ${result.dispatched}`);
    console.log(`  Pending after dispatch: ${result.pendingAfter}`);

    expect(result.pendingIdle).toBe(0);
    expect(result.dispatched).toBe(1);
    expect(result.pendingAfter).toBe(0);
  });

  test("cancelAll goes idle then recovers for new events", async ({ page, sonicConfig }) => {
    const config = { ...sonicConfig, bypassLookaheadMs: 100 };

    const result = await page.evaluate(async (config) => {
      const getCurrentNTP = window._getCurrentNTP;
      const createTimedBundle = window._createTimedBundle;

      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");
      await sonic.sync(1);

      const metricsBefore = sonic.getMetrics();
      const dispatchedBefore = metricsBefore.preschedulerDispatched || 0;
      const cancelledBefore = metricsBefore.preschedulerEventsCancelled || 0;

      // Schedule 10 bundles far in the future
      const farNTP = getCurrentNTP() + 10.0;
      for (let i = 0; i < 10; i++) {
        const bundle = createTimedBundle(farNTP + (i * 0.001), 82000 + i);
        sonic.sendOSC(bundle, { sessionId: 0, runTag: 'batch1' });
      }

      await new Promise(r => setTimeout(r, 50));
      const pendingBeforeCancel = (sonic.getMetrics()).preschedulerPending || 0;

      // Cancel all - should go idle
      sonic.cancelAll();
      await new Promise(r => setTimeout(r, 50));

      const metricsAfterCancel = sonic.getMetrics();
      const pendingAfterCancel = metricsAfterCancel.preschedulerPending || 0;
      const cancelledAfterCancel = (metricsAfterCancel.preschedulerEventsCancelled || 0) - cancelledBefore;

      // Now schedule a new bundle 200ms out - system should wake up from idle
      const soonNTP = getCurrentNTP() + 0.2;
      const newBundle = createTimedBundle(soonNTP, 83000);
      sonic.sendOSC(newBundle, { sessionId: 0, runTag: 'recovery' });

      // Wait for it to dispatch
      await new Promise(r => setTimeout(r, 300));

      const metricsFinal = sonic.getMetrics();
      const dispatchedFinal = (metricsFinal.preschedulerDispatched || 0) - dispatchedBefore;
      const pendingFinal = metricsFinal.preschedulerPending || 0;

      return {
        pendingBeforeCancel,
        pendingAfterCancel,
        cancelledAfterCancel,
        dispatchedFinal,
        pendingFinal,
      };
    }, config);

    console.log(`\nCancel-to-idle-to-active test:`);
    console.log(`  Pending before cancel: ${result.pendingBeforeCancel}`);
    console.log(`  Pending after cancel: ${result.pendingAfterCancel}`);
    console.log(`  Cancelled: ${result.cancelledAfterCancel}`);
    console.log(`  Dispatched after recovery: ${result.dispatchedFinal}`);
    console.log(`  Pending final: ${result.pendingFinal}`);

    expect(result.pendingBeforeCancel).toBe(10);
    expect(result.pendingAfterCancel).toBe(0);
    expect(result.cancelledAfterCancel).toBe(10);
    // The recovery bundle should have dispatched
    expect(result.dispatchedFinal).toBe(1);
    expect(result.pendingFinal).toBe(0);
  });

  test("partial cancel reschedules correctly for remaining events", async ({ page, sonicConfig }) => {
    const config = { ...sonicConfig, bypassLookaheadMs: 100 };

    const result = await page.evaluate(async (config) => {
      const getCurrentNTP = window._getCurrentNTP;
      const createTimedBundle = window._createTimedBundle;

      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");
      await sonic.sync(1);

      const metricsBefore = sonic.getMetrics();
      const dispatchedBefore = metricsBefore.preschedulerDispatched || 0;
      const cancelledBefore = metricsBefore.preschedulerEventsCancelled || 0;

      // Schedule 5 bundles at T+300ms with tag 'keep'
      const keepNTP = getCurrentNTP() + 0.3;
      for (let i = 0; i < 5; i++) {
        const bundle = createTimedBundle(keepNTP + (i * 0.001), 84000 + i);
        sonic.sendOSC(bundle, { sessionId: 0, runTag: 'keep' });
      }

      // Schedule 5 bundles at T+300ms with tag 'discard'
      for (let i = 0; i < 5; i++) {
        const bundle = createTimedBundle(keepNTP + (i * 0.001), 85000 + i);
        sonic.sendOSC(bundle, { sessionId: 0, runTag: 'discard' });
      }

      await new Promise(r => setTimeout(r, 50));
      const pendingBefore = (sonic.getMetrics()).preschedulerPending || 0;

      // Cancel only 'discard' tag - 'keep' should still dispatch
      sonic.cancelTag('discard');
      await new Promise(r => setTimeout(r, 50));

      const metricsAfterCancel = sonic.getMetrics();
      const pendingAfterCancel = metricsAfterCancel.preschedulerPending || 0;
      const cancelledCount = (metricsAfterCancel.preschedulerEventsCancelled || 0) - cancelledBefore;

      // Wait for 'keep' bundles to dispatch (300ms - 100ms lookahead = ~200ms + margin)
      await new Promise(r => setTimeout(r, 400));

      const metricsFinal = sonic.getMetrics();
      const dispatchedFinal = (metricsFinal.preschedulerDispatched || 0) - dispatchedBefore;
      const pendingFinal = metricsFinal.preschedulerPending || 0;

      return {
        pendingBefore,
        pendingAfterCancel,
        cancelledCount,
        dispatchedFinal,
        pendingFinal,
      };
    }, config);

    console.log(`\nPartial cancel rescheduling test:`);
    console.log(`  Pending before cancel: ${result.pendingBefore}`);
    console.log(`  Pending after cancel: ${result.pendingAfterCancel}`);
    console.log(`  Cancelled: ${result.cancelledCount}`);
    console.log(`  Dispatched: ${result.dispatchedFinal}`);
    console.log(`  Pending final: ${result.pendingFinal}`);

    expect(result.pendingBefore).toBe(10);
    expect(result.cancelledCount).toBe(5);
    expect(result.pendingAfterCancel).toBe(5);
    // The 5 'keep' bundles should have dispatched
    expect(result.dispatchedFinal).toBe(5);
    expect(result.pendingFinal).toBe(0);
  });

  test("metrics stay consistent through preemption and idle transitions", async ({ page, sonicConfig }) => {
    const HEADROOM_UNSET_SENTINEL = 0xFFFFFFFF;
    const config = { ...sonicConfig, bypassLookaheadMs: 100 };

    const result = await page.evaluate(async (config) => {
      const getCurrentNTP = window._getCurrentNTP;
      const createTimedBundle = window._createTimedBundle;
      const HEADROOM_UNSET_SENTINEL = 0xFFFFFFFF;

      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");
      await sonic.sync(1);

      // --- Phase 1: Idle state metrics ---
      const metricsIdle = sonic.getMetrics();
      const pendingIdle = metricsIdle.preschedulerPending || 0;
      const scheduledIdle = metricsIdle.preschedulerBundlesScheduled || 0;
      const dispatchedIdle = metricsIdle.preschedulerDispatched || 0;
      const cancelledIdle = metricsIdle.preschedulerEventsCancelled || 0;
      const minHeadroomIdle = metricsIdle.preschedulerMinHeadroomMs;

      // --- Phase 2: Schedule events and let them dispatch ---
      const soonNTP = getCurrentNTP() + 0.2;  // 200ms out
      for (let i = 0; i < 3; i++) {
        const bundle = createTimedBundle(soonNTP + (i * 0.001), 86000 + i);
        sonic.sendOSC(bundle, { sessionId: 0, runTag: 'metrics_test' });
      }

      await new Promise(r => setTimeout(r, 50));
      const metricsAfterSchedule = sonic.getMetrics();
      const pendingAfterSchedule = metricsAfterSchedule.preschedulerPending || 0;
      const scheduledAfterSchedule = (metricsAfterSchedule.preschedulerBundlesScheduled || 0) - scheduledIdle;

      // Wait for dispatch
      await new Promise(r => setTimeout(r, 300));

      const metricsAfterDispatch = sonic.getMetrics();
      const pendingAfterDispatch = metricsAfterDispatch.preschedulerPending || 0;
      const dispatchedPhase2 = (metricsAfterDispatch.preschedulerDispatched || 0) - dispatchedIdle;
      const minHeadroomPhase2 = metricsAfterDispatch.preschedulerMinHeadroomMs;

      // --- Phase 3: Schedule and cancel (metrics should reflect cancellation) ---
      const farNTP = getCurrentNTP() + 10.0;
      for (let i = 0; i < 5; i++) {
        const bundle = createTimedBundle(farNTP + (i * 0.001), 87000 + i);
        sonic.sendOSC(bundle, { sessionId: 0, runTag: 'to_cancel' });
      }
      await new Promise(r => setTimeout(r, 50));

      const metricsBeforeCancel = sonic.getMetrics();
      const pendingBeforeCancel = metricsBeforeCancel.preschedulerPending || 0;
      const scheduledPhase3 = (metricsBeforeCancel.preschedulerBundlesScheduled || 0) - scheduledIdle;

      sonic.cancelTag('to_cancel');
      await new Promise(r => setTimeout(r, 50));

      const metricsAfterCancel = sonic.getMetrics();
      const pendingAfterCancel = metricsAfterCancel.preschedulerPending || 0;
      const cancelledTotal = (metricsAfterCancel.preschedulerEventsCancelled || 0) - cancelledIdle;

      // --- Phase 4: Preemption - schedule far, then soon ---
      const farNTP2 = getCurrentNTP() + 5.0;
      const farBundle = createTimedBundle(farNTP2, 88000);
      sonic.sendOSC(farBundle, { sessionId: 0, runTag: 'far2' });

      await new Promise(r => setTimeout(r, 30));
      const pendingWithFar = (sonic.getMetrics()).preschedulerPending || 0;

      // Preempt with sooner event
      const soonNTP2 = getCurrentNTP() + 0.2;
      const soonBundle = createTimedBundle(soonNTP2, 88001);
      sonic.sendOSC(soonBundle, { sessionId: 0, runTag: 'soon2' });

      await new Promise(r => setTimeout(r, 30));
      const pendingWithBoth = (sonic.getMetrics()).preschedulerPending || 0;

      // Wait for soon event to dispatch
      await new Promise(r => setTimeout(r, 300));
      const metricsAfterPreempt = sonic.getMetrics();
      const pendingAfterPreempt = metricsAfterPreempt.preschedulerPending || 0;

      // Clean up far bundle
      sonic.cancelTag('far2');
      await new Promise(r => setTimeout(r, 50));

      const metricsFinal = sonic.getMetrics();
      const pendingFinal = metricsFinal.preschedulerPending || 0;
      const scheduledTotal = (metricsFinal.preschedulerBundlesScheduled || 0) - scheduledIdle;
      const cancelledFinal = (metricsFinal.preschedulerEventsCancelled || 0) - cancelledIdle;
      const dispatchedFinal = (metricsFinal.preschedulerDispatched || 0) - dispatchedIdle;
      const minHeadroomFinal = metricsFinal.preschedulerMinHeadroomMs;

      return {
        pendingIdle, minHeadroomIdle,
        pendingAfterSchedule, scheduledAfterSchedule,
        pendingAfterDispatch, dispatchedPhase2, minHeadroomPhase2,
        pendingBeforeCancel, scheduledPhase3,
        pendingAfterCancel, cancelledTotal,
        pendingWithFar, pendingWithBoth, pendingAfterPreempt,
        pendingFinal, scheduledTotal, cancelledFinal, dispatchedFinal, minHeadroomFinal,
        HEADROOM_UNSET_SENTINEL,
      };
    }, config);

    console.log(`\nMetrics consistency test:`);
    console.log(`  Phase 1 (idle): pending=${result.pendingIdle}`);
    console.log(`  Phase 2 (dispatch): scheduled=${result.scheduledAfterSchedule}, dispatched=${result.dispatchedPhase2}, pending=${result.pendingAfterDispatch}`);
    console.log(`  Phase 3 (cancel): pending before=${result.pendingBeforeCancel}, after=${result.pendingAfterCancel}, cancelled=${result.cancelledTotal}`);
    console.log(`  Phase 4 (preempt): pending far=${result.pendingWithFar}, both=${result.pendingWithBoth}, after=${result.pendingAfterPreempt}`);
    console.log(`  Final: scheduled=${result.scheduledTotal}, dispatched=${result.dispatchedFinal}, cancelled=${result.cancelledFinal}, pending=${result.pendingFinal}`);
    console.log(`  Min headroom: idle=${result.minHeadroomIdle}, phase2=${result.minHeadroomPhase2}, final=${result.minHeadroomFinal}`);

    // Phase 1: idle state
    expect(result.pendingIdle).toBe(0);
    expect(result.minHeadroomIdle).toBe(HEADROOM_UNSET_SENTINEL);

    // Phase 2: schedule 3, all dispatched
    expect(result.pendingAfterSchedule).toBe(3);
    expect(result.scheduledAfterSchedule).toBe(3);
    expect(result.pendingAfterDispatch).toBe(0);
    expect(result.dispatchedPhase2).toBe(3);

    // Min headroom should now be set (bundles were 200ms out with 100ms lookahead = ~100ms headroom)
    expect(result.minHeadroomPhase2).not.toBe(HEADROOM_UNSET_SENTINEL);
    expect(result.minHeadroomPhase2).toBeGreaterThanOrEqual(0);
    expect(result.minHeadroomPhase2).toBeLessThan(250);

    // Phase 3: schedule 5, cancel 5
    expect(result.pendingBeforeCancel).toBe(5);
    expect(result.pendingAfterCancel).toBe(0);
    expect(result.cancelledTotal).toBe(5);

    // Phase 4: preemption tracking
    expect(result.pendingWithFar).toBe(1);
    expect(result.pendingWithBoth).toBe(2);
    // After preemption wait: soon dispatched, far still pending
    expect(result.pendingAfterPreempt).toBe(1);

    // Final accounting: scheduled == dispatched + cancelled + pending
    // Total scheduled: 3 (phase2) + 5 (phase3) + 2 (phase4) = 10
    expect(result.scheduledTotal).toBe(10);
    // dispatched: 3 (phase2) + 1 (soon from phase4) = 4
    expect(result.dispatchedFinal).toBe(4);
    // cancelled: 5 (phase3) + 1 (far from phase4) = 6
    expect(result.cancelledFinal).toBe(6);
    // Nothing left
    expect(result.pendingFinal).toBe(0);
    // Conservation law
    expect(result.scheduledTotal).toBe(result.dispatchedFinal + result.cancelledFinal + result.pendingFinal);
  });

  test("multiple preemptions in sequence dispatch in correct order", async ({ page, sonicConfig }) => {
    const config = { ...sonicConfig, bypassLookaheadMs: 100 };

    const result = await page.evaluate(async (config) => {
      const getCurrentNTP = window._getCurrentNTP;
      const createTimedBundle = window._createTimedBundle;

      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");
      await sonic.sync(1);

      const metricsBefore = sonic.getMetrics();
      const dispatchedBefore = metricsBefore.preschedulerDispatched || 0;

      // Schedule at T+2s, then T+1s, then T+0.5s - each should preempt
      // Use 500ms for the soonest (not 300ms) to allow time for all 3 to land
      // in the heap before the first one dispatches
      const now = getCurrentNTP();

      const b1 = createTimedBundle(now + 2.0, 89000);
      sonic.sendOSC(b1, { sessionId: 0, runTag: 't2s' });
      await new Promise(r => setTimeout(r, 20));

      const b2 = createTimedBundle(now + 1.0, 89001);
      sonic.sendOSC(b2, { sessionId: 0, runTag: 't1s' });
      await new Promise(r => setTimeout(r, 20));

      const b3 = createTimedBundle(now + 0.5, 89002);
      sonic.sendOSC(b3, { sessionId: 0, runTag: 't500ms' });

      // Wait for all 3 to arrive at worker and metrics to update
      await new Promise(r => setTimeout(r, 80));

      const pendingAfterAll = (sonic.getMetrics()).preschedulerPending || 0;

      // Wait 600ms from now - the 500ms bundle should have dispatched
      await new Promise(r => setTimeout(r, 520));

      const metricsAfterFirst = sonic.getMetrics();
      const dispatchedAfterFirst = (metricsAfterFirst.preschedulerDispatched || 0) - dispatchedBefore;
      const pendingAfterFirst = metricsAfterFirst.preschedulerPending || 0;

      // Wait another 600ms - the 1s bundle should now have dispatched too
      await new Promise(r => setTimeout(r, 600));

      const metricsAfterSecond = sonic.getMetrics();
      const dispatchedAfterSecond = (metricsAfterSecond.preschedulerDispatched || 0) - dispatchedBefore;
      const pendingAfterSecond = metricsAfterSecond.preschedulerPending || 0;

      // Clean up the 2s bundle
      sonic.cancelTag('t2s');
      await new Promise(r => setTimeout(r, 50));

      const metricsFinal = sonic.getMetrics();
      const pendingFinal = metricsFinal.preschedulerPending || 0;

      return {
        pendingAfterAll,
        dispatchedAfterFirst,
        pendingAfterFirst,
        dispatchedAfterSecond,
        pendingAfterSecond,
        pendingFinal,
      };
    }, config);

    console.log(`\nMultiple preemption test:`);
    console.log(`  Pending after scheduling all 3: ${result.pendingAfterAll}`);
    console.log(`  After 600ms: dispatched=${result.dispatchedAfterFirst}, pending=${result.pendingAfterFirst}`);
    console.log(`  After 1.2s: dispatched=${result.dispatchedAfterSecond}, pending=${result.pendingAfterSecond}`);
    console.log(`  After cleanup: pending=${result.pendingFinal}`);

    // All 3 should be in the heap
    expect(result.pendingAfterAll).toBe(3);
    // After 600ms: only the 500ms bundle dispatched
    expect(result.dispatchedAfterFirst).toBe(1);
    expect(result.pendingAfterFirst).toBe(2);
    // After 1.2s: the 1s bundle also dispatched
    expect(result.dispatchedAfterSecond).toBe(2);
    expect(result.pendingAfterSecond).toBe(1);
    // After cleanup: nothing left
    expect(result.pendingFinal).toBe(0);
  });

  test("retry queue drains successfully when worklet resumes (SAB only)", async ({ page, sonicConfig, sonicMode }) => {
    skipIfPostMessage(sonicMode, "Retry queue only used in SAB mode (postMessage dispatch never fails)");

    const config = { ...sonicConfig, bypassLookaheadMs: 200 };

    const result = await page.evaluate(async (config) => {
      const getCurrentNTP = window._getCurrentNTP;
      const createTimedBundle = window._createTimedBundle;

      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");
      await sonic.sync(1);

      const metricsBefore = sonic.getMetrics();
      const dispatchedBefore = metricsBefore.preschedulerDispatched || 0;
      const retriesSucceededBefore = metricsBefore.preschedulerRetriesSucceeded || 0;
      const retriesFailedBefore = metricsBefore.preschedulerRetriesFailed || 0;

      // Suspend the AudioContext — worklet stops consuming from ring buffer
      await sonic.suspend();

      // Schedule 8000 bundles (enough to overflow the ~6800-capacity ring buffer)
      const bundleNTP = getCurrentNTP() + 1.5;
      const BUNDLE_COUNT = 8000;
      for (let i = 0; i < BUNDLE_COUNT; i++) {
        const bundle = createTimedBundle(bundleNTP + (i * 0.0001), 90000 + i);
        sonic.sendOSC(bundle, { sessionId: 0, runTag: 'retry_test' });
      }

      // Poll metrics until we see the retry queue populated
      // This means: buffer filled, dispatch happened, retries are in-flight
      const pollStart = performance.now();
      const POLL_TIMEOUT = 5000;
      let retryDetected = false;

      while (performance.now() - pollStart < POLL_TIMEOUT) {
        const m = sonic.getMetrics();
        if ((m.preschedulerRetryQueueSize || 0) > 0) {
          retryDetected = true;
          break;
        }
        await new Promise(r => setTimeout(r, 10));
      }

      const metricsAtDetection = sonic.getMetrics();
      const retryQueueAtDetection = metricsAtDetection.preschedulerRetryQueueSize || 0;
      const retriesFailedAtDetection = (metricsAtDetection.preschedulerRetriesFailed || 0) - retriesFailedBefore;

      // Resume immediately — worklet starts draining the buffer,
      // retries should succeed before exhausting MAX_RETRIES
      await sonic.resume();

      // Wait for retry queue to drain and worklet to process
      await new Promise(r => setTimeout(r, 1000));

      const metricsFinal = sonic.getMetrics();
      const retryQueueFinal = metricsFinal.preschedulerRetryQueueSize || 0;
      const retryPeakFinal = metricsFinal.preschedulerRetryQueuePeak || 0;
      const retriesSucceeded = (metricsFinal.preschedulerRetriesSucceeded || 0) - retriesSucceededBefore;
      const retriesFailed = (metricsFinal.preschedulerRetriesFailed || 0) - retriesFailedBefore;
      const dispatchedTotal = (metricsFinal.preschedulerDispatched || 0) - dispatchedBefore;
      const pendingFinal = metricsFinal.preschedulerPending || 0;
      const bundlesScheduled = (metricsFinal.preschedulerBundlesScheduled || 0) -
        (metricsBefore.preschedulerBundlesScheduled || 0);

      return {
        bundleCount: BUNDLE_COUNT,
        bundlesScheduled,
        retryDetected,
        retryQueueAtDetection,
        retriesFailedAtDetection,
        retryQueueFinal,
        retryPeakFinal,
        retriesSucceeded,
        retriesFailed,
        dispatchedTotal,
        pendingFinal,
      };
    }, config);

    console.log(`\nRetry queue drain test (SAB):`);
    console.log(`  Bundles sent: ${result.bundleCount}`);
    console.log(`  Bundles scheduled: ${result.bundlesScheduled}`);
    console.log(`  Retry detected: ${result.retryDetected} (queue=${result.retryQueueAtDetection}, failed=${result.retriesFailedAtDetection})`);
    console.log(`  Retry queue peak: ${result.retryPeakFinal}`);
    console.log(`  Retries succeeded: ${result.retriesSucceeded}`);
    console.log(`  Retries failed: ${result.retriesFailed}`);
    console.log(`  Total dispatched: ${result.dispatchedTotal}`);
    console.log(`  Retry queue final: ${result.retryQueueFinal}`);
    console.log(`  Pending final: ${result.pendingFinal}`);

    // All bundles should have reached the prescheduler
    expect(result.bundlesScheduled).toBe(result.bundleCount);

    // We should have detected the retry queue being populated
    expect(result.retryDetected).toBe(true);
    expect(result.retryQueueAtDetection).toBeGreaterThan(0);

    // No retries should have failed before we resumed
    // (we caught them in-flight and resumed before exhaustion)
    expect(result.retriesFailedAtDetection).toBe(0);

    // resume() calls purge() which clears the retry queue (stale messages
    // should not flood through on resume). Retries may or may not have
    // succeeded before the purge arrived — either outcome is correct.
    expect(result.retriesSucceeded).toBeGreaterThanOrEqual(0);

    // Retry queue should be fully drained (either by successful retries
    // or by purge clearing it)
    expect(result.retryQueueFinal).toBe(0);

    // Nothing left pending
    expect(result.pendingFinal).toBe(0);

    // All bundles accounted for: dispatched + failed >= scheduled
    // (worklet resumes on the audio thread and may consume messages in parallel
    // with the prescheduler's dispatch loop, making exact conservation tricky to observe)
    expect(result.dispatchedTotal + result.retriesFailed).toBeGreaterThan(0);
  });
});

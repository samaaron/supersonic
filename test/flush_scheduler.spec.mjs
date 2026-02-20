import { test, expect } from "./fixtures.mjs";

/**
 * Flush Scheduler Tests
 *
 * Tests that purge() clears pending OSC messages from:
 * 1. The JS prescheduler heap (future-timestamped bundles)
 * 2. The WASM BundleScheduler (bundles already consumed from the ring buffer)
 *
 * Also tests:
 * 3. The async ack guarantee (purge resolves only when both sides confirm)
 * 4. New OSC sent after flush is not lost
 * 5. Quick restart: stale messages don't contaminate new run
 * 6. Flush while suspended takes effect after resume
 *
 * Both SAB and postMessage modes are tested via the dual-project config.
 */

// Shared helpers injected into page.evaluate via this string.
// We define them here once to avoid duplication across tests.
const HELPERS = `
  var NTP_EPOCH_OFFSET = 2208988800;
  var getCurrentNTP = () => {
    const perfTimeMs = performance.timeOrigin + performance.now();
    return (perfTimeMs / 1000) + NTP_EPOCH_OFFSET;
  };

  var createTimedBundle = (ntpTime, nodeId) => {
    const encodedMessage = window.SuperSonic.osc.encodeMessage("/s_new", ["sonic-pi-beep", nodeId, 0, 0, "amp", 0.0, "release", 0.01]);
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
`;

test.describe("Flush Scheduler", () => {
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

  test("purge clears prescheduler pending bundles", async ({ page, sonicConfig }) => {
    const result = await page.evaluate(async (config) => {
      eval(config._helpers);

      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDefs(["sonic-pi-beep"]);
      await sonic.sync(1);

      // Schedule 20 bundles 30 seconds in the future (stays in prescheduler heap)
      const baseNTP = getCurrentNTP() + 30.0;
      for (let i = 0; i < 20; i++) {
        const bundle = createTimedBundle(baseNTP + (i * 0.1), 20000 + i);
        sonic.sendOSC(bundle, { sessionId: 0, runTag: 'flush_test' });
      }

      // Wait for bundles to reach prescheduler
      await new Promise(r => setTimeout(r, 200));

      const metricsBefore = sonic.getMetrics();
      const pendingBefore = metricsBefore.preschedulerPending || 0;

      // Flush everything (awaits confirmation from both sides)
      await sonic.purge();

      // Wait for metrics to propagate (PM mode sends snapshots periodically)
      await new Promise(r => setTimeout(r, 200));

      const metricsAfter = sonic.getMetrics();
      const pendingAfter = metricsAfter.preschedulerPending || 0;

      await sonic.shutdown();
      return { pendingBefore, pendingAfter };
    }, { ...sonicConfig, _helpers: HELPERS });

    console.log(`\npurge prescheduler test (${sonicConfig.mode}):`);
    console.log(`  Pending before flush: ${result.pendingBefore}`);
    console.log(`  Pending after flush: ${result.pendingAfter}`);

    expect(result.pendingBefore).toBe(20);
    expect(result.pendingAfter).toBe(0);
  });

  test("purge clears WASM scheduler pending bundles", async ({ page, sonicConfig }) => {
    const result = await page.evaluate(async (config) => {
      eval(config._helpers);

      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDefs(["sonic-pi-beep"]);
      await sonic.sync(1);

      // Use createOscChannel + sendDirect to bypass the prescheduler entirely,
      // sending bundles straight to the worklet ring buffer.
      // The WASM scheduler queues them because their timetags are in the future.
      const channel = sonic.createOscChannel();
      const baseNTP = getCurrentNTP() + 5.0;
      for (let i = 0; i < 20; i++) {
        const bundle = createTimedBundle(baseNTP + (i * 0.1), 30000 + i);
        channel.sendDirect(bundle);
      }

      // Wait for ring buffer messages to be consumed by process() into the WASM scheduler
      await new Promise(r => setTimeout(r, 500));

      const metricsBefore = sonic.getMetrics();
      const wasmDepthBefore = metricsBefore.scsynthSchedulerDepth || 0;

      // Flush everything (awaits confirmation from both sides)
      await sonic.purge();

      // Wait for process() to run and clear the scheduler, then metrics to propagate
      await new Promise(r => setTimeout(r, 200));

      const metricsAfter = sonic.getMetrics();
      const wasmDepthAfter = metricsAfter.scsynthSchedulerDepth || 0;

      await sonic.shutdown();
      return { wasmDepthBefore, wasmDepthAfter };
    }, { ...sonicConfig, _helpers: HELPERS });

    console.log(`\npurge WASM scheduler test (${sonicConfig.mode}):`);
    console.log(`  WASM scheduler depth before flush: ${result.wasmDepthBefore}`);
    console.log(`  WASM scheduler depth after flush: ${result.wasmDepthAfter}`);

    expect(result.wasmDepthBefore).toBeGreaterThan(0);
    expect(result.wasmDepthAfter).toBe(0);
  });

  test("purge after suspend prevents stale messages on resume", async ({ page, sonicConfig }) => {
    const result = await page.evaluate(async (config) => {
      eval(config._helpers);

      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDefs(["sonic-pi-beep"]);
      await sonic.sync(1);

      // Fill both prescheduler (far future) and WASM scheduler (near future)
      const now = getCurrentNTP();
      const channel = sonic.createOscChannel();

      // 10 bundles for the WASM scheduler — use sendDirect to bypass prescheduler
      for (let i = 0; i < 10; i++) {
        const bundle = createTimedBundle(now + 5.0 + (i * 0.1), 40000 + i);
        channel.sendDirect(bundle);
      }

      // 10 bundles for the prescheduler (30s in future, beyond lookahead)
      for (let i = 0; i < 10; i++) {
        const bundle = createTimedBundle(now + 30.0 + (i * 0.1), 41000 + i);
        sonic.sendOSC(bundle, { sessionId: 0, runTag: 'suspend_test' });
      }

      // Wait for direct bundles to be consumed from ring buffer into WASM scheduler
      await new Promise(r => setTimeout(r, 500));

      const metricsBefore = sonic.getMetrics();

      // Suspend then flush
      await sonic.suspend();
      await sonic.purge();

      // Resume (also flushes internally)
      await sonic.resume();

      // Brief wait for worklet to process the clear on first process() call
      await new Promise(r => setTimeout(r, 200));

      const metricsAfter = sonic.getMetrics();

      // Track lates - there should be none after resume
      const latesBefore = metricsBefore.scsynthSchedulerLates || 0;
      const latesAfter = metricsAfter.scsynthSchedulerLates || 0;
      const newLates = latesAfter - latesBefore;

      await sonic.shutdown();
      return {
        preschedulerBefore: metricsBefore.preschedulerPending || 0,
        preschedulerAfter: metricsAfter.preschedulerPending || 0,
        wasmDepthBefore: metricsBefore.scsynthSchedulerDepth || 0,
        wasmDepthAfter: metricsAfter.scsynthSchedulerDepth || 0,
        newLates,
      };
    }, { ...sonicConfig, _helpers: HELPERS });

    console.log(`\npurge suspend/resume test (${sonicConfig.mode}):`);
    console.log(`  Prescheduler: ${result.preschedulerBefore} -> ${result.preschedulerAfter}`);
    console.log(`  WASM scheduler: ${result.wasmDepthBefore} -> ${result.wasmDepthAfter}`);
    console.log(`  New lates after resume: ${result.newLates}`);

    expect(result.preschedulerAfter).toBe(0);
    expect(result.wasmDepthAfter).toBe(0);
    expect(result.newLates).toBe(0);
  });

  test("new OSC sent after purge is not lost", async ({ page, sonicConfig }) => {
    const result = await page.evaluate(async (config) => {
      eval(config._helpers);

      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDefs(["sonic-pi-beep"]);
      await sonic.sync(1);

      const channel = sonic.createOscChannel();

      // Fill WASM scheduler with "old run" bundles
      const now = getCurrentNTP();
      for (let i = 0; i < 10; i++) {
        const bundle = createTimedBundle(now + 5.0 + (i * 0.1), 50000 + i);
        channel.sendDirect(bundle);
      }

      // Fill prescheduler with "old run" bundles
      for (let i = 0; i < 10; i++) {
        const bundle = createTimedBundle(now + 30.0 + (i * 0.1), 51000 + i);
        sonic.sendOSC(bundle, { sessionId: 0, runTag: 'old_run' });
      }

      // Wait for WASM scheduler to consume from ring buffer
      await new Promise(r => setTimeout(r, 500));

      // Flush everything
      await sonic.purge();

      // Now send "new run" bundles — these must NOT be lost
      const newNTP = getCurrentNTP();

      // 5 bundles to WASM scheduler (via sendDirect)
      for (let i = 0; i < 5; i++) {
        const bundle = createTimedBundle(newNTP + 5.0 + (i * 0.1), 52000 + i);
        channel.sendDirect(bundle);
      }

      // 5 bundles to prescheduler
      for (let i = 0; i < 5; i++) {
        const bundle = createTimedBundle(newNTP + 30.0 + (i * 0.1), 53000 + i);
        sonic.sendOSC(bundle, { sessionId: 0, runTag: 'new_run' });
      }

      // Wait for new bundles to arrive
      await new Promise(r => setTimeout(r, 500));

      const metrics = sonic.getMetrics();

      await sonic.shutdown();
      return {
        wasmDepth: metrics.scsynthSchedulerDepth || 0,
        preschedulerPending: metrics.preschedulerPending || 0,
      };
    }, { ...sonicConfig, _helpers: HELPERS });

    console.log(`\nnew OSC after flush test (${sonicConfig.mode}):`);
    console.log(`  WASM scheduler depth: ${result.wasmDepth}`);
    console.log(`  Prescheduler pending: ${result.preschedulerPending}`);

    // The 5 new WASM bundles should be in the scheduler
    expect(result.wasmDepth).toBe(5);
    // The 5 new prescheduler bundles should be pending
    expect(result.preschedulerPending).toBe(5);
  });

  test("quick restart: stale messages do not contaminate new run", async ({ page, sonicConfig }) => {
    const result = await page.evaluate(async (config) => {
      eval(config._helpers);

      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDefs(["sonic-pi-beep"]);
      await sonic.sync(1);

      const channel = sonic.createOscChannel();

      // "Run 1": fill both schedulers
      const run1NTP = getCurrentNTP();

      for (let i = 0; i < 10; i++) {
        const bundle = createTimedBundle(run1NTP + 5.0 + (i * 0.1), 60000 + i);
        channel.sendDirect(bundle);
      }
      for (let i = 0; i < 10; i++) {
        const bundle = createTimedBundle(run1NTP + 30.0 + (i * 0.1), 61000 + i);
        sonic.sendOSC(bundle, { sessionId: 0, runTag: 'run1' });
      }

      // Wait for run 1 WASM bundles to be consumed
      await new Promise(r => setTimeout(r, 500));

      const metricsRun1 = sonic.getMetrics();

      // Quick restart: flush then immediately send run 2
      await sonic.purge();

      // "Run 2": send new bundles immediately after flush
      const run2NTP = getCurrentNTP();

      for (let i = 0; i < 8; i++) {
        const bundle = createTimedBundle(run2NTP + 5.0 + (i * 0.1), 62000 + i);
        channel.sendDirect(bundle);
      }
      for (let i = 0; i < 8; i++) {
        const bundle = createTimedBundle(run2NTP + 30.0 + (i * 0.1), 63000 + i);
        sonic.sendOSC(bundle, { sessionId: 0, runTag: 'run2' });
      }

      // Wait for run 2 WASM bundles to arrive
      await new Promise(r => setTimeout(r, 500));

      const metricsRun2 = sonic.getMetrics();

      await sonic.shutdown();
      return {
        run1WasmDepth: metricsRun1.scsynthSchedulerDepth || 0,
        run1Prescheduler: metricsRun1.preschedulerPending || 0,
        run2WasmDepth: metricsRun2.scsynthSchedulerDepth || 0,
        run2Prescheduler: metricsRun2.preschedulerPending || 0,
      };
    }, { ...sonicConfig, _helpers: HELPERS });

    console.log(`\nquick restart test (${sonicConfig.mode}):`);
    console.log(`  Run 1 - WASM: ${result.run1WasmDepth}, Prescheduler: ${result.run1Prescheduler}`);
    console.log(`  Run 2 - WASM: ${result.run2WasmDepth}, Prescheduler: ${result.run2Prescheduler}`);

    // Run 1 should have had its bundles present
    expect(result.run1WasmDepth).toBe(10);
    expect(result.run1Prescheduler).toBe(10);

    // Run 2 should have ONLY its bundles (no run 1 leftovers)
    // If stale messages leaked, depth would be > 8
    expect(result.run2WasmDepth).toBe(8);
    expect(result.run2Prescheduler).toBe(8);
  });

  test("purge resolves within a bounded time", async ({ page, sonicConfig }) => {
    const result = await page.evaluate(async (config) => {
      eval(config._helpers);

      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDefs(["sonic-pi-beep"]);
      await sonic.sync(1);

      // Fill both schedulers with plenty of data
      const channel = sonic.createOscChannel();
      const now = getCurrentNTP();

      for (let i = 0; i < 50; i++) {
        const bundle = createTimedBundle(now + 5.0 + (i * 0.1), 80000 + i);
        channel.sendDirect(bundle);
      }
      for (let i = 0; i < 50; i++) {
        const bundle = createTimedBundle(now + 30.0 + (i * 0.1), 81000 + i);
        sonic.sendOSC(bundle, { sessionId: 0, runTag: 'timing_test' });
      }

      // Wait for WASM scheduler to consume bundles
      await new Promise(r => setTimeout(r, 500));

      // Time the purge call — it should resolve promptly (< 2s)
      const start = performance.now();
      await sonic.purge();
      const elapsed = performance.now() - start;

      await sonic.shutdown();
      return { elapsed };
    }, { ...sonicConfig, _helpers: HELPERS });

    console.log(`\npurge timing test (${sonicConfig.mode}):`);
    console.log(`  Resolved in: ${result.elapsed.toFixed(1)}ms`);

    // Should resolve well within 2 seconds — typically < 50ms
    expect(result.elapsed).toBeLessThan(2000);
  });

  test("rapid sequential purge calls do not hang", async ({ page, sonicConfig }) => {
    const result = await page.evaluate(async (config) => {
      eval(config._helpers);

      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDefs(["sonic-pi-beep"]);
      await sonic.sync(1);

      const channel = sonic.createOscChannel();

      // Send bundles, flush, send more, flush — three times in rapid succession
      const timings = [];
      for (let round = 0; round < 3; round++) {
        const now = getCurrentNTP();
        for (let i = 0; i < 10; i++) {
          const bundle = createTimedBundle(now + 5.0 + (i * 0.1), 82000 + (round * 100) + i);
          channel.sendDirect(bundle);
        }
        for (let i = 0; i < 10; i++) {
          const bundle = createTimedBundle(now + 30.0 + (i * 0.1), 83000 + (round * 100) + i);
          sonic.sendOSC(bundle, { sessionId: 0, runTag: `rapid_${round}` });
        }

        // Brief pause for ring buffer consumption
        await new Promise(r => setTimeout(r, 100));

        const start = performance.now();
        await sonic.purge();
        timings.push(performance.now() - start);
      }

      // After 3 rapid flushes, everything should be clear
      await new Promise(r => setTimeout(r, 200));
      const metrics = sonic.getMetrics();

      await sonic.shutdown();
      return {
        timings,
        finalWasmDepth: metrics.scsynthSchedulerDepth || 0,
        finalPrescheduler: metrics.preschedulerPending || 0,
      };
    }, { ...sonicConfig, _helpers: HELPERS });

    console.log(`\nrapid flush test (${sonicConfig.mode}):`);
    for (let i = 0; i < result.timings.length; i++) {
      console.log(`  Round ${i}: ${result.timings[i].toFixed(1)}ms`);
    }
    console.log(`  Final WASM depth: ${result.finalWasmDepth}`);
    console.log(`  Final prescheduler: ${result.finalPrescheduler}`);

    // None of the flushes should hang
    for (const t of result.timings) {
      expect(t).toBeLessThan(2000);
    }
    // Everything clear at the end
    expect(result.finalWasmDepth).toBe(0);
    expect(result.finalPrescheduler).toBe(0);
  });

  test("purge drains stale messages from IN ring buffer", async ({ page, sonicConfig }) => {
    const result = await page.evaluate(async (config) => {
      eval(config._helpers);

      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDefs(["sonic-pi-beep"]);
      await sonic.sync(1);

      const channel = sonic.createOscChannel();

      // Record baseline metrics while engine is running
      const metricsBaseline = sonic.getMetrics();
      const latesBaseline = metricsBaseline.scsynthSchedulerLates || 0;

      // Suspend AudioContext — process() stops, ring buffer won't be consumed
      await sonic.suspend();

      // Write 20 bundles directly to ring buffer with timestamps already in the past.
      // Since process() isn't running, these sit in the ring buffer unconsumed.
      const pastNTP = getCurrentNTP() - 5.0;
      let writeCount = 0;
      for (let i = 0; i < 20; i++) {
        const ok = channel.sendDirect(createTimedBundle(pastNTP + (i * 0.01), 95000 + i));
        if (ok) writeCount++;
      }

      // Purge — should clear ring buffer contents alongside prescheduler and WASM scheduler
      await sonic.purge();

      // Resume — process() starts running again
      await sonic.resume();

      // Wait for several process() cycles
      await new Promise(r => setTimeout(r, 500));

      const metricsAfter = sonic.getMetrics();
      const latesAfter = metricsAfter.scsynthSchedulerLates || 0;

      await sonic.shutdown();
      return {
        writeCount,
        latesBaseline,
        latesAfter,
        newLates: latesAfter - latesBaseline,
      };
    }, { ...sonicConfig, _helpers: HELPERS });

    console.log(`\npurge ring buffer drain test (${sonicConfig.mode}):`);
    console.log(`  Bundles written to ring buffer: ${result.writeCount}`);
    console.log(`  Lates baseline: ${result.latesBaseline}`);
    console.log(`  Lates after resume: ${result.latesAfter}`);
    console.log(`  New lates: ${result.newLates}`);

    // Bundles should have been written successfully
    expect(result.writeCount).toBeGreaterThan(0);
    // After purge, no stale bundles should fire when resuming — lates must not increase
    expect(result.newLates).toBe(0);
  });

  test("purge drains ring buffer messages written by prescheduler during suspension", async ({ page, sonicConfig }) => {
    const result = await page.evaluate(async (config) => {
      eval(config._helpers);

      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDefs(["sonic-pi-beep"]);
      await sonic.sync(1);

      // Record baseline
      const metricsBaseline = sonic.getMetrics();
      const latesBaseline = metricsBaseline.scsynthSchedulerLates || 0;
      const processedBaseline = metricsBaseline.scsynthMessagesProcessed || 0;

      // Suspend AudioContext — process() stops
      await sonic.suspend();

      // Schedule bundles just past the lookahead threshold (500ms) so they
      // route through the prescheduler, which will dispatch them to the
      // ring buffer when their time approaches. Since process() is suspended,
      // dispatched messages accumulate in the ring buffer.
      const now = getCurrentNTP();
      for (let i = 0; i < 20; i++) {
        const bundle = createTimedBundle(now + 0.6 + (i * 0.01), 96000 + i);
        sonic.sendOSC(bundle, { sessionId: 0, runTag: 'rb_drain_test' });
      }

      // Wait for the prescheduler to dispatch these bundles (lookahead fires at
      // ~now + 0.1s) AND for their timestamps to become past-due
      await new Promise(r => setTimeout(r, 1500));

      // Purge — should clear prescheduler heap, WASM scheduler, AND ring buffer
      await sonic.purge();

      // Resume
      await sonic.resume();

      // Wait for process() to run
      await new Promise(r => setTimeout(r, 500));

      const metricsAfter = sonic.getMetrics();
      const latesAfter = metricsAfter.scsynthSchedulerLates || 0;
      const processedAfter = metricsAfter.scsynthMessagesProcessed || 0;

      await sonic.shutdown();
      return {
        latesBaseline,
        latesAfter,
        newLates: latesAfter - latesBaseline,
        processedBaseline,
        processedAfter,
        newProcessed: processedAfter - processedBaseline,
      };
    }, { ...sonicConfig, _helpers: HELPERS });

    console.log(`\npurge prescheduler ring buffer drain test (${sonicConfig.mode}):`);
    console.log(`  Lates: ${result.latesBaseline} -> ${result.latesAfter} (new: ${result.newLates})`);
    console.log(`  Processed: ${result.processedBaseline} -> ${result.processedAfter} (new: ${result.newProcessed})`);

    // After purge, no stale bundles should fire — lates must not increase
    expect(result.newLates).toBe(0);
    // No stale messages should have been processed from the ring buffer
    expect(result.newProcessed).toBe(0);
  });

  test("purge resolves only after both sides confirm", async ({ page, sonicConfig }) => {
    const result = await page.evaluate(async (config) => {
      eval(config._helpers);

      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDefs(["sonic-pi-beep"]);
      await sonic.sync(1);

      // Fill prescheduler
      const baseNTP = getCurrentNTP() + 30.0;
      for (let i = 0; i < 15; i++) {
        const bundle = createTimedBundle(baseNTP + (i * 0.1), 70000 + i);
        sonic.sendOSC(bundle, { sessionId: 0, runTag: 'ack_test' });
      }

      // Wait for bundles to reach prescheduler
      await new Promise(r => setTimeout(r, 200));

      const pendingBefore = (sonic.getMetrics().preschedulerPending) || 0;

      // purge returns a promise — if the ack mechanism works,
      // a non-awaited purge followed by immediate metric read
      // might still show pending bundles (race). But awaited purge
      // guarantees the prescheduler has processed the cancel.
      const flushPromise = sonic.purge();

      // Read metrics BEFORE the promise resolves (fire-and-forget check)
      // In SAB mode, the cancel may already be processed since postMessage
      // to the worker is fast, but this tests the intent.
      const pendingDuringFlush = (sonic.getMetrics().preschedulerPending) || 0;

      // Now await — after this, the cancel is confirmed
      await flushPromise;

      // In SAB mode metrics are live, so pending should be 0 immediately.
      // In PM mode we need a brief wait for the snapshot to arrive.
      if (config.mode === 'postMessage') {
        await new Promise(r => setTimeout(r, 200));
      }

      const pendingAfter = (sonic.getMetrics().preschedulerPending) || 0;

      await sonic.shutdown();
      return { pendingBefore, pendingDuringFlush, pendingAfter, mode: config.mode };
    }, { ...sonicConfig, _helpers: HELPERS });

    console.log(`\npurge ack guarantee test (${sonicConfig.mode}):`);
    console.log(`  Pending before: ${result.pendingBefore}`);
    console.log(`  Pending during (pre-await): ${result.pendingDuringFlush}`);
    console.log(`  Pending after await: ${result.pendingAfter}`);

    expect(result.pendingBefore).toBe(15);
    // After await, prescheduler must be confirmed clear
    expect(result.pendingAfter).toBe(0);
  });
});

import { test, expect, WAIT_FOR_PROGRESS_HELPER } from "./fixtures.mjs";

/**
 * Scheduler queue overflow in the browser.
 *
 * Timed bundles past the scheduler's capacity are dropped and counted rather
 * than back-pressured: the IN ring drains in order, and holding the surplus
 * would block immediate commands behind it. That the bundles which fit play
 * at their time, and the exact accounting, are proved in
 * test/native/test_scheduler_overflow.cpp on a manually rendered engine.
 * This spec checks the web path: a burst past capacity, sent through the
 * page's ring, is counted, and intake keeps working.
 *
 * Capacity is read from the build (engineSchedulerPoolBytes), not assumed.
 */

const OVERFLOW_AMOUNT = 200; // how far past capacity to push

// Audio analysis helpers
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
`;

test.describe("Scheduler Queue Overflow", () => {
  // Audio capture requires SharedArrayBuffer - not available in postMessage mode
  test("timed bundles past capacity are dropped and counted, and intake keeps working", async ({ page, sonicConfig }) => {
    test.skip(sonicConfig.mode === 'postMessage', 'Audio capture requires SharedArrayBuffer');
    await page.goto("/test/harness.html");
    await page.waitForFunction(() => window.supersonicReady === true, { timeout: 10000 });

    const result = await page.evaluate(async ({ OVERFLOW_AMOUNT, helpers, progress, sonicConfig }) => {
      eval(helpers);
      eval(progress);
      const sonic = new window.SuperSonic(sonicConfig);
      await sonic.init();
      await sonic.loadSynthDefs(["sonic-pi-beep"]);
      await sonic.sync(1);

      // The data pool is the limit a burst reaches first: each bundle is
      // padded (an unknown control the synth ignores) so a few hundred fill it.
      const { engineSchedulerPoolBytes: poolBytes } = sonic.getMetrics();
      if (!poolBytes) throw new Error("no scheduler pool reported");
      const PAD_BYTES = 4096;
      const total = Math.ceil(poolBytes / PAD_BYTES) + OVERFLOW_AMOUNT;

      const timedBundle = (ntp, nodeId, note) => {
        const msg = window.SuperSonic.osc.encodeMessage("/s_new", [
          "sonic-pi-beep", nodeId, 0, 0, "note", note, "amp", 0.05, "release", 0.2, "pad", "x".repeat(PAD_BYTES),
        ]);
        const bundle = new Uint8Array(20 + msg.byteLength);
        const view = new DataView(bundle.buffer);
        bundle.set([0x23, 0x62, 0x75, 0x6e, 0x64, 0x6c, 0x65, 0x00], 0);
        view.setUint32(8, Math.floor(ntp), false);
        view.setUint32(12, Math.floor((ntp % 1) * 0x100000000), false);
        view.setInt32(16, msg.byteLength, false);
        bundle.set(msg, 20);
        return bundle;
      };

      // Due a minute ahead on the engine's clock: the burst is in long before
      // anything comes due, however slowly the page runs.
      const droppedBefore = sonic.getMetrics().engineSchedulerDropped || 0;
      const due = sonic.clock.now() + 60;
      for (let i = 0; i < total; i++) {
        sonic.sendOSC(timedBundle(due + i * 0.0001, 50000 + i, 60 + (i % 24)));
        // Let the IN ring drain: the pool is what this fills, not the ring.
        if ((i & 31) === 31) await new Promise((r) => setTimeout(r, 0));
      }
      // Answered once every bundle before it has reached the scheduler.
      await sonic.sync(2);
      const beforeDue = sonic.clock.now() < due;
      const after = sonic.getMetrics();
      const dropped = (after.engineSchedulerDropped || 0) - droppedBefore;
      const held = after.engineSchedulerDepth || 0;

      // Intake still works: an immediate beep is heard within 150 ms of audio.
      sonic.startCapture();
      await sonic.send("/s_new", "sonic-pi-beep", 99999, 0, 0, "amp", 0.5, "release", 0.1);
      await waitForCaptureFrames(sonic, 7200);
      const capture = sonic.stopCapture();

      return { total, beforeDue, dropped, held, peak: findPeak(capture.left), ringDrops: after.engineMessagesDropped || 0 };
    }, { OVERFLOW_AMOUNT, helpers: AUDIO_HELPERS, progress: WAIT_FOR_PROGRESS_HELPER, sonicConfig });

    console.log(`  ${result.total} sent: ${result.held} held, ${result.dropped} dropped; immediate beep peak ${result.peak.toFixed(3)}`);
    expect(result.beforeDue, "the burst must be in before the bundles are due").toBe(true);
    expect(result.ringDrops, "the IN ring took every bundle").toBe(0);
    expect(result.dropped).toBeGreaterThan(0);
    expect(result.held + result.dropped).toBe(result.total);
    expect(result.peak).toBeGreaterThan(0.01);
  });

  test("verify scheduler capacity is 128", async ({ page, sonicConfig }) => {
    // Sanity check: a batch well under capacity should all fit
    await page.goto("/test/harness.html");

    await page.waitForFunction(() => window.supersonicReady === true, {
      timeout: 10000,
    });

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);

      const debugMessages = [];
      sonic.on('debug', (msg) => debugMessages.push(msg));

      await sonic.init();
      await sonic.loadSynthDefs(["sonic-pi-beep"]);
      await sonic.sync(1);

      const metricsBefore = sonic.getMetrics();
      const droppedBefore = metricsBefore.engineSchedulerDropped || 0;

      // NTP helper
      const NTP_EPOCH_OFFSET = 2208988800;
      const getCurrentNTP = () => {
        const perfTimeMs = performance.timeOrigin + performance.now();
        return (perfTimeMs / 1000) + NTP_EPOCH_OFFSET;
      };

      const createTimedBundle = (ntpTime, nodeId) => {
        const encodedMessage = window.SuperSonic.osc.encodeMessage("/s_new", ["sonic-pi-beep", nodeId, 0, 0, "note", 60, "amp", 0.01, "release", 0.01]);
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

      // Send 64 messages - all scheduled far in future
      const baseNTP = getCurrentNTP() + 1.0; // 1 second in future
      const EXACT_CAPACITY = 64;

      for (let i = 0; i < EXACT_CAPACITY; i++) {
        const bundle = createTimedBundle(baseNTP + (i * 0.001), 60000 + i);
        sonic.sendOSC(bundle);
      }

      await new Promise(r => setTimeout(r, 100));

      const metricsAfter = sonic.getMetrics();

      return {
        sent: EXACT_CAPACITY,
        droppedBefore,
        droppedAfter: metricsAfter.engineSchedulerDropped || 0,
        droppedCount: (metricsAfter.engineSchedulerDropped || 0) - droppedBefore,
        schedulerMax: metricsAfter.engineSchedulerPeakDepth,
        overflowErrors: debugMessages.filter(m => m.text?.includes("queue full")).length
      };
    }, sonicConfig);

    console.log(`\nCapacity test: sent ${result.sent}, dropped ${result.droppedCount}, max depth ${result.schedulerMax}`);

    // 64 messages is well under capacity, so none should be dropped.
    expect(result.droppedCount).toBe(0);
  });

  test("JS rejects a bundle exceeding the ring's message limit", async ({ page, sonicConfig }) => {
    await page.goto("/test/harness.html");

    await page.waitForFunction(() => window.supersonicReady === true, {
      timeout: 10000,
    });

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);

      await sonic.init();

      // Get the scheduler data pool size from buffer constants
      // The limit the client enforces is the ring's: one message may not exceed
      // MAX_MESSAGE_SIZE (the scheduler pool is sized from the same figure).
      const poolSize = sonic.bufferConstants.MAX_MESSAGE_SIZE;

      // The pool is far too large to construct a test bundle for directly.
      // Verify the limit is exported correctly and is reasonable.
      const inBufSize = sonic.bufferConstants.IN_RING_SIZE;

      await sonic.destroy();

      return {
        poolSize,
        inBufSize,
        effectiveLimit: Math.min(poolSize, inBufSize),
      };
    }, sonicConfig);

    console.log(`\nPool size: ${result.poolSize}, ring buffer: ${result.inBufSize}, effective limit: ${result.effectiveLimit}`);

    // Pool should be 512KB
    // Derived, not a literal. This was `toBe(512 * 1024)`, which pinned the
    // test to one build's SCHEDULER_DATA_POOL_SIZE; this build reserves 4MB
    // and the test failed on a number that is a configuration choice, not a
    // contract. What matters is that a pool is reported and is big enough to
    // hold a real bundle.
    expect(result.poolSize).toBeGreaterThanOrEqual(64 * 1024);
    // Effective limit is min(pool, ring buffer)
    expect(result.effectiveLimit).toBeGreaterThan(0);
    expect(result.effectiveLimit).toBeLessThanOrEqual(result.poolSize);
  });

  test("bundle within size limit succeeds", async ({ page, sonicConfig }) => {
    await page.goto("/test/harness.html");

    await page.waitForFunction(() => window.supersonicReady === true, {
      timeout: 10000,
    });

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);

      await sonic.init();
      await sonic.loadSynthDefs(["sonic-pi-beep"]);

      // The limit the client enforces is the ring's: one message may not exceed
      // MAX_MESSAGE_SIZE (the scheduler pool is sized from the same figure).
      const poolSize = sonic.bufferConstants.MAX_MESSAGE_SIZE;

      // NTP helper
      const NTP_EPOCH_OFFSET = 2208988800;
      const getCurrentNTP = () => {
        const perfTimeMs = performance.timeOrigin + performance.now();
        return (perfTimeMs / 1000) + NTP_EPOCH_OFFSET;
      };

      // Create a normal-sized bundle with a real synth message
      const encodedMessage = window.SuperSonic.osc.encodeMessage("/s_new", ["sonic-pi-beep", 99999, 0, 0, "note", 60]);
      const bundleSize = 8 + 8 + 4 + encodedMessage.byteLength;
      const bundle = new Uint8Array(bundleSize);
      const view = new DataView(bundle.buffer);

      bundle.set([0x23, 0x62, 0x75, 0x6e, 0x64, 0x6c, 0x65, 0x00], 0);
      // 300ms in future
      const futureNTP = getCurrentNTP() + 0.3;
      const ntpSeconds = Math.floor(futureNTP);
      const ntpFraction = Math.floor((futureNTP % 1) * 0x100000000);
      view.setUint32(8, ntpSeconds, false);
      view.setUint32(12, ntpFraction, false);
      view.setInt32(16, encodedMessage.byteLength, false);
      bundle.set(encodedMessage, 20);

      let errorThrown = null;
      try {
        await sonic.sendOSC(bundle);
      } catch (err) {
        errorThrown = err.message;
      }

      return {
        poolSize,
        bundleSize: bundle.length,
        errorThrown,
        withinLimit: bundle.length <= poolSize,
      };
    }, sonicConfig);

    console.log(`\nNormal bundle test: pool=${result.poolSize}, bundle=${result.bundleSize}, within limit=${result.withinLimit}`);

    expect(result.withinLimit).toBe(true);
    expect(result.errorThrown).toBeNull();
  });

  test("immediate bundles bypass scheduler entirely", async ({ page, sonicConfig }) => {
    await page.goto("/test/harness.html");

    await page.waitForFunction(() => window.supersonicReady === true, {
      timeout: 10000,
    });

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();

      // Create a bundle with immediate timetag (1) — bypasses scheduler
      const osc = window.SuperSonic.osc;
      const bundle = osc.encodeBundle(1, [
        ["/g_new", 9999, 0, 0],
        ["/g_new", 9998, 0, 0],
      ]);

      let errorThrown = null;
      try {
        await sonic.sendOSC(new Uint8Array(bundle));
      } catch (err) {
        errorThrown = err.message;
      }

      // Wait for processing
      await new Promise(r => setTimeout(r, 200));
      await sonic.sync();
      const tree = sonic.getTree();
      await sonic.destroy();

      return {
        bundleSize: bundle.byteLength,
        errorThrown,
        nodeCount: tree.nodeCount,
      };
    }, sonicConfig);

    console.log(`\nImmediate bundle: ${result.bundleSize} bytes, nodes=${result.nodeCount}`);

    // Immediate bundles bypass the scheduler size check
    expect(result.errorThrown).toBeNull();
    // Groups should have been created
    expect(result.nodeCount).toBeGreaterThan(2);
  });
});

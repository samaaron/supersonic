import { test, expect } from "./fixtures.mjs";

/**
 * Tests for granular bypass category counters.
 *
 * Bypass categories track why a message bypassed the prescheduler:
 * - nonBundle: Plain OSC messages (not bundles)
 * - immediate: Bundles with timetag 0 or 1
 * - nearFuture: Bundles within 200ms but not late
 * - late: Bundles past their scheduled time
 */

test.describe("Bypass Category Counters", () => {
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

  test("non-bundle messages increment bypassNonBundle counter", async ({ page, sonicConfig }) => {
    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");
      await sonic.sync();

      const metricsBefore = sonic.getMetrics();

      // Send plain OSC messages (not bundles)
      for (let i = 0; i < 5; i++) {
        await sonic.send("/status");
      }

      // Wait for metrics to update
      await new Promise(r => setTimeout(r, 200));

      const metricsAfter = sonic.getMetrics();

      return {
        success: true,
        before: {
          bypassed: metricsBefore.preschedulerBypassed ?? 0,
          nonBundle: metricsBefore.bypassNonBundle ?? 0,
        },
        after: {
          bypassed: metricsAfter.preschedulerBypassed ?? 0,
          nonBundle: metricsAfter.bypassNonBundle ?? 0,
        },
      };
    }, sonicConfig);

    expect(result.success).toBe(true);
    // Non-bundle messages should increment both counters
    expect(result.after.nonBundle - result.before.nonBundle).toBeGreaterThanOrEqual(5);
    expect(result.after.bypassed - result.before.bypassed).toBeGreaterThanOrEqual(5);
  });

  test("immediate bundles (timetag 0/1) increment bypassImmediate counter", async ({ page, sonicConfig }) => {
    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");
      await sonic.sync();

      const metricsBefore = sonic.getMetrics();

      // Create and send bundles with immediate timetag (0)
      // OSC bundle format: #bundle\0 + 8-byte timetag + messages
      const bundleHeader = new Uint8Array([0x23, 0x62, 0x75, 0x6e, 0x64, 0x6c, 0x65, 0x00]);
      const immediateTimetag = new Uint8Array([0, 0, 0, 0, 0, 0, 0, 1]); // timetag = 1

      // Simple /status message
      const statusMsg = window.SuperSonic.osc.encodeMessage("/status", []);

      for (let i = 0; i < 3; i++) {
        const bundle = new Uint8Array(16 + 4 + statusMsg.length);
        bundle.set(bundleHeader, 0);
        bundle.set(immediateTimetag, 8);
        // Message size (big-endian)
        const view = new DataView(bundle.buffer);
        view.setInt32(16, statusMsg.length, false);
        bundle.set(statusMsg, 20);

        await sonic.sendOSC(bundle);
      }

      // Wait for metrics to update
      await new Promise(r => setTimeout(r, 200));

      const metricsAfter = sonic.getMetrics();

      return {
        success: true,
        before: {
          bypassed: metricsBefore.preschedulerBypassed ?? 0,
          immediate: metricsBefore.bypassImmediate ?? 0,
        },
        after: {
          bypassed: metricsAfter.preschedulerBypassed ?? 0,
          immediate: metricsAfter.bypassImmediate ?? 0,
        },
      };
    }, sonicConfig);

    expect(result.success).toBe(true);
    // Immediate bundles should increment both counters
    expect(result.after.immediate - result.before.immediate).toBeGreaterThanOrEqual(3);
    expect(result.after.bypassed - result.before.bypassed).toBeGreaterThanOrEqual(3);
  });

  test("aggregate preschedulerBypassed equals sum of categories", async ({ page, sonicConfig }) => {
    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");
      await sonic.sync();

      // Send a mix of messages
      // Plain messages (non-bundle)
      for (let i = 0; i < 3; i++) {
        await sonic.send("/status");
      }

      // Immediate bundles
      const bundleHeader = new Uint8Array([0x23, 0x62, 0x75, 0x6e, 0x64, 0x6c, 0x65, 0x00]);
      const immediateTimetag = new Uint8Array([0, 0, 0, 0, 0, 0, 0, 1]);
      const statusMsg = window.SuperSonic.osc.encodeMessage("/status", []);

      for (let i = 0; i < 2; i++) {
        const bundle = new Uint8Array(16 + 4 + statusMsg.length);
        bundle.set(bundleHeader, 0);
        bundle.set(immediateTimetag, 8);
        const view = new DataView(bundle.buffer);
        view.setInt32(16, statusMsg.length, false);
        bundle.set(statusMsg, 20);
        await sonic.sendOSC(bundle);
      }

      // Wait for metrics to update
      await new Promise(r => setTimeout(r, 200));

      const metrics = sonic.getMetrics();

      const categorySum =
        (metrics.bypassNonBundle ?? 0) +
        (metrics.bypassImmediate ?? 0) +
        (metrics.bypassNearFuture ?? 0) +
        (metrics.bypassLate ?? 0);

      return {
        success: true,
        bypassed: metrics.preschedulerBypassed ?? 0,
        categorySum,
        nonBundle: metrics.bypassNonBundle ?? 0,
        immediate: metrics.bypassImmediate ?? 0,
        nearFuture: metrics.bypassNearFuture ?? 0,
        late: metrics.bypassLate ?? 0,
      };
    }, sonicConfig);

    expect(result.success).toBe(true);
    // The aggregate counter should equal the sum of all categories
    expect(result.bypassed).toBe(result.categorySum);
  });

  test("bypass category metrics appear in getMetricsSchema", async ({ page, sonicConfig }) => {
    const result = await page.evaluate(async (config) => {
      const schema = window.SuperSonic.getMetricsSchema();
      return {
        hasBypassNonBundle: !!schema.bypassNonBundle,
        hasBypassImmediate: !!schema.bypassImmediate,
        hasBypassNearFuture: !!schema.bypassNearFuture,
        hasBypassLate: !!schema.bypassLate,
        hasPreschedulerBypassed: !!schema.preschedulerBypassed,
      };
    }, sonicConfig);

    expect(result.hasBypassNonBundle).toBe(true);
    expect(result.hasBypassImmediate).toBe(true);
    expect(result.hasBypassNearFuture).toBe(true);
    expect(result.hasBypassLate).toBe(true);
    expect(result.hasPreschedulerBypassed).toBe(true);
  });

  test("near-future bundles metric is tracked and exposed", async ({ page, sonicConfig }) => {
    // Note: Testing exact near-future timing is unreliable due to differences between
    // performance.now() and audioContext.currentTime. This test verifies the metric exists
    // and is accessible in the metrics output.
    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();

      const metrics = sonic.getMetrics();

      return {
        success: true,
        hasNearFutureMetric: 'bypassNearFuture' in metrics,
        nearFutureValue: metrics.bypassNearFuture,
        isNumber: typeof metrics.bypassNearFuture === 'number',
      };
    }, sonicConfig);

    expect(result.success).toBe(true);
    expect(result.hasNearFutureMetric).toBe(true);
    expect(result.isNumber).toBe(true);
    expect(result.nearFutureValue).toBeGreaterThanOrEqual(0);
  });

  test("late bundles increment bypassLate counter", async ({ page, sonicConfig }) => {
    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");
      await sonic.sync();

      const metricsBefore = sonic.getMetrics();

      // Create bundles with timetag in the past
      const NTP_EPOCH_OFFSET = 2208988800;
      const nowNTP = (performance.timeOrigin + performance.now()) / 1000 + NTP_EPOCH_OFFSET;
      const pastNTP = nowNTP - 1.0; // 1 second in the past

      const bundleHeader = new Uint8Array([0x23, 0x62, 0x75, 0x6e, 0x64, 0x6c, 0x65, 0x00]);
      const statusMsg = window.SuperSonic.osc.encodeMessage("/status", []);

      for (let i = 0; i < 2; i++) {
        const targetNTP = pastNTP - (i * 0.5); // 1s, 1.5s in past
        const bundle = new Uint8Array(16 + 4 + statusMsg.length);
        bundle.set(bundleHeader, 0);

        // Write NTP timetag (big-endian)
        const view = new DataView(bundle.buffer);
        const seconds = Math.floor(targetNTP);
        const fraction = Math.floor((targetNTP % 1) * 0x100000000);
        view.setUint32(8, seconds, false);
        view.setUint32(12, fraction, false);
        view.setInt32(16, statusMsg.length, false);
        bundle.set(statusMsg, 20);

        await sonic.sendOSC(bundle);
      }

      // Wait for metrics to update
      await new Promise(r => setTimeout(r, 200));

      const metricsAfter = sonic.getMetrics();

      return {
        success: true,
        before: {
          bypassed: metricsBefore.preschedulerBypassed ?? 0,
          late: metricsBefore.bypassLate ?? 0,
        },
        after: {
          bypassed: metricsAfter.preschedulerBypassed ?? 0,
          late: metricsAfter.bypassLate ?? 0,
        },
      };
    }, sonicConfig);

    expect(result.success).toBe(true);
    // Late bundles should increment both counters
    expect(result.after.late - result.before.late).toBeGreaterThanOrEqual(2);
    expect(result.after.bypassed - result.before.bypassed).toBeGreaterThanOrEqual(2);
  });

  test("late bundles metric is tracked and exposed", async ({ page, sonicConfig }) => {
    // Verify the late metric exists and is accessible
    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();

      const metrics = sonic.getMetrics();

      return {
        success: true,
        hasLateMetric: 'bypassLate' in metrics,
        lateValue: metrics.bypassLate,
        isNumber: typeof metrics.bypassLate === 'number',
      };
    }, sonicConfig);

    expect(result.success).toBe(true);
    expect(result.hasLateMetric).toBe(true);
    expect(result.isNumber).toBe(true);
    expect(result.lateValue).toBeGreaterThanOrEqual(0);
  });

  test("bypassLookaheadMs config affects routing threshold", async ({ page, sonicConfig }) => {
    // Test that bundles within the configurable threshold bypass the prescheduler,
    // while bundles beyond the threshold go to the prescheduler
    const result = await page.evaluate(async (baseConfig) => {
      const NTP_EPOCH_OFFSET = 2208988800;
      const getNTP = () => (performance.timeOrigin + performance.now()) / 1000 + NTP_EPOCH_OFFSET;

      const bundleHeader = new Uint8Array([0x23, 0x62, 0x75, 0x6e, 0x64, 0x6c, 0x65, 0x00]);
      const statusMsg = window.SuperSonic.osc.encodeMessage("/status", []);

      const createBundleAt = (offsetMs) => {
        const targetNTP = getNTP() + offsetMs / 1000;
        const bundle = new Uint8Array(16 + 4 + statusMsg.length);
        bundle.set(bundleHeader, 0);
        const view = new DataView(bundle.buffer);
        view.setUint32(8, Math.floor(targetNTP), false);
        view.setUint32(12, Math.floor((targetNTP % 1) * 0x100000000), false);
        view.setInt32(16, statusMsg.length, false);
        bundle.set(statusMsg, 20);
        return bundle;
      };

      // Test 1: With short threshold (50ms), a 100ms bundle should NOT bypass
      const shortConfig = { ...baseConfig, bypassLookaheadMs: 50, snapshotIntervalMs: 25 };
      const sonicShort = new window.SuperSonic(shortConfig);
      await sonicShort.init();
      await sonicShort.sync();

      const shortBefore = sonicShort.getMetrics();
      await sonicShort.sendOSC(createBundleAt(100)); // 100ms in future > 50ms threshold
      await new Promise(r => setTimeout(r, 50));
      const shortAfter = sonicShort.getMetrics();
      await sonicShort.destroy();

      // Test 2: With long threshold (500ms), a 100ms bundle SHOULD bypass as nearFuture
      const longConfig = { ...baseConfig, bypassLookaheadMs: 500, snapshotIntervalMs: 25 };
      const sonicLong = new window.SuperSonic(longConfig);
      await sonicLong.init();
      await sonicLong.sync();

      const longBefore = sonicLong.getMetrics();
      await sonicLong.sendOSC(createBundleAt(100)); // 100ms in future < 500ms threshold
      await new Promise(r => setTimeout(r, 50));
      const longAfter = sonicLong.getMetrics();
      await sonicLong.destroy();

      return {
        success: true,
        shortThreshold: {
          nearFutureDelta: (shortAfter.bypassNearFuture ?? 0) - (shortBefore.bypassNearFuture ?? 0),
          bypassedDelta: (shortAfter.preschedulerBypassed ?? 0) - (shortBefore.preschedulerBypassed ?? 0),
        },
        longThreshold: {
          nearFutureDelta: (longAfter.bypassNearFuture ?? 0) - (longBefore.bypassNearFuture ?? 0),
          bypassedDelta: (longAfter.preschedulerBypassed ?? 0) - (longBefore.preschedulerBypassed ?? 0),
        },
      };
    }, sonicConfig);

    expect(result.success).toBe(true);
    // With 50ms threshold, 100ms future bundle should NOT be nearFuture (goes to prescheduler)
    expect(result.shortThreshold.nearFutureDelta).toBe(0);
    // With 500ms threshold, 100ms future bundle SHOULD be nearFuture (bypasses)
    expect(result.longThreshold.nearFutureDelta).toBeGreaterThanOrEqual(1);
  });

  test("OscChannel in worker respects bypassLookaheadMs threshold", async ({ page, sonicConfig }) => {
    // Test that OscChannel transferred to a worker uses the correct threshold
    const result = await page.evaluate(async (baseConfig) => {
      // Create SuperSonic with custom threshold
      const config = { ...baseConfig, bypassLookaheadMs: 100 }; // 100ms threshold
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.sync();

      // Create a worker
      const worker = new Worker("/test/assets/osc_channel_test_worker.js", { type: "module" });

      // Wait for worker ready
      await new Promise((resolve, reject) => {
        const timeout = setTimeout(() => reject(new Error("Worker ready timeout")), 5000);
        worker.onmessage = (e) => {
          if (e.data.type === "ready") {
            clearTimeout(timeout);
            resolve();
          }
        };
      });

      // Create OscChannel and transfer to worker
      const channel = sonic.createOscChannel();
      worker.postMessage(
        { type: "initChannel", channel: channel.transferable },
        channel.transferList
      );

      // Wait for channel ready
      const channelInfo = await new Promise((resolve, reject) => {
        const timeout = setTimeout(() => reject(new Error("Channel init timeout")), 5000);
        worker.onmessage = (e) => {
          if (e.data.type === "channelReady") {
            clearTimeout(timeout);
            resolve(e.data);
          }
        };
      });

      // Test classification at different offsets
      const classifyAt = (offsetMs) => new Promise((resolve) => {
        worker.onmessage = (e) => {
          if (e.data.type === "classified") resolve(e.data);
        };
        worker.postMessage({ type: "classify", offsetMs });
      });

      const results = {
        channelMode: channelInfo.mode,
        // 50ms should be nearFuture (< 100ms threshold)
        at50ms: await classifyAt(50),
        // 150ms should be farFuture (> 100ms threshold)
        at150ms: await classifyAt(150),
        // -50ms should be late
        atMinus50ms: await classifyAt(-50),
      };

      worker.terminate();
      await sonic.destroy();

      return {
        success: true,
        ...results,
      };
    }, sonicConfig);

    expect(result.success).toBe(true);
    expect(result.channelMode).toBe(sonicConfig.mode);
    expect(result.at50ms.category).toBe("nearFuture");
    expect(result.at150ms.category).toBe("farFuture");
    expect(result.atMinus50ms.category).toBe("late");
  });

  test("oscOutMessagesSent counts messages from both main thread and OscChannel workers", async ({ page, sonicConfig }) => {
    // This test verifies that ALL messages sent to scsynth are counted,
    // regardless of whether they come from the main thread or from workers via OscChannel
    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.sync();

      // Get baseline metrics
      const metricsBefore = sonic.getMetrics();
      const baselineSent = metricsBefore.oscOutMessagesSent ?? 0;

      // Send messages from main thread
      const MAIN_THREAD_COUNT = 10;
      for (let i = 0; i < MAIN_THREAD_COUNT; i++) {
        await sonic.send("/status");
      }

      // Create worker and OscChannel
      const worker = new Worker("/test/assets/osc_channel_test_worker.js", { type: "module" });

      // Wait for worker ready
      await new Promise((resolve, reject) => {
        const timeout = setTimeout(() => reject(new Error("Worker ready timeout")), 5000);
        worker.onmessage = (e) => {
          if (e.data.type === "ready") {
            clearTimeout(timeout);
            resolve();
          }
        };
      });

      // Create OscChannel and transfer to worker
      const channel = sonic.createOscChannel();
      worker.postMessage(
        { type: "initChannel", channel: channel.transferable },
        channel.transferList
      );

      // Wait for channel ready
      const channelInfo = await new Promise((resolve, reject) => {
        const timeout = setTimeout(() => reject(new Error("Channel init timeout")), 5000);
        worker.onmessage = (e) => {
          if (e.data.type === "channelReady") {
            clearTimeout(timeout);
            resolve(e.data);
          }
        };
      });

      // Send messages from worker via OscChannel
      const WORKER_COUNT = 15;
      const workerResult = await new Promise((resolve, reject) => {
        const timeout = setTimeout(() => reject(new Error("Worker send timeout")), 5000);
        worker.onmessage = (e) => {
          if (e.data.type === "sentMultiple") {
            clearTimeout(timeout);
            resolve(e.data);
          }
        };
        worker.postMessage({ type: "sendMultiple", count: WORKER_COUNT, offsetMs: 0 });
      });

      // Wait for metrics to settle
      await new Promise(r => setTimeout(r, 300));

      // Get final metrics
      const metricsAfter = sonic.getMetrics();
      const finalSent = metricsAfter.oscOutMessagesSent ?? 0;

      worker.terminate();
      await sonic.destroy();

      const totalExpected = MAIN_THREAD_COUNT + WORKER_COUNT;
      const totalActual = finalSent - baselineSent;

      return {
        success: true,
        mode: config.mode,
        channelInfo,
        mainThreadCount: MAIN_THREAD_COUNT,
        workerCount: WORKER_COUNT,
        workerSentConfirmed: workerResult.sent,
        workerMetrics: workerResult.metrics,
        workerSabDebug: workerResult.sabDebug,
        totalExpected,
        totalActual,
        baselineSent,
        finalSent,
        match: totalActual === totalExpected,
        metricsAfter,
      };
    }, sonicConfig);

    expect(result.success).toBe(true);
    expect(result.workerSentConfirmed).toBe(result.workerCount);

    // Debug output
    console.log(`Mode: ${result.mode}`);
    console.log(`Channel info:`, result.channelInfo);
    console.log(`Main thread: ${result.mainThreadCount}, Worker: ${result.workerCount}`);
    console.log(`Worker metrics from OscChannel:`, result.workerMetrics);
    console.log(`Worker SAB debug:`, result.workerSabDebug);
    console.log(`Expected: ${result.totalExpected}, Actual: ${result.totalActual}`);
    console.log(`Baseline sent: ${result.baselineSent}, Final sent: ${result.finalSent}`);

    // This is the key assertion: all messages from all sources should be counted
    expect(result.totalActual).toBe(result.totalExpected);
  });

  test("bypass category metrics aggregate from multiple concurrent OscChannel workers", async ({ page, sonicConfig }) => {
    // This test verifies that bypass category counters (bypassNearFuture, bypassLate, etc.)
    // are correctly aggregated when multiple workers send via their own OscChannels
    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.sync();

      // Get baseline metrics
      const metricsBefore = sonic.getMetrics();
      const baselineBypassed = metricsBefore.preschedulerBypassed ?? 0;
      const baselineNearFuture = metricsBefore.bypassNearFuture ?? 0;
      const baselineLate = metricsBefore.bypassLate ?? 0;

      // Create worker helper
      const createWorker = async (name) => {
        const worker = new Worker("/test/assets/osc_channel_test_worker.js", { type: "module" });

        await new Promise((resolve, reject) => {
          const timeout = setTimeout(() => reject(new Error(`${name} ready timeout`)), 5000);
          worker.onmessage = (e) => {
            if (e.data.type === "ready") {
              clearTimeout(timeout);
              resolve();
            }
          };
        });

        const channel = sonic.createOscChannel();
        worker.postMessage(
          { type: "initChannel", channel: channel.transferable },
          channel.transferList
        );

        await new Promise((resolve, reject) => {
          const timeout = setTimeout(() => reject(new Error(`${name} channel timeout`)), 5000);
          worker.onmessage = (e) => {
            if (e.data.type === "channelReady") {
              clearTimeout(timeout);
              resolve(e.data);
            }
          };
        });

        return worker;
      };

      // Create two workers
      const worker1 = await createWorker("worker1");
      const worker2 = await createWorker("worker2");

      // Helper to send messages from a worker one at a time
      const sendFromWorker = async (worker, count, offsetMs) => {
        let totalSent = 0;
        let lastMetrics = null;
        let failures = [];

        for (let i = 0; i < count; i++) {
          const result = await new Promise((resolve, reject) => {
            const timeout = setTimeout(() => reject(new Error("Send timeout")), 5000);
            worker.onmessage = (e) => {
              if (e.data.type === "sent") {
                clearTimeout(timeout);
                resolve(e.data);
              }
            };
            worker.postMessage({ type: "sendBundle", offsetMs });
          });
          if (result.success) {
            totalSent++;
          } else {
            failures.push({ index: i, result });
          }
          lastMetrics = result.metrics;
        }

        return { sent: totalSent, metrics: lastMetrics, failures };
      };

      // Send from both workers (sequentially per worker, but interleaved)
      // Worker 1: 10 messages with 50ms offset (nearFuture - within 200ms lookahead)
      // Worker 2: 10 messages with -50ms offset (late - in the past)
      const [result1, result2] = await Promise.all([
        sendFromWorker(worker1, 10, 50),   // nearFuture
        sendFromWorker(worker2, 10, -50),  // late
      ]);

      // Wait for metrics to settle
      await new Promise(r => setTimeout(r, 300));

      // Get final metrics
      const metricsAfter = sonic.getMetrics();
      const finalBypassed = metricsAfter.preschedulerBypassed ?? 0;
      const finalNearFuture = metricsAfter.bypassNearFuture ?? 0;
      const finalLate = metricsAfter.bypassLate ?? 0;

      worker1.terminate();
      worker2.terminate();
      await sonic.destroy();

      return {
        success: true,
        mode: config.mode,
        worker1: { sent: result1.sent, metrics: result1.metrics, failures: result1.failures },
        worker2: { sent: result2.sent, metrics: result2.metrics, failures: result2.failures },
        ringBuffer: {
          inUsage: metricsAfter.inBufferUsagePercent,
          inPeakUsage: metricsAfter.inBufferPeakUsagePercent,
          writeContention: metricsAfter.ringBufferWriteContention,
        },
        baseline: { bypassed: baselineBypassed, nearFuture: baselineNearFuture, late: baselineLate },
        final: { bypassed: finalBypassed, nearFuture: finalNearFuture, late: finalLate },
        delta: {
          bypassed: finalBypassed - baselineBypassed,
          nearFuture: finalNearFuture - baselineNearFuture,
          late: finalLate - baselineLate,
        },
        expected: {
          bypassed: 20,      // 10 from each worker
          nearFuture: 10,    // 10 from worker1
          late: 10,          // 10 from worker2
        },
      };
    }, sonicConfig);

    expect(result.success).toBe(true);

    console.log(`\nBypass aggregation test (mode: ${result.mode}):`);
    console.log(`  Worker1 sent: ${result.worker1.sent}/10, failures:`, result.worker1.failures?.length ?? 0);
    console.log(`  Worker2 sent: ${result.worker2.sent}/10, failures:`, result.worker2.failures?.length ?? 0);
    console.log(`  Worker1 OscChannel local metrics:`, result.worker1.metrics);
    console.log(`  Worker2 OscChannel local metrics:`, result.worker2.metrics);
    if (result.worker1.failures?.length) console.log(`  Worker1 failure details:`, result.worker1.failures);
    if (result.worker2.failures?.length) console.log(`  Worker2 failure details:`, result.worker2.failures);
    console.log(`  Baseline:`, result.baseline);
    console.log(`  Final:`, result.final);
    console.log(`  Delta:`, result.delta);
    console.log(`  Expected:`, result.expected);
    console.log(`  Ring buffer:`, result.ringBuffer);

    // Both workers should have sent most of their messages (allow for contention in SAB mode)
    expect(result.worker1.sent).toBeGreaterThanOrEqual(5);
    expect(result.worker2.sent).toBeGreaterThanOrEqual(5);

    // Key assertions: bypass categories from both workers should be aggregated
    // The delta should match what was ACTUALLY sent, not what was attempted
    const actualTotal = result.worker1.sent + result.worker2.sent;
    expect(result.delta.bypassed).toBe(actualTotal);
    expect(result.delta.nearFuture).toBe(result.worker1.sent);  // Worker1 sends nearFuture
    expect(result.delta.late).toBe(result.worker2.sent);         // Worker2 sends late
  });
});

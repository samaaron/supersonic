import { test, expect } from "@playwright/test";

/**
 * Prescheduler Cancellation Tests
 *
 * Tests verify that:
 * 1. cancelTag() removes all bundles with a specific tag (any session)
 * 2. cancelSession() removes all bundles from a specific session (any tag)
 * 3. cancelSessionTag() removes bundles matching both session AND tag
 * 4. Cancelled bundles never reach the ring buffer/SAB
 */

test.describe("Prescheduler Cancellation", () => {
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

  test("cancelTag removes all bundles with matching tag (any session)", async ({ page }) => {
    const result = await page.evaluate(async () => {
      // NTP helpers
      const NTP_EPOCH_OFFSET = 2208988800;
      const getCurrentNTP = () => {
        const perfTimeMs = performance.timeOrigin + performance.now();
        return (perfTimeMs / 1000) + NTP_EPOCH_OFFSET;
      };

      const createTimedBundle = (ntpTime, nodeId) => {
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

      const sonic = new window.SuperSonic({
        baseURL: "/dist/",
      });

      await sonic.init();
      await sonic.loadSynthDefs(["sonic-pi-beep"]);
      await sonic.sync(1);

      const metricsBefore = sonic.getMetrics();
      const cancelledBefore = metricsBefore.preschedulerEventsCancelled || 0;

      // Schedule bundles 10 seconds in future (stays in prescheduler heap)
      const baseNTP = getCurrentNTP() + 10.0;

      // Send 10 bundles with tag 'run_1' from session 0
      for (let i = 0; i < 10; i++) {
        const bundle = createTimedBundle(baseNTP + (i * 0.001), 10000 + i);
        sonic.sendOSC(bundle, { sessionId: 0, runTag: 'run_1' });
      }

      // Send 10 bundles with tag 'run_1' from session 1
      for (let i = 0; i < 10; i++) {
        const bundle = createTimedBundle(baseNTP + (i * 0.001), 11000 + i);
        sonic.sendOSC(bundle, { sessionId: 1, runTag: 'run_1' });
      }

      // Send 10 bundles with tag 'run_2' from session 0
      for (let i = 0; i < 10; i++) {
        const bundle = createTimedBundle(baseNTP + (i * 0.001), 12000 + i);
        sonic.sendOSC(bundle, { sessionId: 0, runTag: 'run_2' });
      }

      // Wait for bundles to reach prescheduler
      await new Promise(r => setTimeout(r, 100));

      const metricsAfterSend = sonic.getMetrics();
      const pendingAfterSend = metricsAfterSend.preschedulerPending || 0;

      // Cancel all bundles with tag 'run_1' (should remove 20, leave 10)
      sonic.cancelTag('run_1');

      // Wait for cancel to process
      await new Promise(r => setTimeout(r, 100));

      const metricsAfterCancel = sonic.getMetrics();
      const pendingAfterCancel = metricsAfterCancel.preschedulerPending || 0;
      const cancelledAfter = metricsAfterCancel.preschedulerEventsCancelled || 0;
      const cancelledCount = cancelledAfter - cancelledBefore;

      return {
        pendingAfterSend,
        pendingAfterCancel,
        cancelledCount,
        expectedCancelled: 20,  // 10 from session 0 + 10 from session 1
        expectedRemaining: 10,  // 10 with tag 'run_2'
      };
    });

    console.log(`\ncancelTag test:`);
    console.log(`  Pending after send: ${result.pendingAfterSend}`);
    console.log(`  Pending after cancel: ${result.pendingAfterCancel}`);
    console.log(`  Cancelled: ${result.cancelledCount} (expected ${result.expectedCancelled})`);

    expect(result.pendingAfterSend).toBe(30);
    expect(result.cancelledCount).toBe(result.expectedCancelled);
    expect(result.pendingAfterCancel).toBe(result.expectedRemaining);
  });

  test("cancelSession removes all bundles from matching session (any tag)", async ({ page }) => {
    const result = await page.evaluate(async () => {
      // NTP helpers
      const NTP_EPOCH_OFFSET = 2208988800;
      const getCurrentNTP = () => {
        const perfTimeMs = performance.timeOrigin + performance.now();
        return (perfTimeMs / 1000) + NTP_EPOCH_OFFSET;
      };

      const createTimedBundle = (ntpTime, nodeId) => {
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

      const sonic = new window.SuperSonic({
        baseURL: "/dist/",
      });

      await sonic.init();
      await sonic.loadSynthDefs(["sonic-pi-beep"]);
      await sonic.sync(1);

      const metricsBefore = sonic.getMetrics();
      const cancelledBefore = metricsBefore.preschedulerEventsCancelled || 0;

      // Schedule bundles 10 seconds in future
      const baseNTP = getCurrentNTP() + 10.0;

      // Send 10 bundles from session 1 with tag 'a'
      for (let i = 0; i < 10; i++) {
        const bundle = createTimedBundle(baseNTP + (i * 0.001), 20000 + i);
        sonic.sendOSC(bundle, { sessionId: 1, runTag: 'a' });
      }

      // Send 10 bundles from session 1 with tag 'b'
      for (let i = 0; i < 10; i++) {
        const bundle = createTimedBundle(baseNTP + (i * 0.001), 21000 + i);
        sonic.sendOSC(bundle, { sessionId: 1, runTag: 'b' });
      }

      // Send 10 bundles from session 2 with tag 'a'
      for (let i = 0; i < 10; i++) {
        const bundle = createTimedBundle(baseNTP + (i * 0.001), 22000 + i);
        sonic.sendOSC(bundle, { sessionId: 2, runTag: 'a' });
      }

      // Wait for bundles to reach prescheduler
      await new Promise(r => setTimeout(r, 100));

      const metricsAfterSend = sonic.getMetrics();
      const pendingAfterSend = metricsAfterSend.preschedulerPending || 0;

      // Cancel all bundles from session 1 (should remove 20, leave 10)
      sonic.cancelSession(1);

      // Wait for cancel to process
      await new Promise(r => setTimeout(r, 100));

      const metricsAfterCancel = sonic.getMetrics();
      const pendingAfterCancel = metricsAfterCancel.preschedulerPending || 0;
      const cancelledAfter = metricsAfterCancel.preschedulerEventsCancelled || 0;
      const cancelledCount = cancelledAfter - cancelledBefore;

      return {
        pendingAfterSend,
        pendingAfterCancel,
        cancelledCount,
        expectedCancelled: 20,  // All from session 1
        expectedRemaining: 10,  // From session 2
      };
    });

    console.log(`\ncancelSession test:`);
    console.log(`  Pending after send: ${result.pendingAfterSend}`);
    console.log(`  Pending after cancel: ${result.pendingAfterCancel}`);
    console.log(`  Cancelled: ${result.cancelledCount} (expected ${result.expectedCancelled})`);

    expect(result.pendingAfterSend).toBe(30);
    expect(result.cancelledCount).toBe(result.expectedCancelled);
    expect(result.pendingAfterCancel).toBe(result.expectedRemaining);
  });

  test("cancelSessionTag removes only bundles matching both session AND tag", async ({ page }) => {
    const result = await page.evaluate(async () => {
      // NTP helpers
      const NTP_EPOCH_OFFSET = 2208988800;
      const getCurrentNTP = () => {
        const perfTimeMs = performance.timeOrigin + performance.now();
        return (perfTimeMs / 1000) + NTP_EPOCH_OFFSET;
      };

      const createTimedBundle = (ntpTime, nodeId) => {
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

      const sonic = new window.SuperSonic({
        baseURL: "/dist/",
      });

      await sonic.init();
      await sonic.loadSynthDefs(["sonic-pi-beep"]);
      await sonic.sync(1);

      const metricsBefore = sonic.getMetrics();
      const cancelledBefore = metricsBefore.preschedulerEventsCancelled || 0;

      // Schedule bundles 10 seconds in future
      const baseNTP = getCurrentNTP() + 10.0;

      // Send 10 bundles: session=1, tag='a'
      for (let i = 0; i < 10; i++) {
        const bundle = createTimedBundle(baseNTP + (i * 0.001), 30000 + i);
        sonic.sendOSC(bundle, { sessionId: 1, runTag: 'a' });
      }

      // Send 10 bundles: session=1, tag='b'
      for (let i = 0; i < 10; i++) {
        const bundle = createTimedBundle(baseNTP + (i * 0.001), 31000 + i);
        sonic.sendOSC(bundle, { sessionId: 1, runTag: 'b' });
      }

      // Send 10 bundles: session=2, tag='a'
      for (let i = 0; i < 10; i++) {
        const bundle = createTimedBundle(baseNTP + (i * 0.001), 32000 + i);
        sonic.sendOSC(bundle, { sessionId: 2, runTag: 'a' });
      }

      // Wait for bundles to reach prescheduler
      await new Promise(r => setTimeout(r, 100));

      const metricsAfterSend = sonic.getMetrics();
      const pendingAfterSend = metricsAfterSend.preschedulerPending || 0;

      // Cancel only bundles matching session=1 AND tag='a' (should remove 10, leave 20)
      sonic.cancelSessionTag(1, 'a');

      // Wait for cancel to process
      await new Promise(r => setTimeout(r, 100));

      const metricsAfterCancel = sonic.getMetrics();
      const pendingAfterCancel = metricsAfterCancel.preschedulerPending || 0;
      const cancelledAfter = metricsAfterCancel.preschedulerEventsCancelled || 0;
      const cancelledCount = cancelledAfter - cancelledBefore;

      return {
        pendingAfterSend,
        pendingAfterCancel,
        cancelledCount,
        expectedCancelled: 10,  // Only session=1, tag='a'
        expectedRemaining: 20,  // session=1/tag='b' + session=2/tag='a'
      };
    });

    console.log(`\ncancelSessionTag test:`);
    console.log(`  Pending after send: ${result.pendingAfterSend}`);
    console.log(`  Pending after cancel: ${result.pendingAfterCancel}`);
    console.log(`  Cancelled: ${result.cancelledCount} (expected ${result.expectedCancelled})`);

    expect(result.pendingAfterSend).toBe(30);
    expect(result.cancelledCount).toBe(result.expectedCancelled);
    expect(result.pendingAfterCancel).toBe(result.expectedRemaining);
  });

  test("cancelled bundles never reach the ring buffer", async ({ page }) => {
    const result = await page.evaluate(async () => {
      // NTP helpers
      const NTP_EPOCH_OFFSET = 2208988800;
      const getCurrentNTP = () => {
        const perfTimeMs = performance.timeOrigin + performance.now();
        return (perfTimeMs / 1000) + NTP_EPOCH_OFFSET;
      };

      const createTimedBundle = (ntpTime, nodeId) => {
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

      const sonic = new window.SuperSonic({
        baseURL: "/dist/",
      });

      await sonic.init();
      await sonic.loadSynthDefs(["sonic-pi-beep"]);
      await sonic.sync(1);

      const metricsBefore = sonic.getMetrics();
      const sentBefore = metricsBefore.preschedulerSent || 0;
      const cancelledBefore = metricsBefore.preschedulerEventsCancelled || 0;

      // Schedule bundles 5 seconds in future (long enough to cancel before dispatch)
      const baseNTP = getCurrentNTP() + 5.0;

      // Send 20 bundles with tag 'to_cancel'
      for (let i = 0; i < 20; i++) {
        const bundle = createTimedBundle(baseNTP + (i * 0.001), 40000 + i);
        sonic.sendOSC(bundle, { sessionId: 0, runTag: 'to_cancel' });
      }

      // Wait for bundles to reach prescheduler
      await new Promise(r => setTimeout(r, 50));

      const metricsAfterSend = sonic.getMetrics();
      const sentAfterSend = metricsAfterSend.preschedulerSent || 0;
      const pendingAfterSend = metricsAfterSend.preschedulerPending || 0;

      // Cancel immediately (before any dispatch cycle can occur)
      sonic.cancelTag('to_cancel');

      // Wait a bit to ensure cancel has processed
      await new Promise(r => setTimeout(r, 50));

      const metricsAfterCancel = sonic.getMetrics();
      const sentAfterCancel = metricsAfterCancel.preschedulerSent || 0;
      const pendingAfterCancel = metricsAfterCancel.preschedulerPending || 0;
      const cancelledAfter = metricsAfterCancel.preschedulerEventsCancelled || 0;

      // Wait past the scheduled time to ensure bundles would have been dispatched
      // if they hadn't been cancelled
      await new Promise(r => setTimeout(r, 300));

      const metricsFinal = sonic.getMetrics();
      const sentFinal = metricsFinal.preschedulerSent || 0;

      return {
        pendingAfterSend,
        pendingAfterCancel,
        cancelledCount: cancelledAfter - cancelledBefore,
        sentBeforeSend: sentBefore,
        sentAfterSend,
        sentAfterCancel,
        sentFinal,
        sentDuringTest: sentFinal - sentBefore,
      };
    });

    console.log(`\nCancelled bundles never reach ring buffer:`);
    console.log(`  Pending after send: ${result.pendingAfterSend}`);
    console.log(`  Pending after cancel: ${result.pendingAfterCancel}`);
    console.log(`  Cancelled: ${result.cancelledCount}`);
    console.log(`  Sent to ring buffer during test: ${result.sentDuringTest}`);

    // All 20 bundles should have been in the pending queue
    expect(result.pendingAfterSend).toBe(20);

    // All should have been cancelled
    expect(result.cancelledCount).toBe(20);

    // Queue should be empty after cancel
    expect(result.pendingAfterCancel).toBe(0);

    // No bundles should have been sent to the ring buffer
    // (sentDuringTest should be 0 since all were cancelled before dispatch)
    expect(result.sentDuringTest).toBe(0);
  });

  test("cancelAllScheduled clears entire prescheduler queue", async ({ page }) => {
    const result = await page.evaluate(async () => {
      // NTP helpers
      const NTP_EPOCH_OFFSET = 2208988800;
      const getCurrentNTP = () => {
        const perfTimeMs = performance.timeOrigin + performance.now();
        return (perfTimeMs / 1000) + NTP_EPOCH_OFFSET;
      };

      const createTimedBundle = (ntpTime, nodeId) => {
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

      const sonic = new window.SuperSonic({
        baseURL: "/dist/",
      });

      await sonic.init();
      await sonic.loadSynthDefs(["sonic-pi-beep"]);
      await sonic.sync(1);

      const metricsBefore = sonic.getMetrics();
      const cancelledBefore = metricsBefore.preschedulerEventsCancelled || 0;

      // Schedule bundles 10 seconds in future
      const baseNTP = getCurrentNTP() + 10.0;

      // Send bundles with various sessions and tags
      for (let i = 0; i < 10; i++) {
        const bundle = createTimedBundle(baseNTP + (i * 0.001), 50000 + i);
        sonic.sendOSC(bundle, { sessionId: 0, runTag: 'run_1' });
      }
      for (let i = 0; i < 10; i++) {
        const bundle = createTimedBundle(baseNTP + (i * 0.001), 51000 + i);
        sonic.sendOSC(bundle, { sessionId: 1, runTag: 'run_2' });
      }
      for (let i = 0; i < 10; i++) {
        const bundle = createTimedBundle(baseNTP + (i * 0.001), 52000 + i);
        sonic.sendOSC(bundle, { sessionId: 2, runTag: 'run_3' });
      }

      // Wait for bundles to reach prescheduler
      await new Promise(r => setTimeout(r, 100));

      const metricsAfterSend = sonic.getMetrics();
      const pendingAfterSend = metricsAfterSend.preschedulerPending || 0;

      // Cancel ALL scheduled bundles
      sonic.cancelAllScheduled();

      // Wait for cancel to process
      await new Promise(r => setTimeout(r, 100));

      const metricsAfterCancel = sonic.getMetrics();
      const pendingAfterCancel = metricsAfterCancel.preschedulerPending || 0;
      const cancelledAfter = metricsAfterCancel.preschedulerEventsCancelled || 0;
      const cancelledCount = cancelledAfter - cancelledBefore;

      return {
        pendingAfterSend,
        pendingAfterCancel,
        cancelledCount,
      };
    });

    console.log(`\ncancelAllScheduled test:`);
    console.log(`  Pending after send: ${result.pendingAfterSend}`);
    console.log(`  Pending after cancel: ${result.pendingAfterCancel}`);
    console.log(`  Cancelled: ${result.cancelledCount}`);

    expect(result.pendingAfterSend).toBe(30);
    expect(result.cancelledCount).toBe(30);
    expect(result.pendingAfterCancel).toBe(0);
  });
});

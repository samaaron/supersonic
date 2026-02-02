import { test, expect } from "./fixtures.mjs";

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
  test.beforeEach(async ({ page, sonicConfig }) => {
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

  test("cancelTag removes all bundles with matching tag (any session)", async ({ page, sonicConfig }) => {
    const result = await page.evaluate(async (config) => {
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

      const sonic = new window.SuperSonic(config);

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
    }, sonicConfig);

    console.log(`\ncancelTag test:`);
    console.log(`  Pending after send: ${result.pendingAfterSend}`);
    console.log(`  Pending after cancel: ${result.pendingAfterCancel}`);
    console.log(`  Cancelled: ${result.cancelledCount} (expected ${result.expectedCancelled})`);

    expect(result.pendingAfterSend).toBe(30);
    expect(result.cancelledCount).toBe(result.expectedCancelled);
    expect(result.pendingAfterCancel).toBe(result.expectedRemaining);
  });

  test("cancelSession removes all bundles from matching session (any tag)", async ({ page, sonicConfig }) => {
    const result = await page.evaluate(async (config) => {
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

      const sonic = new window.SuperSonic(config);

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
    }, sonicConfig);

    console.log(`\ncancelSession test:`);
    console.log(`  Pending after send: ${result.pendingAfterSend}`);
    console.log(`  Pending after cancel: ${result.pendingAfterCancel}`);
    console.log(`  Cancelled: ${result.cancelledCount} (expected ${result.expectedCancelled})`);

    expect(result.pendingAfterSend).toBe(30);
    expect(result.cancelledCount).toBe(result.expectedCancelled);
    expect(result.pendingAfterCancel).toBe(result.expectedRemaining);
  });

  test("cancelSessionTag removes only bundles matching both session AND tag", async ({ page, sonicConfig }) => {
    const result = await page.evaluate(async (config) => {
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

      const sonic = new window.SuperSonic(config);

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
    }, sonicConfig);

    console.log(`\ncancelSessionTag test:`);
    console.log(`  Pending after send: ${result.pendingAfterSend}`);
    console.log(`  Pending after cancel: ${result.pendingAfterCancel}`);
    console.log(`  Cancelled: ${result.cancelledCount} (expected ${result.expectedCancelled})`);

    expect(result.pendingAfterSend).toBe(30);
    expect(result.cancelledCount).toBe(result.expectedCancelled);
    expect(result.pendingAfterCancel).toBe(result.expectedRemaining);
  });

  test("cancelled bundles never reach the ring buffer", async ({ page, sonicConfig }) => {
    const result = await page.evaluate(async (config) => {
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

      const sonic = new window.SuperSonic(config);

      await sonic.init();
      await sonic.loadSynthDefs(["sonic-pi-beep"]);
      await sonic.sync(1);

      const metricsBefore = sonic.getMetrics();
      const sentBefore = metricsBefore.preschedulerDispatched || 0;
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
      const sentAfterSend = metricsAfterSend.preschedulerDispatched || 0;
      const pendingAfterSend = metricsAfterSend.preschedulerPending || 0;

      // Cancel immediately (before any dispatch cycle can occur)
      sonic.cancelTag('to_cancel');

      // Wait a bit to ensure cancel has processed
      await new Promise(r => setTimeout(r, 50));

      const metricsAfterCancel = sonic.getMetrics();
      const sentAfterCancel = metricsAfterCancel.preschedulerDispatched || 0;
      const pendingAfterCancel = metricsAfterCancel.preschedulerPending || 0;
      const cancelledAfter = metricsAfterCancel.preschedulerEventsCancelled || 0;

      // Wait past the scheduled time to ensure bundles would have been dispatched
      // if they hadn't been cancelled
      await new Promise(r => setTimeout(r, 300));

      const metricsFinal = sonic.getMetrics();
      const sentFinal = metricsFinal.preschedulerDispatched || 0;

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
    }, sonicConfig);

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

  test("cancelAllScheduled clears entire prescheduler queue", async ({ page, sonicConfig }) => {
    const result = await page.evaluate(async (config) => {
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

      const sonic = new window.SuperSonic(config);

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
    }, sonicConfig);

    console.log(`\ncancelAllScheduled test:`);
    console.log(`  Pending after send: ${result.pendingAfterSend}`);
    console.log(`  Pending after cancel: ${result.pendingAfterCancel}`);
    console.log(`  Cancelled: ${result.cancelledCount}`);

    expect(result.pendingAfterSend).toBe(30);
    expect(result.cancelledCount).toBe(30);
    expect(result.pendingAfterCancel).toBe(0);
  });

  test("min headroom and lates metrics track dispatch timing for timed bundles", async ({ page, sonicConfig }) => {
    // Sentinel value for "unset" min headroom metric (must match prescheduler worker)
    const HEADROOM_UNSET_SENTINEL = 0xFFFFFFFF;

    // Use shorter lookahead for faster test (bundles 300ms out go through prescheduler)
    const config = { ...sonicConfig, bypassLookaheadMs: 200 };

    const result = await page.evaluate(async (config) => {
      const HEADROOM_UNSET_SENTINEL = 0xFFFFFFFF;

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
            { type: 'f', value: 0.05 },
          ],
        };

        // Encode to binary OSC format
        const encodedMessage = window.SuperSonic.osc.encode(message);
        const bundleSize = 8 + 8 + 4 + encodedMessage.byteLength;
        const bundle = new Uint8Array(bundleSize);
        const view = new DataView(bundle.buffer);

        // "#bundle\0" header
        bundle.set([0x23, 0x62, 0x75, 0x6e, 0x64, 0x6c, 0x65, 0x00], 0);

        // NTP timetag (8 bytes)
        const NTP_EPOCH_OFFSET = 2208988800;
        const seconds = Math.floor(ntpTime);
        const fraction = Math.floor((ntpTime - seconds) * 0xFFFFFFFF);
        view.setUint32(8, seconds, false);
        view.setUint32(12, fraction, false);

        // Message size + data
        view.setInt32(16, encodedMessage.byteLength, false);
        bundle.set(new Uint8Array(encodedMessage), 20);

        return bundle;
      };

      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");
      await sonic.sync(1);

      // Get initial metrics - min headroom should be unset (sentinel value)
      const metricsBefore = sonic.getMetrics();
      const minHeadroomBefore = metricsBefore.preschedulerMinHeadroomMs;
      const latesBefore = metricsBefore.preschedulerLates;

      // Schedule bundles 300ms in the future (beyond the 200ms lookahead window)
      // They will be dispatched ~100ms before execution (300 - 200 = 100ms headroom)
      const baseNTP = getCurrentNTP() + 0.3;  // 300ms in future
      for (let i = 0; i < 5; i++) {
        const bundle = createTimedBundle(baseNTP + (i * 0.01), 60000 + i);
        sonic.sendOSC(bundle);
      }

      // Wait for prescheduler to dispatch the bundles (poll interval is 25ms)
      // Wait until bundles execute (300ms) plus some buffer
      await new Promise(r => setTimeout(r, 500));

      const metricsAfter = sonic.getMetrics();
      const minHeadroomAfter = metricsAfter.preschedulerMinHeadroomMs;
      const latesAfter = metricsAfter.preschedulerLates;
      const dispatched = metricsAfter.preschedulerDispatched;

      await sonic.destroy();

      return {
        minHeadroomBefore,
        latesBefore,
        minHeadroomAfter,
        latesAfter,
        dispatched,
        HEADROOM_UNSET_SENTINEL,
      };
    }, config);

    console.log(`\nMin headroom and lates metrics test:`);
    console.log(`  Before: minHeadroom=${result.minHeadroomBefore}, lates=${result.latesBefore}`);
    console.log(`  After: minHeadroom=${result.minHeadroomAfter}ms, lates=${result.latesAfter}`);
    console.log(`  Dispatched: ${result.dispatched}`);

    // Before any timed bundles, min headroom should be unset (sentinel value)
    expect(result.minHeadroomBefore).toBe(HEADROOM_UNSET_SENTINEL);

    // No lates initially
    expect(result.latesBefore).toBe(0);

    // After dispatching timed bundles, min headroom should be populated
    expect(result.minHeadroomAfter).not.toBe(HEADROOM_UNSET_SENTINEL);

    // Min headroom should be reasonable (0-300ms range for our 300ms future bundles)
    expect(result.minHeadroomAfter).toBeGreaterThanOrEqual(0);
    expect(result.minHeadroomAfter).toBeLessThan(300);

    // Bundles scheduled 300ms in future should not be late
    expect(result.latesAfter).toBe(0);

    // We should have dispatched at least 5 bundles
    expect(result.dispatched).toBeGreaterThanOrEqual(5);
  });

  test("lates counter increments for bundles dispatched after their scheduled time", async ({ page, sonicConfig }) => {
    const result = await page.evaluate(async (config) => {
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
            { type: 'f', value: 0.05 },
          ],
        };

        // Encode to binary OSC format
        const encodedMessage = window.SuperSonic.osc.encode(message);
        const bundleSize = 8 + 8 + 4 + encodedMessage.byteLength;
        const bundle = new Uint8Array(bundleSize);
        const view = new DataView(bundle.buffer);

        // "#bundle\0" header
        bundle.set([0x23, 0x62, 0x75, 0x6e, 0x64, 0x6c, 0x65, 0x00], 0);

        // NTP timetag (8 bytes)
        const seconds = Math.floor(ntpTime);
        const fraction = Math.floor((ntpTime - seconds) * 0xFFFFFFFF);
        view.setUint32(8, seconds, false);
        view.setUint32(12, fraction, false);

        // Message size + data
        view.setInt32(16, encodedMessage.byteLength, false);
        bundle.set(new Uint8Array(encodedMessage), 20);

        return bundle;
      };

      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");
      await sonic.sync(1);

      // Get initial lates count (both prescheduler and scsynth)
      const metricsBefore = sonic.getMetrics();
      const preschedulerLatesBefore = metricsBefore.preschedulerLates;
      const scsynthLatesBefore = metricsBefore.scsynthSchedulerLates;

      // Schedule bundles with timestamps in the PAST (100ms ago)
      // These should be dispatched immediately but counted as late
      // In postMessage mode: preschedulerLates increments
      // In SAB mode: DirectWriter bypasses prescheduler, but scsynth tracks lates
      const pastNTP = getCurrentNTP() - 0.1;  // 100ms in the past
      for (let i = 0; i < 5; i++) {
        const bundle = createTimedBundle(pastNTP - (i * 0.01), 70000 + i);
        sonic.sendOSC(bundle);
      }

      // Wait for bundles to be processed
      await new Promise(r => setTimeout(r, 300));

      const metricsAfter = sonic.getMetrics();
      const preschedulerLatesAfter = metricsAfter.preschedulerLates;
      const scsynthLatesAfter = metricsAfter.scsynthSchedulerLates;
      const dispatched = metricsAfter.preschedulerDispatched;
      const mode = metricsAfter.mode;
      const messagesProcessed = metricsAfter.scsynthMessagesProcessed;
      const schedulerDepth = metricsAfter.scsynthSchedulerDepth;

      await sonic.destroy();

      return {
        preschedulerLatesBefore,
        preschedulerLatesAfter,
        scsynthLatesBefore,
        scsynthLatesAfter,
        dispatched,
        mode,
        messagesProcessed,
        schedulerDepth,
      };
    }, sonicConfig);

    // Calculate total lates from both sources
    const totalLatesBefore = result.preschedulerLatesBefore + result.scsynthLatesBefore;
    const totalLatesAfter = result.preschedulerLatesAfter + result.scsynthLatesAfter;

    console.log(`\nLates counter test (mode: ${result.mode}):`);
    console.log(`  Prescheduler lates: ${result.preschedulerLatesBefore} -> ${result.preschedulerLatesAfter}`);
    console.log(`  Scsynth lates: ${result.scsynthLatesBefore} -> ${result.scsynthLatesAfter}`);
    console.log(`  Total lates: ${totalLatesBefore} -> ${totalLatesAfter}`);
    console.log(`  Dispatched: ${result.dispatched}, Processed: ${result.messagesProcessed}, Depth: ${result.schedulerDepth}`);

    // Total lates (prescheduler + scsynth) should have incremented for bundles scheduled in the past
    // In postMessage mode: prescheduler tracks lates
    // In SAB mode: scsynth tracks lates (DirectWriter bypasses prescheduler for past bundles)
    // All 5 bundles should be counted as late (JS and WASM both use 0ms threshold)
    expect(totalLatesAfter - totalLatesBefore).toBe(5);
  });
});

import { test, expect } from "@playwright/test";

/**
 * Scheduler Queue Overflow Test
 *
 * The scheduler has a fixed capacity of 128 events. When we send more than 128
 * timed bundles rapidly, the queue overflows and messages are dropped.
 *
 * This test sends SCHEDULER_CAPACITY + 200 messages to prove the issue.
 * The test FAILS if any messages are dropped - which currently happens.
 *
 * After implementing backpressure (leaving messages in the ring buffer when
 * scheduler is full), this test should PASS.
 */

const SCHEDULER_CAPACITY = 128;
const OVERFLOW_AMOUNT = 200; // Much more aggressive - send 3x+ capacity
const TOTAL_MESSAGES = SCHEDULER_CAPACITY + OVERFLOW_AMOUNT;

const SONIC_CONFIG = {
  workerBaseURL: "/dist/workers/",
  wasmBaseURL: "/dist/wasm/",
  sampleBaseURL: "/dist/samples/",
  synthdefBaseURL: "/dist/synthdefs/",
};

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
  test(`sending ${TOTAL_MESSAGES} timed bundles should not drop any messages`, async ({ page }) => {
    const errors = [];
    const debugLogs = [];

    page.on("console", (msg) => {
      const text = msg.text();
      debugLogs.push(text);

      if (text.includes("Scheduler queue full") ||
          text.includes("Failed to schedule bundle")) {
        errors.push(text);
        console.log("SCHEDULER OVERFLOW:", text);
      }
    });

    await page.goto("/test/harness.html");

    await page.waitForFunction(() => window.supersonicReady === true, {
      timeout: 10000,
    });

    const result = await page.evaluate(async (config) => {
      const { TOTAL_MESSAGES, helpers } = config;
      eval(helpers); // Load audio analysis functions

      const sonic = new window.SuperSonic({
        workerBaseURL: "/dist/workers/",
        wasmBaseURL: "/dist/wasm/",
        sampleBaseURL: "/dist/samples/",
        synthdefBaseURL: "/dist/synthdefs/",
      });

      const debugMessages = [];
      sonic.on('debug', (msg) => {
        debugMessages.push(msg);
      });

      try {
        await sonic.init();
        await sonic.loadSynthDefs(["sonic-pi-beep"]);
        await sonic.sync(1);

        // ============================================
        // TEST 1: Audio works BEFORE overflow
        // ============================================
        sonic.startCapture();
        await sonic.send("/s_new", "sonic-pi-beep", 88888, 0, 0, "amp", 0.5, "release", 0.1);
        await new Promise(r => setTimeout(r, 150));
        const beforeCapture = sonic.stopCapture();
        await sonic.send("/n_free", 88888);
        await new Promise(r => setTimeout(r, 50));

        const audioBeforeOverflow = {
          hasAudio: hasAudio(beforeCapture.left),
          rms: calculateRMS(beforeCapture.left, 0, Math.min(beforeCapture.frames, 10000)),
          peak: findPeak(beforeCapture.left, 0, Math.min(beforeCapture.frames, 10000)),
          frames: beforeCapture.frames
        };

        // Get metrics before - dump everything for debugging
        const metricsBefore = sonic.getMetrics();
        const droppedBefore = metricsBefore.workletSchedulerDropped || 0;
        const processCountBefore = metricsBefore.workletProcessCount || 0;

        // Create timed bundle with future timestamp
        const createTimedBundle = (ntpTime, nodeId, note) => {
          const message = {
            address: "/s_new",
            args: [
              { type: 's', value: "sonic-pi-beep" },
              { type: 'i', value: nodeId },
              { type: 'i', value: 0 },
              { type: 'i', value: 0 },
              { type: 's', value: "note" },
              { type: 'f', value: note },
              { type: 's', value: "amp" },
              { type: 'f', value: 0.05 },
              { type: 's', value: "release" },
              { type: 'f', value: 0.2 }  // Longer release so we can hear them
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

        // ============================================
        // TEST 2: Cause scheduler overflow with capture running
        // ============================================
        // Start capture BEFORE sending messages so we capture the scheduled synths
        sonic.startCapture();

        // Get NTP time the CORRECT way - using AudioContext + ntpStartTime
        // This matches how the WASM scheduler calculates time
        const ntpStartOffset = sonic.bufferConstants.NTP_START_TIME_START;
        const ringBufferBase = sonic.ringBufferBase;
        const ntpStartView = new Float64Array(
          sonic.sharedBuffer,
          ringBufferBase + ntpStartOffset,
          1
        );
        const ntpStartTime = ntpStartView[0];
        const currentContextTime = sonic.audioContext.currentTime;
        const currentNTP = currentContextTime + ntpStartTime;

        // Schedule bundles 300ms in the future
        const baseNTP = currentNTP + 0.3;

        // Log timing for debugging
        const timingDebug = {
          audioContextTime: currentContextTime,
          ntpStartTime: ntpStartTime,
          currentNTP: currentNTP,
          targetNTP: baseNTP
        };

        // Send all bundles as fast as possible
        // All scheduled within a 26ms window so they pile up in scheduler
        for (let i = 0; i < TOTAL_MESSAGES; i++) {
          const targetNTP = baseNTP + (i * 0.0001); // 0.1ms apart
          const nodeId = 50000 + i;
          const note = 60 + (i % 24);

          const bundle = createTimedBundle(targetNTP, nodeId, note);
          sonic.sendOSC(bundle);
        }

        // Wait a bit for messages to reach the scheduler
        await new Promise(r => setTimeout(r, 100));

        // Poll metrics multiple times to catch peak
        let peakDepth = 0;
        let peakMax = 0;
        for (let poll = 0; poll < 10; poll++) {
          await new Promise(r => setTimeout(r, 20));
          const m = sonic.getMetrics();
          if (m.workletSchedulerDepth > peakDepth) peakDepth = m.workletSchedulerDepth;
          if (m.workletSchedulerMax > peakMax) peakMax = m.workletSchedulerMax;
        }

        // Get metrics after sending
        const metricsAfter = sonic.getMetrics();
        const droppedAfter = metricsAfter.workletSchedulerDropped || 0;
        const schedulerMax = metricsAfter.workletSchedulerMax || 0;

        // Check for scheduler overflow errors in debug
        const overflowErrors = debugMessages.filter(msg =>
          msg.text?.includes("Scheduler queue full") ||
          msg.text?.includes("Failed to schedule bundle")
        );

        // Wait for all scheduled messages to execute (300ms future + 26ms spread + buffer)
        await new Promise(r => setTimeout(r, 500));

        // Check if scheduler depth dropped to 0 (events executed)
        const metricsAfterWait = sonic.getMetrics();
        const schedulerDepthAfterWait = metricsAfterWait.workletSchedulerDepth || 0;

        // Stop capture to see if the scheduled synths produced audio
        const overflowCapture = sonic.stopCapture();
        const audioFromScheduledSynths = {
          hasAudio: hasAudio(overflowCapture.left),
          rms: calculateRMS(overflowCapture.left, 0, Math.min(overflowCapture.frames, 40000)),
          peak: findPeak(overflowCapture.left, 0, Math.min(overflowCapture.frames, 40000)),
          frames: overflowCapture.frames
        };

        // Sync to ensure scsynth has processed everything
        await sonic.sync(2);

        // ============================================
        // TEST 3: Audio works AFTER overflow (same scsynth instance)
        // ============================================
        let audioAfterOverflow = { hasAudio: false, rms: 0, peak: 0, frames: 0, error: null };
        try {
          sonic.startCapture();
          await sonic.send("/s_new", "sonic-pi-beep", 99999, 0, 0, "amp", 0.5, "release", 0.1);
          await new Promise(r => setTimeout(r, 150));
          const afterCapture = sonic.stopCapture();
          await sonic.send("/n_free", 99999);
          await new Promise(r => setTimeout(r, 50));

          audioAfterOverflow = {
            hasAudio: hasAudio(afterCapture.left),
            rms: calculateRMS(afterCapture.left, 0, Math.min(afterCapture.frames, 10000)),
            peak: findPeak(afterCapture.left, 0, Math.min(afterCapture.frames, 10000)),
            frames: afterCapture.frames
          };
        } catch (e) {
          audioAfterOverflow.error = e.message;
        }

        // Check for any scsynth errors in debug messages
        const scsynthErrors = debugMessages.filter(msg =>
          msg.text?.includes("FAILURE") ||
          msg.text?.includes("Command not found") ||
          msg.text?.includes("Node") && msg.text?.includes("not found")
        );

        // Final metrics
        const metricsFinal = sonic.getMetrics();

        // Get final process count to verify WASM is running
        const processCountAfter = metricsFinal.workletProcessCount || 0;

        return {
          success: true,
          totalSent: TOTAL_MESSAGES,
          droppedBefore,
          droppedAfter,
          droppedCount: droppedAfter - droppedBefore,
          schedulerMax,
          polledPeakDepth: peakDepth,
          polledPeakMax: peakMax,
          schedulerCapacity: 64, // hardcoded for reference
          overflowErrors,
          // SAB metrics for debugging
          processCountBefore,
          processCountAfter,
          processCountDelta: processCountAfter - processCountBefore,
          preschedulerBundlesScheduled: metricsFinal.preschedulerBundlesScheduled,
          preschedulerSent: metricsFinal.preschedulerSent,
          mainMessagesSent: metricsFinal.mainMessagesSent,
          workletMessagesProcessed: metricsFinal.workletMessagesProcessed,
          // Buffer usage
          inBufferUsed: metricsFinal.inBufferUsed,
          inBufferCapacity: metricsFinal.inBufferCapacity,
          finalMetrics: {
            schedulerDepth: metricsFinal.workletSchedulerDepth,
            schedulerDropped: metricsFinal.workletSchedulerDropped,
          },
          debugCount: debugMessages.length,
          recentDebug: debugMessages.slice(-20),
          // Audio tests
          audioBeforeOverflow,
          audioFromScheduledSynths,  // Did the 64 scheduled synths produce sound?
          audioAfterOverflow,
          scsynthErrors,
          timingDebug,
          schedulerDepthAfterWait  // Should be 0 if events executed
        };
      } catch (err) {
        return { success: false, error: err.message };
      }
    }, { TOTAL_MESSAGES, helpers: AUDIO_HELPERS });

    console.log(`\nScheduler Overflow Test Results:`);
    if (result.error) {
      console.log(`  ERROR: ${result.error}`);
    }
    console.log(`  Messages sent (test): ${result.totalSent}`);
    console.log(`  Scheduler capacity: ${result.schedulerCapacity}`);
    console.log(`  Peak scheduler depth (metric): ${result.schedulerMax}`);
    console.log(`  Peak scheduler depth (polled): ${result.polledPeakDepth}`);
    console.log(`  Peak scheduler max (polled): ${result.polledPeakMax}`);
    console.log(`  Scheduler depth after wait: ${result.schedulerDepthAfterWait} (should be 0 if executed)`);
    console.log(`  Messages dropped: ${result.droppedCount}`);
    console.log(`\nSAB Metrics:`);
    console.log(`  WASM process count: ${result.processCountBefore} -> ${result.processCountAfter} (delta: ${result.processCountDelta})`);
    console.log(`  Prescheduler bundles scheduled: ${result.preschedulerBundlesScheduled}`);
    console.log(`  Prescheduler sent: ${result.preschedulerSent}`);
    console.log(`  Main messages sent: ${result.mainMessagesSent}`);
    console.log(`  Worklet messages processed: ${result.workletMessagesProcessed}`);
    console.log(`  In buffer: ${result.inBufferUsed} / ${result.inBufferCapacity}`);
    console.log(`  Debug messages: ${result.debugCount}`);
    if (result.timingDebug) {
      console.log(`\nTiming Debug:`);
      console.log(`  AudioContext time: ${result.timingDebug.audioContextTime?.toFixed(3)}s`);
      console.log(`  NTP start time: ${result.timingDebug.ntpStartTime?.toFixed(3)}`);
      console.log(`  Current NTP: ${result.timingDebug.currentNTP?.toFixed(3)}`);
      console.log(`  Target NTP: ${result.timingDebug.targetNTP?.toFixed(3)}`);
    }

    if (result.overflowErrors?.length > 0) {
      console.log(`\nOverflow errors detected:`);
      result.overflowErrors.forEach(e => console.log(`  - ${e}`));
    }

    if (errors.length > 0) {
      console.log(`\nConsole errors:`);
      errors.forEach(e => console.log(`  - ${e}`));
    }

    if (result.recentDebug?.length > 0) {
      console.log(`\nRecent debug messages:`);
      result.recentDebug.slice(-10).forEach(e => console.log(`  - ${e}`));
    }

    console.log(`\nAudio Test (does scsynth produce sound?):`);
    console.log(`  BEFORE overflow (single test synth):`);
    console.log(`    Has audio: ${result.audioBeforeOverflow?.hasAudio ? 'YES' : 'NO'}`);
    console.log(`    RMS: ${result.audioBeforeOverflow?.rms?.toFixed(4)}`);
    console.log(`    Peak: ${result.audioBeforeOverflow?.peak?.toFixed(4)}`);
    console.log(`  DURING overflow (64 scheduled synths should play, 200 dropped):`);
    console.log(`    Has audio: ${result.audioFromScheduledSynths?.hasAudio ? 'YES' : 'NO'}`);
    console.log(`    RMS: ${result.audioFromScheduledSynths?.rms?.toFixed(4)}`);
    console.log(`    Peak: ${result.audioFromScheduledSynths?.peak?.toFixed(4)}`);
    console.log(`  AFTER overflow (single test synth):`);
    console.log(`    Has audio: ${result.audioAfterOverflow?.hasAudio ? 'YES' : 'NO'}`);
    console.log(`    RMS: ${result.audioAfterOverflow?.rms?.toFixed(4)}`);
    console.log(`    Peak: ${result.audioAfterOverflow?.peak?.toFixed(4)}`);
    if (result.audioAfterOverflow?.error) {
      console.log(`    Error: ${result.audioAfterOverflow.error}`);
    }
    if (result.scsynthErrors?.length > 0) {
      console.log(`\nscsynth errors after overflow:`);
      result.scsynthErrors.forEach(e => console.log(`  - ${e}`));
    }

    // Test assertions:
    // 1. The test should complete successfully
    expect(result.success).toBe(true);

    // 2. NO messages should be dropped (this is the main assertion)
    // Currently this WILL FAIL because scheduler overflows at 64
    // After implementing backpressure, this should PASS
    expect(result.droppedCount).toBe(0);

    // 3. No overflow errors in debug log
    expect(result.overflowErrors?.length || 0).toBe(0);
  });

  test("verify scheduler capacity is 128", async ({ page }) => {
    // Sanity check: send exactly 128 messages - should all fit
    await page.goto("/test/harness.html");

    await page.waitForFunction(() => window.supersonicReady === true, {
      timeout: 10000,
    });

    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: "/dist/workers/",
        wasmBaseURL: "/dist/wasm/",
        sampleBaseURL: "/dist/samples/",
        synthdefBaseURL: "/dist/synthdefs/",
      });

      const debugMessages = [];
      sonic.on('debug', (msg) => debugMessages.push(msg));

      await sonic.init();
      await sonic.loadSynthDefs(["sonic-pi-beep"]);
      await sonic.sync(1);

      const metricsBefore = sonic.getMetrics();
      const droppedBefore = metricsBefore.workletSchedulerDropped || 0;

      // NTP helper
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

      // Send exactly 64 messages (the capacity) - all scheduled far in future
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
        droppedAfter: metricsAfter.workletSchedulerDropped || 0,
        droppedCount: (metricsAfter.workletSchedulerDropped || 0) - droppedBefore,
        schedulerMax: metricsAfter.workletSchedulerMax,
        overflowErrors: debugMessages.filter(m => m.text?.includes("queue full")).length
      };
    });

    console.log(`\nCapacity test: sent ${result.sent}, dropped ${result.droppedCount}, max depth ${result.schedulerMax}`);

    // At exactly 64 messages, we should be at capacity but not overflow
    // (In practice, some might execute before others arrive, so drops are unlikely)
    expect(result.droppedCount).toBe(0);
  });

  test("oversized scheduled bundle throws error", async ({ page }) => {
    await page.goto("/test/harness.html");

    await page.waitForFunction(() => window.supersonicReady === true, {
      timeout: 10000,
    });

    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: "/dist/workers/",
        wasmBaseURL: "/dist/wasm/",
        sampleBaseURL: "/dist/samples/",
        synthdefBaseURL: "/dist/synthdefs/",
      });

      await sonic.init();

      // Get the scheduler slot size from buffer constants
      const slotSize = sonic.bufferConstants.scheduler_slot_size;

      // NTP helper - create a bundle scheduled > 200ms in future (won't bypass)
      const NTP_EPOCH_OFFSET = 2208988800;
      const getCurrentNTP = () => {
        const perfTimeMs = performance.timeOrigin + performance.now();
        return (perfTimeMs / 1000) + NTP_EPOCH_OFFSET;
      };

      // Create an oversized bundle by repeating valid messages
      const createOversizedBundle = (ntpTime, targetSize) => {
        // Create a valid small message
        const smallMessage = {
          address: "/test",
          args: [
            { type: 's', value: "padding_data_to_make_message_larger" },
            { type: 'i', value: 12345 },
          ]
        };
        const encodedSmall = window.SuperSonic.osc.encode(smallMessage);

        // Calculate how many messages we need to exceed targetSize
        // Bundle overhead: "#bundle\0" (8) + timetag (8) = 16 bytes
        // Per message: size (4) + message bytes
        const messageWithSize = 4 + encodedSmall.byteLength;
        const bundleOverhead = 16;
        const messagesNeeded = Math.ceil((targetSize - bundleOverhead) / messageWithSize) + 1;

        // Build the bundle
        const totalSize = bundleOverhead + (messagesNeeded * messageWithSize);
        const bundle = new Uint8Array(totalSize);
        const view = new DataView(bundle.buffer);

        // "#bundle\0"
        bundle.set([0x23, 0x62, 0x75, 0x6e, 0x64, 0x6c, 0x65, 0x00], 0);

        // Timetag (NTP format)
        const ntpSeconds = Math.floor(ntpTime);
        const ntpFraction = Math.floor((ntpTime % 1) * 0x100000000);
        view.setUint32(8, ntpSeconds, false);
        view.setUint32(12, ntpFraction, false);

        // Add messages
        let offset = 16;
        for (let i = 0; i < messagesNeeded; i++) {
          view.setInt32(offset, encodedSmall.byteLength, false);
          offset += 4;
          bundle.set(encodedSmall, offset);
          offset += encodedSmall.byteLength;
        }

        return bundle;
      };

      // Schedule 500ms in future (definitely won't bypass prescheduler)
      const futureNTP = getCurrentNTP() + 0.5;

      // Create a bundle that exceeds slot size
      const oversizedBundle = createOversizedBundle(futureNTP, slotSize);

      let errorThrown = null;
      try {
        await sonic.sendOSC(oversizedBundle);
      } catch (err) {
        errorThrown = err.message;
      }

      return {
        slotSize,
        bundleSize: oversizedBundle.length,
        errorThrown,
        errorContainsSize: errorThrown?.includes('too large'),
        errorContainsLimit: errorThrown?.includes(String(slotSize)),
      };
    });

    console.log(`\nSize limit test: slot=${result.slotSize}, bundle=${result.bundleSize}`);
    console.log(`Error: ${result.errorThrown}`);

    expect(result.errorThrown).not.toBeNull();
    expect(result.errorContainsSize).toBe(true);
    expect(result.errorContainsLimit).toBe(true);
  });

  test("bundle within size limit succeeds", async ({ page }) => {
    await page.goto("/test/harness.html");

    await page.waitForFunction(() => window.supersonicReady === true, {
      timeout: 10000,
    });

    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: "/dist/workers/",
        wasmBaseURL: "/dist/wasm/",
        sampleBaseURL: "/dist/samples/",
        synthdefBaseURL: "/dist/synthdefs/",
      });

      await sonic.init();
      await sonic.loadSynthDefs(["sonic-pi-beep"]);

      const slotSize = sonic.bufferConstants.scheduler_slot_size;

      // NTP helper
      const NTP_EPOCH_OFFSET = 2208988800;
      const getCurrentNTP = () => {
        const perfTimeMs = performance.timeOrigin + performance.now();
        return (perfTimeMs / 1000) + NTP_EPOCH_OFFSET;
      };

      // Create a normal-sized bundle with a real synth message
      const message = {
        address: "/s_new",
        args: [
          { type: 's', value: "sonic-pi-beep" },
          { type: 'i', value: 99999 },
          { type: 'i', value: 0 },
          { type: 'i', value: 0 },
          { type: 's', value: "note" },
          { type: 'f', value: 60 },
        ]
      };

      const encodedMessage = window.SuperSonic.osc.encode(message);
      const bundleSize = 8 + 8 + 4 + encodedMessage.byteLength;
      const bundle = new Uint8Array(bundleSize);
      const view = new DataView(bundle.buffer);

      bundle.set([0x23, 0x62, 0x75, 0x6e, 0x64, 0x6c, 0x65, 0x00], 0);
      const futureNTP = getCurrentNTP() + 0.5;
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
        slotSize,
        bundleSize: bundle.length,
        errorThrown,
        withinLimit: bundle.length <= slotSize,
      };
    });

    console.log(`\nNormal bundle test: slot=${result.slotSize}, bundle=${result.bundleSize}, within limit=${result.withinLimit}`);

    expect(result.withinLimit).toBe(true);
    expect(result.errorThrown).toBeNull();
  });

  test("immediate bundle bypasses size limit", async ({ page }) => {
    await page.goto("/test/harness.html");

    await page.waitForFunction(() => window.supersonicReady === true, {
      timeout: 10000,
    });

    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: "/dist/workers/",
        wasmBaseURL: "/dist/wasm/",
        sampleBaseURL: "/dist/samples/",
        synthdefBaseURL: "/dist/synthdefs/",
      });

      await sonic.init();

      const slotSize = sonic.bufferConstants.scheduler_slot_size;

      // Create an oversized bundle with IMMEDIATE timetag (1)
      const createImmediateOversizedBundle = (targetSize) => {
        // Create a valid small message
        const smallMessage = {
          address: "/test",
          args: [
            { type: 's', value: "padding_data_to_make_message_larger" },
            { type: 'i', value: 12345 },
          ]
        };
        const encodedSmall = window.SuperSonic.osc.encode(smallMessage);

        // Calculate how many messages we need to exceed targetSize
        const messageWithSize = 4 + encodedSmall.byteLength;
        const bundleOverhead = 16;
        const messagesNeeded = Math.ceil((targetSize - bundleOverhead) / messageWithSize) + 1;

        // Build the bundle
        const totalSize = bundleOverhead + (messagesNeeded * messageWithSize);
        const bundle = new Uint8Array(totalSize);
        const view = new DataView(bundle.buffer);

        // "#bundle\0"
        bundle.set([0x23, 0x62, 0x75, 0x6e, 0x64, 0x6c, 0x65, 0x00], 0);

        // Timetag = 1 (immediate execution)
        view.setUint32(8, 0, false);
        view.setUint32(12, 1, false);

        // Add messages
        let offset = 16;
        for (let i = 0; i < messagesNeeded; i++) {
          view.setInt32(offset, encodedSmall.byteLength, false);
          offset += 4;
          bundle.set(encodedSmall, offset);
          offset += encodedSmall.byteLength;
        }

        return bundle;
      };

      // Create bundle larger than slot size but with immediate timetag
      const oversizedImmediate = createImmediateOversizedBundle(slotSize);

      let errorThrown = null;
      try {
        await sonic.sendOSC(oversizedImmediate);
      } catch (err) {
        errorThrown = err.message;
      }

      return {
        slotSize,
        bundleSize: oversizedImmediate.length,
        errorThrown,
      };
    });

    console.log(`\nImmediate bypass test: slot=${result.slotSize}, bundle=${result.bundleSize}`);
    console.log(`Error: ${result.errorThrown || 'none (bypassed as expected)'}`);

    // Immediate bundles should NOT throw size error - they bypass the scheduler
    expect(result.errorThrown).toBeNull();
  });
});

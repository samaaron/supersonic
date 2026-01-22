import { test, expect } from "./fixtures.mjs";

/**
 * Buffer Peak Metrics Tests
 *
 * Verifies that ring buffer peak usage metrics are:
 * 1. Present in the metrics object
 * 2. Logically consistent (peak >= current, peak <= capacity)
 * 3. Updated correctly after buffer activity
 */

test.describe("Buffer Peak Metrics", () => {
  test("peak metrics are present and have correct structure", async ({ page, sonicConfig }) => {
    await page.goto("/test/harness.html");

    await page.waitForFunction(() => window.supersonicReady === true, {
      timeout: 10000,
    });

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();

      // Wait for some audio processing to happen
      await new Promise(r => setTimeout(r, 200));

      const metrics = sonic.getMetrics();

      return {
        success: true,
        inBufferUsed: {
          hasBytes: metrics.inBufferUsed?.bytes !== undefined,
          hasPercentage: metrics.inBufferUsed?.percentage !== undefined,
          hasPeakBytes: metrics.inBufferUsed?.peakBytes !== undefined,
          hasPeakPercentage: metrics.inBufferUsed?.peakPercentage !== undefined,
          hasCapacity: metrics.inBufferUsed?.capacity !== undefined,
          values: metrics.inBufferUsed,
        },
        outBufferUsed: {
          hasBytes: metrics.outBufferUsed?.bytes !== undefined,
          hasPercentage: metrics.outBufferUsed?.percentage !== undefined,
          hasPeakBytes: metrics.outBufferUsed?.peakBytes !== undefined,
          hasPeakPercentage: metrics.outBufferUsed?.peakPercentage !== undefined,
          hasCapacity: metrics.outBufferUsed?.capacity !== undefined,
          values: metrics.outBufferUsed,
        },
        debugBufferUsed: {
          hasBytes: metrics.debugBufferUsed?.bytes !== undefined,
          hasPercentage: metrics.debugBufferUsed?.percentage !== undefined,
          hasPeakBytes: metrics.debugBufferUsed?.peakBytes !== undefined,
          hasPeakPercentage: metrics.debugBufferUsed?.peakPercentage !== undefined,
          hasCapacity: metrics.debugBufferUsed?.capacity !== undefined,
          values: metrics.debugBufferUsed,
        },
      };
    }, sonicConfig);

    expect(result.success).toBe(true);

    // Verify inBufferUsed structure
    expect(result.inBufferUsed.hasBytes).toBe(true);
    expect(result.inBufferUsed.hasPercentage).toBe(true);
    expect(result.inBufferUsed.hasPeakBytes).toBe(true);
    expect(result.inBufferUsed.hasPeakPercentage).toBe(true);
    expect(result.inBufferUsed.hasCapacity).toBe(true);

    // Verify outBufferUsed structure
    expect(result.outBufferUsed.hasBytes).toBe(true);
    expect(result.outBufferUsed.hasPercentage).toBe(true);
    expect(result.outBufferUsed.hasPeakBytes).toBe(true);
    expect(result.outBufferUsed.hasPeakPercentage).toBe(true);
    expect(result.outBufferUsed.hasCapacity).toBe(true);

    // Verify debugBufferUsed structure
    expect(result.debugBufferUsed.hasBytes).toBe(true);
    expect(result.debugBufferUsed.hasPercentage).toBe(true);
    expect(result.debugBufferUsed.hasPeakBytes).toBe(true);
    expect(result.debugBufferUsed.hasPeakPercentage).toBe(true);
    expect(result.debugBufferUsed.hasCapacity).toBe(true);

    console.log("\nBuffer metrics structure:");
    console.log("  inBufferUsed:", result.inBufferUsed.values);
    console.log("  outBufferUsed:", result.outBufferUsed.values);
    console.log("  debugBufferUsed:", result.debugBufferUsed.values);
  });

  test("peak values are >= current values", async ({ page, sonicConfig }) => {
    await page.goto("/test/harness.html");

    await page.waitForFunction(() => window.supersonicReady === true, {
      timeout: 10000,
    });

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDefs(["sonic-pi-beep"]);
      await sonic.sync(1);

      // Send some messages to use the input buffer
      for (let i = 0; i < 20; i++) {
        await sonic.send("/s_new", "sonic-pi-beep", 10000 + i, 0, 0,
          "amp", 0.01, "release", 0.1, "note", 60 + (i % 12));
      }

      // Wait for processing
      await new Promise(r => setTimeout(r, 100));

      // Take multiple metric samples
      const samples = [];
      for (let i = 0; i < 10; i++) {
        await new Promise(r => setTimeout(r, 20));
        samples.push(sonic.getMetrics());
      }

      // Free synths
      for (let i = 0; i < 20; i++) {
        await sonic.send("/n_free", 10000 + i);
      }

      return {
        success: true,
        samples: samples.map(m => ({
          in: {
            bytes: m.inBufferUsed?.bytes,
            peak: m.inBufferUsed?.peakBytes,
            percentage: m.inBufferUsed?.percentage,
            peakPercentage: m.inBufferUsed?.peakPercentage,
          },
          out: {
            bytes: m.outBufferUsed?.bytes,
            peak: m.outBufferUsed?.peakBytes,
            percentage: m.outBufferUsed?.percentage,
            peakPercentage: m.outBufferUsed?.peakPercentage,
          },
          debug: {
            bytes: m.debugBufferUsed?.bytes,
            peak: m.debugBufferUsed?.peakBytes,
            percentage: m.debugBufferUsed?.percentage,
            peakPercentage: m.debugBufferUsed?.peakPercentage,
          },
        })),
      };
    }, sonicConfig);

    expect(result.success).toBe(true);

    // Verify peak >= current for all samples
    for (let i = 0; i < result.samples.length; i++) {
      const s = result.samples[i];

      // in buffer: peak >= current
      expect(s.in.peak).toBeGreaterThanOrEqual(s.in.bytes);
      expect(s.in.peakPercentage).toBeGreaterThanOrEqual(s.in.percentage);

      // out buffer: peak >= current
      expect(s.out.peak).toBeGreaterThanOrEqual(s.out.bytes);
      expect(s.out.peakPercentage).toBeGreaterThanOrEqual(s.out.percentage);

      // debug buffer: peak >= current
      expect(s.debug.peak).toBeGreaterThanOrEqual(s.debug.bytes);
      expect(s.debug.peakPercentage).toBeGreaterThanOrEqual(s.debug.percentage);
    }

    console.log("\nPeak validation passed for", result.samples.length, "samples");
    console.log("  Sample ranges:");
    const last = result.samples[result.samples.length - 1];
    console.log(`    inBuffer: ${last.in.bytes} bytes (peak: ${last.in.peak})`);
    console.log(`    outBuffer: ${last.out.bytes} bytes (peak: ${last.out.peak})`);
    console.log(`    debugBuffer: ${last.debug.bytes} bytes (peak: ${last.debug.peak})`);
  });

  test("peak values are <= capacity", async ({ page, sonicConfig }) => {
    await page.goto("/test/harness.html");

    await page.waitForFunction(() => window.supersonicReady === true, {
      timeout: 10000,
    });

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();

      // Wait for some processing
      await new Promise(r => setTimeout(r, 200));

      const metrics = sonic.getMetrics();

      return {
        success: true,
        in: {
          peakBytes: metrics.inBufferUsed?.peakBytes,
          capacity: metrics.inBufferUsed?.capacity,
        },
        out: {
          peakBytes: metrics.outBufferUsed?.peakBytes,
          capacity: metrics.outBufferUsed?.capacity,
        },
        debug: {
          peakBytes: metrics.debugBufferUsed?.peakBytes,
          capacity: metrics.debugBufferUsed?.capacity,
        },
      };
    }, sonicConfig);

    expect(result.success).toBe(true);

    // Peak must be <= capacity
    expect(result.in.peakBytes).toBeLessThanOrEqual(result.in.capacity);
    expect(result.out.peakBytes).toBeLessThanOrEqual(result.out.capacity);
    expect(result.debug.peakBytes).toBeLessThanOrEqual(result.debug.capacity);

    console.log("\nCapacity validation:");
    console.log(`  inBuffer: peak ${result.in.peakBytes} <= capacity ${result.in.capacity}`);
    console.log(`  outBuffer: peak ${result.out.peakBytes} <= capacity ${result.out.capacity}`);
    console.log(`  debugBuffer: peak ${result.debug.peakBytes} <= capacity ${result.debug.capacity}`);
  });

  test("peak increases after buffer activity", async ({ page, sonicConfig }) => {
    await page.goto("/test/harness.html");

    await page.waitForFunction(() => window.supersonicReady === true, {
      timeout: 10000,
    });

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDefs(["sonic-pi-beep"]);
      await sonic.sync(1);

      // Get initial metrics
      const before = sonic.getMetrics();

      // Generate significant buffer activity
      // Send a burst of messages WITHOUT awaiting each one to allow accumulation
      const nodeIds = [];
      const sendPromises = [];
      for (let i = 0; i < 50; i++) {
        const nodeId = 20000 + i;
        nodeIds.push(nodeId);
        // Don't await - fire all messages as fast as possible
        sendPromises.push(sonic.send("/s_new", "sonic-pi-beep", nodeId, 0, 0,
          "amp", 0.01, "release", 0.05, "note", 48 + (i % 24)));
      }

      // Now wait for all to complete
      await Promise.all(sendPromises);

      // Brief additional wait for metrics to update (peaks written every ~43ms)
      await new Promise(r => setTimeout(r, 100));

      // Get metrics after burst
      const after = sonic.getMetrics();

      // Clean up
      for (const nodeId of nodeIds) {
        await sonic.send("/n_free", nodeId);
      }

      return {
        success: true,
        before: {
          inPeak: before.inBufferUsed?.peakBytes,
          outPeak: before.outBufferUsed?.peakBytes,
        },
        after: {
          inPeak: after.inBufferUsed?.peakBytes,
          outPeak: after.outBufferUsed?.peakBytes,
        },
      };
    }, sonicConfig);

    expect(result.success).toBe(true);

    // After sending many messages, peak should be > 0
    expect(result.after.inPeak).toBeGreaterThan(0);

    console.log("\nPeak after activity:");
    console.log(`  inBuffer peak: ${result.before.inPeak} -> ${result.after.inPeak}`);
    console.log(`  outBuffer peak: ${result.before.outPeak} -> ${result.after.outPeak}`);
  });

  test("percentages are calculated correctly", async ({ page, sonicConfig }) => {
    await page.goto("/test/harness.html");

    await page.waitForFunction(() => window.supersonicReady === true, {
      timeout: 10000,
    });

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();

      // Wait for some processing
      await new Promise(r => setTimeout(r, 200));

      const metrics = sonic.getMetrics();

      // Calculate expected percentages
      const checkPercentage = (buf) => {
        const expectedPercentage = (buf.bytes / buf.capacity) * 100;
        const expectedPeakPercentage = (buf.peakBytes / buf.capacity) * 100;
        return {
          bytes: buf.bytes,
          peakBytes: buf.peakBytes,
          capacity: buf.capacity,
          percentage: buf.percentage,
          peakPercentage: buf.peakPercentage,
          expectedPercentage,
          expectedPeakPercentage,
          percentageMatch: Math.abs(buf.percentage - expectedPercentage) < 0.001,
          peakPercentageMatch: Math.abs(buf.peakPercentage - expectedPeakPercentage) < 0.001,
        };
      };

      return {
        success: true,
        in: checkPercentage(metrics.inBufferUsed),
        out: checkPercentage(metrics.outBufferUsed),
        debug: checkPercentage(metrics.debugBufferUsed),
      };
    }, sonicConfig);

    expect(result.success).toBe(true);

    // Verify percentage calculations match
    expect(result.in.percentageMatch).toBe(true);
    expect(result.in.peakPercentageMatch).toBe(true);
    expect(result.out.percentageMatch).toBe(true);
    expect(result.out.peakPercentageMatch).toBe(true);
    expect(result.debug.percentageMatch).toBe(true);
    expect(result.debug.peakPercentageMatch).toBe(true);

    console.log("\nPercentage calculation verification:");
    console.log(`  inBuffer: ${result.in.percentage.toFixed(4)}% (expected ${result.in.expectedPercentage.toFixed(4)}%)`);
    console.log(`  inBuffer peak: ${result.in.peakPercentage.toFixed(4)}% (expected ${result.in.expectedPeakPercentage.toFixed(4)}%)`);
  });
});

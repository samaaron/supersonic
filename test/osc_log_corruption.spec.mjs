import { test, expect } from "./fixtures.mjs";

/**
 * Tests for OSCOutLogWorker ring buffer corruption on startup.
 *
 * The OSCOutLogWorker reads the IN ring buffer to log outgoing OSC messages.
 * A race condition can cause corruption: if IN_HEAD advances between the
 * worker's init (which snapshots IN_HEAD) and its first read, the worker
 * reads buffer positions that don't start on a message boundary, producing
 * "[OSCOutLogWorker] Corrupted message at position N" console errors.
 */

test.describe("OSC Out Log Worker - no corruption on startup", () => {
  test.beforeEach(async ({ page }) => {
    await page.goto("/test/harness.html");
    await page.waitForFunction(() => window.supersonicReady === true, {
      timeout: 10000,
    });
  });

  test("no corrupted message errors during init and normal operation", async ({ page, sonicConfig }) => {
    const corruptionErrors = [];
    const allErrors = [];

    page.on("console", (msg) => {
      const text = msg.text();
      if (text.includes("Corrupted message at position")) {
        corruptionErrors.push(text);
      }
      if (msg.type() === "error") {
        allErrors.push(text);
      }
    });

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.sync();

      // Send several messages to exercise the ring buffer and log worker
      for (let i = 0; i < 20; i++) {
        await sonic.send("/status");
      }

      // Wait for log worker to process all messages
      await new Promise(r => setTimeout(r, 500));

      await sonic.shutdown();

      return { ok: true };
    }, sonicConfig);

    expect(result.ok).toBe(true);

    // The critical assertion: no corruption errors from the log worker
    if (corruptionErrors.length > 0) {
      console.log(`Found ${corruptionErrors.length} corruption errors:`);
      corruptionErrors.slice(0, 5).forEach(e => console.log(`  ${e}`));
    }
    expect(corruptionErrors.length).toBe(0);
  });

  test("no corruption after rapid message burst immediately after init", async ({ page, sonicConfig }) => {
    // This test specifically targets the race condition: messages flowing
    // before the log worker has started its wait loop
    const corruptionErrors = [];

    page.on("console", (msg) => {
      if (msg.text().includes("Corrupted message at position")) {
        corruptionErrors.push(msg.text());
      }
    });

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      // Don't sync — immediately blast messages to maximize race window
      const promises = [];
      for (let i = 0; i < 50; i++) {
        promises.push(sonic.send("/burst_" + i));
      }
      await Promise.all(promises);

      await sonic.sync();

      // Wait for log worker to catch up
      await new Promise(r => setTimeout(r, 500));

      await sonic.shutdown();
      return { ok: true };
    }, sonicConfig);

    expect(result.ok).toBe(true);
    expect(corruptionErrors.length).toBe(0);
  });

  test("no corruption with worker channel messages during startup", async ({ page, sonicConfig }) => {
    // Worker channels write to the same IN buffer — another corruption vector
    const corruptionErrors = [];

    page.on("console", (msg) => {
      if (msg.text().includes("Corrupted message at position")) {
        corruptionErrors.push(msg.text());
      }
    });

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.sync();

      // Create a worker and send messages through it
      const worker = new Worker("/test/assets/osc_channel_test_worker.js", { type: "module" });

      await new Promise((resolve, reject) => {
        const timeout = setTimeout(() => reject(new Error("Worker ready timeout")), 5000);
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
        const timeout = setTimeout(() => reject(new Error("Channel init timeout")), 5000);
        worker.onmessage = (e) => {
          if (e.data.type === "channelReady") {
            clearTimeout(timeout);
            resolve(e.data);
          }
        };
      });

      // Send messages from worker
      await new Promise((resolve, reject) => {
        const timeout = setTimeout(() => reject(new Error("Worker send timeout")), 5000);
        worker.onmessage = (e) => {
          if (e.data.type === "sentMultiple") {
            clearTimeout(timeout);
            resolve(e.data);
          }
        };
        worker.postMessage({ type: "sendMultiple", count: 20, offsetMs: 0 });
      });

      // Wait for log processing
      await new Promise(r => setTimeout(r, 500));

      worker.terminate();
      await sonic.shutdown();
      return { ok: true };
    }, sonicConfig);

    expect(result.ok).toBe(true);
    expect(corruptionErrors.length).toBe(0);
  });
});

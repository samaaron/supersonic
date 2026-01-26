import { test, expect } from "./fixtures.mjs";

/**
 * Tests for centralized OSC out logging via worklet.
 *
 * The OSC log feature centralizes all outbound OSC message logging through the worklet,
 * allowing messages from both main thread and workers to be captured with source IDs.
 */

test.describe("Centralized OSC Out Logging", () => {
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

  test("message:sent event is emitted for main thread messages", async ({ page, sonicConfig }) => {
    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.sync();

      const receivedMessages = [];

      // Listen for message:sent events
      sonic.on("message:sent", (oscData, sourceId) => {
        receivedMessages.push({
          size: oscData?.byteLength ?? oscData?.length ?? 0,
          sourceId: sourceId,
        });
      });

      // Send some messages from main thread
      for (let i = 0; i < 5; i++) {
        await sonic.send("/status");
      }

      // Wait for OSC log to be collected and sent (snapshot interval is typically 50ms)
      await new Promise(r => setTimeout(r, 200));

      await sonic.shutdown();

      return {
        receivedCount: receivedMessages.length,
        messages: receivedMessages.slice(0, 10), // First 10 for inspection
      };
    }, sonicConfig);

    // Should have received at least 5 messages
    expect(result.receivedCount).toBeGreaterThanOrEqual(5);

    // Check that messages have sourceId (may be 0 for main thread)
    for (const msg of result.messages) {
      expect(msg.size).toBeGreaterThan(0);
      expect(typeof msg.sourceId).toBe("number");
    }
  });

  test("main thread messages have sourceId 0", async ({ page, sonicConfig }) => {
    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.sync();

      const sourceIds = new Set();

      // Listen for message:sent events
      sonic.on("message:sent", (oscData, sourceId) => {
        sourceIds.add(sourceId);
      });

      // Send messages only from main thread
      for (let i = 0; i < 3; i++) {
        await sonic.send("/status");
      }

      // Wait for OSC log
      await new Promise(r => setTimeout(r, 200));

      await sonic.shutdown();

      return {
        sourceIds: Array.from(sourceIds),
      };
    }, sonicConfig);

    // Main thread messages should have sourceId 0
    expect(result.sourceIds).toContain(0);
    // Only main thread was sending, so should only see sourceId 0
    expect(result.sourceIds.length).toBe(1);
    expect(result.sourceIds[0]).toBe(0);
  });

  test("worker messages have sourceId >= 1", async ({ page, sonicConfig }) => {
    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.sync();

      const sourceIds = new Set();
      const messagesBySource = new Map();

      // Listen for message:sent events
      sonic.on("message:sent", (oscData, sourceId) => {
        sourceIds.add(sourceId);
        const count = messagesBySource.get(sourceId) || 0;
        messagesBySource.set(sourceId, count + 1);
      });

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
      const WORKER_COUNT = 5;
      await new Promise((resolve, reject) => {
        const timeout = setTimeout(() => reject(new Error("Worker send timeout")), 5000);
        worker.onmessage = (e) => {
          if (e.data.type === "sentMultiple") {
            clearTimeout(timeout);
            resolve(e.data);
          }
        };
        worker.postMessage({ type: "sendMultiple", count: WORKER_COUNT, offsetMs: 0 });
      });

      // Wait for OSC log
      await new Promise(r => setTimeout(r, 300));

      worker.terminate();
      await sonic.shutdown();

      return {
        sourceIds: Array.from(sourceIds),
        messagesBySource: Object.fromEntries(messagesBySource),
      };
    }, sonicConfig);

    // Should have messages from worker (sourceId >= 1)
    const workerSourceIds = result.sourceIds.filter(id => id >= 1);
    expect(workerSourceIds.length).toBeGreaterThanOrEqual(1);

    // Worker sourceId should be exactly 1 (first worker channel)
    expect(workerSourceIds).toContain(1);

    // Should have received messages from worker source
    const workerMessageCount = result.messagesBySource[1] || 0;
    expect(workerMessageCount).toBeGreaterThanOrEqual(1);
  });

  test("multiple workers get unique sourceIds", async ({ page, sonicConfig }) => {
    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.sync();

      const sourceIds = new Set();
      const messagesBySource = new Map();

      // Listen for message:sent events
      sonic.on("message:sent", (oscData, sourceId) => {
        sourceIds.add(sourceId);
        const count = messagesBySource.get(sourceId) || 0;
        messagesBySource.set(sourceId, count + 1);
      });

      // Helper to create and initialize a worker
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

        const channelInfo = await new Promise((resolve, reject) => {
          const timeout = setTimeout(() => reject(new Error(`${name} channel timeout`)), 5000);
          worker.onmessage = (e) => {
            if (e.data.type === "channelReady") {
              clearTimeout(timeout);
              resolve(e.data);
            }
          };
        });

        return { worker, channelInfo };
      };

      // Create three workers
      const { worker: worker1 } = await createWorker("worker1");
      const { worker: worker2 } = await createWorker("worker2");
      const { worker: worker3 } = await createWorker("worker3");

      // Helper to send messages from a worker
      const sendFromWorker = async (worker, count) => {
        return new Promise((resolve, reject) => {
          const timeout = setTimeout(() => reject(new Error("Send timeout")), 5000);
          worker.onmessage = (e) => {
            if (e.data.type === "sentMultiple") {
              clearTimeout(timeout);
              resolve(e.data);
            }
          };
          worker.postMessage({ type: "sendMultiple", count, offsetMs: 0 });
        });
      };

      // Send messages from all workers sequentially to ensure they complete
      await sendFromWorker(worker1, 3);
      await sendFromWorker(worker2, 3);
      await sendFromWorker(worker3, 3);

      // Wait for OSC log - need enough time for multiple snapshot intervals
      // Snapshot interval is typically 50ms, so 500ms should be plenty
      await new Promise(r => setTimeout(r, 500));

      worker1.terminate();
      worker2.terminate();
      worker3.terminate();
      await sonic.shutdown();

      return {
        sourceIds: Array.from(sourceIds).sort((a, b) => a - b),
        messagesBySource: Object.fromEntries(messagesBySource),
      };
    }, sonicConfig);

    // Should have at least some worker sourceIds
    const workerSourceIds = result.sourceIds.filter(id => id >= 1);

    // Log for debugging
    console.log(`Worker sourceIds found: ${JSON.stringify(workerSourceIds)}`);
    console.log(`Messages by source: ${JSON.stringify(result.messagesBySource)}`);

    // Should have 3 unique worker sourceIds (1, 2, 3)
    expect(workerSourceIds.length).toBe(3);
    expect(workerSourceIds).toContain(1);
    expect(workerSourceIds).toContain(2);
    expect(workerSourceIds).toContain(3);

    // Each worker should have sent messages
    expect(result.messagesBySource[1]).toBeGreaterThanOrEqual(1);
    expect(result.messagesBySource[2]).toBeGreaterThanOrEqual(1);
    expect(result.messagesBySource[3]).toBeGreaterThanOrEqual(1);
  });

  test("main thread and worker messages are both captured", async ({ page, sonicConfig }) => {
    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.sync();

      const messagesBySource = new Map();

      // Listen for message:sent events
      sonic.on("message:sent", (oscData, sourceId) => {
        const count = messagesBySource.get(sourceId) || 0;
        messagesBySource.set(sourceId, count + 1);
      });

      // Send from main thread
      const MAIN_COUNT = 5;
      for (let i = 0; i < MAIN_COUNT; i++) {
        await sonic.send("/status");
      }

      // Create a worker and send from it
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

      const WORKER_COUNT = 7;
      await new Promise((resolve, reject) => {
        const timeout = setTimeout(() => reject(new Error("Worker send timeout")), 5000);
        worker.onmessage = (e) => {
          if (e.data.type === "sentMultiple") {
            clearTimeout(timeout);
            resolve(e.data);
          }
        };
        worker.postMessage({ type: "sendMultiple", count: WORKER_COUNT, offsetMs: 0 });
      });

      // Wait for OSC log
      await new Promise(r => setTimeout(r, 300));

      worker.terminate();
      await sonic.shutdown();

      return {
        mainThreadCount: messagesBySource.get(0) || 0,
        workerCount: messagesBySource.get(1) || 0,
        totalSources: messagesBySource.size,
      };
    }, sonicConfig);

    // Both main thread and worker should have messages captured
    expect(result.mainThreadCount).toBeGreaterThanOrEqual(MAIN_COUNT);
    expect(result.workerCount).toBeGreaterThanOrEqual(1);
    expect(result.totalSources).toBe(2); // sourceId 0 and 1

    // Log actual counts for debugging
    console.log(`Main thread messages: ${result.mainThreadCount}, Worker messages: ${result.workerCount}`);
  });

  test("oscData in message:sent contains valid OSC bytes", async ({ page, sonicConfig }) => {
    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.sync();

      const receivedMessages = [];

      // Listen for message:sent events
      sonic.on("message:sent", (oscData, sourceId) => {
        // Check if it's valid OSC (starts with / for messages or # for bundles)
        const bytes = new Uint8Array(oscData.buffer || oscData);
        const firstChar = String.fromCharCode(bytes[0]);
        receivedMessages.push({
          size: bytes.length,
          sourceId,
          firstChar,
          isValidOsc: firstChar === "/" || firstChar === "#",
        });
      });

      // Send a /status message
      await sonic.send("/status");

      // Wait for OSC log
      await new Promise(r => setTimeout(r, 200));

      await sonic.shutdown();

      return {
        messages: receivedMessages,
      };
    }, sonicConfig);

    // Should have captured at least one message
    expect(result.messages.length).toBeGreaterThanOrEqual(1);

    // All messages should be valid OSC
    for (const msg of result.messages) {
      expect(msg.isValidOsc).toBe(true);
      expect(msg.size).toBeGreaterThan(0);
    }
  });
});

// Define MAIN_COUNT and WORKER_COUNT as constants for reference in expects
const MAIN_COUNT = 5;

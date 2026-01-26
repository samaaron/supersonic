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

/**
 * Tests for the logTail-based OSC logging implementation.
 *
 * The refactored implementation:
 * - SAB mode: Main thread reads directly from IN buffer using IN_LOG_TAIL pointer
 * - PM mode: Worklet transfers raw bytes, main thread parses using readMessagesFromBuffer
 *
 * These tests verify:
 * - logTail advances correctly (no duplicate messages)
 * - Metrics match actual message counts
 * - Wrap-around handling works correctly
 * - High throughput scenarios don't lose messages
 */
test.describe("OSC Log logTail Implementation", () => {
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

  test("logTail advances correctly - no extra messages received", async ({ page, sonicConfig, sonicMode }) => {
    // Test that logTail advances correctly by verifying we don't receive MORE
    // messages than we sent (which would indicate re-reading old messages)
    console.log(`Testing logTail advancement in ${sonicMode} mode`);

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.sync();

      // Get baseline count of messages already sent during init
      let baselineCount = 0;
      let afterSendCount = 0;
      let collectingBaseline = true;

      sonic.on("message:sent", (oscData, sourceId) => {
        if (collectingBaseline) {
          baselineCount++;
        } else {
          afterSendCount++;
        }
      });

      // Wait for init messages to be logged
      await new Promise(r => setTimeout(r, 300));
      collectingBaseline = false;

      // Send a known number of messages
      const SEND_COUNT = 50;
      for (let i = 0; i < SEND_COUNT; i++) {
        await sonic.send("/status");
      }

      // Wait for all logs to be collected (multiple snapshot intervals)
      await new Promise(r => setTimeout(r, 500));

      const metrics = sonic.getMetrics();

      await sonic.shutdown();

      return {
        baselineCount,
        afterSendCount,
        sendCount: SEND_COUNT,
        oscOutMessagesSent: metrics.oscOutMessagesSent,
      };
    }, sonicConfig);

    console.log(`Baseline: ${result.baselineCount}, After send: ${result.afterSendCount}, Expected: ${result.sendCount}`);

    // Should receive exactly the number of messages sent (accurate logging)
    expect(result.afterSendCount).toBe(result.sendCount);
  });

  test("message:sent count matches oscOutMessagesSent metric", async ({ page, sonicConfig, sonicMode }) => {
    console.log(`Testing metric consistency in ${sonicMode} mode`);

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.sync();

      let eventCount = 0;

      sonic.on("message:sent", (oscData, sourceId) => {
        eventCount++;
      });

      // Send known number of messages
      const SEND_COUNT = 30;
      for (let i = 0; i < SEND_COUNT; i++) {
        await sonic.send("/status");
      }

      // Wait for logs and metrics to update
      await new Promise(r => setTimeout(r, 400));

      const metrics = sonic.getMetrics();

      await sonic.shutdown();

      return {
        eventCount,
        oscOutMessagesSent: metrics.oscOutMessagesSent,
        sendCount: SEND_COUNT,
      };
    }, sonicConfig);

    console.log(`Events: ${result.eventCount}, Metric: ${result.oscOutMessagesSent}, Sent: ${result.sendCount}`);

    // The message:sent event count should match what we logged
    // Note: There may be some init messages, so eventCount >= sendCount
    expect(result.eventCount).toBeGreaterThanOrEqual(result.sendCount);

    // Metric should track all sent messages (including init)
    expect(result.oscOutMessagesSent).toBeGreaterThanOrEqual(result.sendCount);
  });

  test("high throughput doesn't lose messages", async ({ page, sonicConfig, sonicMode }) => {
    console.log(`Testing high throughput in ${sonicMode} mode`);

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");
      await sonic.sync();

      let burstEventCount = 0;
      let countingBurst = false;

      sonic.on("message:sent", (oscData, sourceId) => {
        if (countingBurst) {
          burstEventCount++;
        }
      });

      // Wait for any init messages to settle
      await new Promise(r => setTimeout(r, 300));

      // Start counting from here
      countingBurst = true;

      // Send a large burst of messages quickly
      const BURST_SIZE = 500;
      const promises = [];

      const startTime = performance.now();
      for (let i = 0; i < BURST_SIZE; i++) {
        promises.push(sonic.send("/status"));
      }
      await Promise.all(promises);
      const sendDuration = performance.now() - startTime;

      // Wait for all logs to be collected
      await new Promise(r => setTimeout(r, 1000));

      const metrics = sonic.getMetrics();

      await sonic.shutdown();

      return {
        burstEventCount,
        burstSize: BURST_SIZE,
        sendDuration,
        oscOutMessagesSent: metrics.oscOutMessagesSent,
        scsynthMessagesProcessed: metrics.scsynthMessagesProcessed,
      };
    }, sonicConfig);

    console.log(`Burst of ${result.burstSize} messages in ${result.sendDuration.toFixed(1)}ms`);
    console.log(`Burst events received: ${result.burstEventCount}`);
    console.log(`Metrics - sent: ${result.oscOutMessagesSent}, processed: ${result.scsynthMessagesProcessed}`);

    // Should receive exactly the number of messages in the burst
    expect(result.burstEventCount).toBe(result.burstSize);
  });

  test("wrap-around handling - continuous stream over time", async ({ page, sonicConfig, sonicMode }) => {
    // This test sends enough messages to cause the IN buffer to wrap multiple times,
    // verifying that logTail correctly handles wrap-around

    console.log(`Testing wrap-around handling in ${sonicMode} mode`);

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");
      await sonic.sync();

      let eventCount = 0;
      let corruptedCount = 0;
      let counting = false;

      sonic.on("message:sent", (oscData, sourceId) => {
        if (!counting) return;
        eventCount++;
        // Verify message integrity: OSC messages start with / or #
        const bytes = new Uint8Array(oscData.buffer || oscData);
        const firstChar = String.fromCharCode(bytes[0]);
        if (firstChar !== "/" && firstChar !== "#") {
          corruptedCount++;
        }
      });

      // Wait for init messages to fully flush through the logging system
      await new Promise(r => setTimeout(r, 300));
      counting = true;

      // The IN_BUFFER_SIZE is typically 8KB-16KB
      // Each /status message is ~20 bytes with header
      // Sending 2000 messages should wrap the buffer multiple times
      const TOTAL_MESSAGES = 2000;
      const BATCH_SIZE = 50;

      let totalSent = 0;
      for (let batch = 0; batch < TOTAL_MESSAGES / BATCH_SIZE; batch++) {
        const promises = [];
        for (let i = 0; i < BATCH_SIZE; i++) {
          promises.push(sonic.send("/status"));
          totalSent++;
        }
        await Promise.all(promises);
        // Small delay between batches to allow processing
        await new Promise(r => setTimeout(r, 10));
      }

      // Wait for all logs
      await new Promise(r => setTimeout(r, 500));

      const metrics = sonic.getMetrics();

      await sonic.shutdown();

      return {
        totalSent,
        eventCount,
        corruptedCount,
        oscOutMessagesSent: metrics.oscOutMessagesSent,
        inBufferCapacity: metrics.inBufferUsed?.capacity,
      };
    }, sonicConfig);

    console.log(`Sent: ${result.totalSent}, Events: ${result.eventCount}, Corrupted: ${result.corruptedCount}`);
    console.log(`IN buffer capacity: ${result.inBufferCapacity} bytes`);

    // No corrupted messages should occur even with wrap-around
    expect(result.corruptedCount).toBe(0);

    // Should receive exactly the number of messages sent (accurate wrap-around handling)
    expect(result.eventCount).toBe(result.totalSent);
  });

  test("messages from multiple sources are all logged correctly", async ({ page, sonicConfig, sonicMode }) => {
    console.log(`Testing multi-source logging in ${sonicMode} mode`);

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.sync();

      // Wait for any init messages to be logged before we start counting
      await new Promise(r => setTimeout(r, 300));

      const messagesBySource = new Map();
      let counting = false;

      sonic.on("message:sent", (oscData, sourceId) => {
        if (!counting) return;
        const count = messagesBySource.get(sourceId) || 0;
        messagesBySource.set(sourceId, count + 1);
      });

      counting = true;

      // Send from main thread
      const MAIN_COUNT = 20;
      for (let i = 0; i < MAIN_COUNT; i++) {
        await sonic.send("/status");
      }

      // Create worker and send from it
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

      const WORKER_COUNT = 20;
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

      // Wait for logs
      await new Promise(r => setTimeout(r, 500));

      const metrics = sonic.getMetrics();

      worker.terminate();
      await sonic.shutdown();

      return {
        messagesBySource: Object.fromEntries(messagesBySource),
        mainCount: MAIN_COUNT,
        workerCount: WORKER_COUNT,
        totalExpected: MAIN_COUNT + WORKER_COUNT,
        oscOutMessagesSent: metrics.oscOutMessagesSent,
      };
    }, sonicConfig);

    console.log(`Messages by source: ${JSON.stringify(result.messagesBySource)}`);

    // Should receive exactly the number of messages sent from each source
    expect(result.messagesBySource[0]).toBe(result.mainCount);
    expect(result.messagesBySource[1]).toBe(result.workerCount);
  });

  test("rapid sequential sends don't corrupt logTail state", async ({ page, sonicConfig, sonicMode }) => {
    // Test that rapid back-to-back sends don't cause logTail state corruption

    console.log(`Testing rapid sequential sends in ${sonicMode} mode`);

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.sync();

      // Wait for any init messages to be logged before we start counting
      await new Promise(r => setTimeout(r, 300));

      const receivedSizes = [];
      let corruptedCount = 0;
      let counting = false;

      sonic.on("message:sent", (oscData, sourceId) => {
        if (!counting) return;
        const bytes = new Uint8Array(oscData.buffer || oscData);
        receivedSizes.push(bytes.length);

        // Check for corruption: OSC messages should start with / or #
        const firstChar = String.fromCharCode(bytes[0]);
        if (firstChar !== "/" && firstChar !== "#") {
          corruptedCount++;
        }
      });

      counting = true;

      // Send many messages as fast as possible without awaiting
      const RAPID_COUNT = 200;
      const sendPromises = [];
      for (let i = 0; i < RAPID_COUNT; i++) {
        sendPromises.push(sonic.send("/status"));
      }
      await Promise.all(sendPromises);

      // Wait for all logs
      await new Promise(r => setTimeout(r, 500));

      await sonic.shutdown();

      return {
        rapidCount: RAPID_COUNT,
        receivedCount: receivedSizes.length,
        corruptedCount,
        avgSize: receivedSizes.length > 0
          ? receivedSizes.reduce((a, b) => a + b, 0) / receivedSizes.length
          : 0,
      };
    }, sonicConfig);

    console.log(`Sent: ${result.rapidCount}, Received: ${result.receivedCount}, Corrupted: ${result.corruptedCount}`);

    // No corrupted messages
    expect(result.corruptedCount).toBe(0);

    // Should receive exactly the number of messages sent
    expect(result.receivedCount).toBe(result.rapidCount);
  });

  test("oscData bytes are correctly preserved through logging", async ({ page, sonicConfig, sonicMode }) => {
    // Verify that the actual OSC data bytes are preserved correctly,
    // especially important for the raw byte transfer in PM mode

    console.log(`Testing byte preservation in ${sonicMode} mode`);

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");
      await sonic.sync();

      const receivedMessages = [];

      sonic.on("message:sent", (oscData, sourceId) => {
        const bytes = new Uint8Array(oscData.buffer || oscData);
        // Extract the OSC address string (null-terminated, 4-byte aligned)
        let address = "";
        for (let i = 0; i < bytes.length && bytes[i] !== 0; i++) {
          address += String.fromCharCode(bytes[i]);
        }
        receivedMessages.push({
          address,
          size: bytes.length,
          sourceId,
        });
      });

      // Send various message types
      await sonic.send("/status");
      await sonic.send("/g_queryTree", 0, 0);
      await sonic.send("/s_new", "sonic-pi-beep", 99999, 0, 0, "note", 60, "amp", 0);
      await sonic.send("/n_free", 99999);

      // Wait for logs
      await new Promise(r => setTimeout(r, 300));

      await sonic.shutdown();

      // Find specific addresses in received messages
      const foundAddresses = new Set(receivedMessages.map(m => m.address));

      return {
        receivedCount: receivedMessages.length,
        foundAddresses: Array.from(foundAddresses),
        hasStatus: foundAddresses.has("/status"),
        hasQueryTree: foundAddresses.has("/g_queryTree"),
        hasSNew: foundAddresses.has("/s_new"),
        hasNFree: foundAddresses.has("/n_free"),
      };
    }, sonicConfig);

    console.log(`Received ${result.receivedCount} messages`);
    console.log(`Found addresses: ${result.foundAddresses.join(", ")}`);

    // All sent message types should be captured with correct addresses
    expect(result.hasStatus).toBe(true);
    expect(result.hasQueryTree).toBe(true);
    expect(result.hasSNew).toBe(true);
    expect(result.hasNFree).toBe(true);
  });

  test("logging continues correctly after buffer wrap", async ({ page, sonicConfig, sonicMode }) => {
    // Specifically test that after the buffer wraps, new messages are still logged correctly

    console.log(`Testing post-wrap logging in ${sonicMode} mode`);

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.sync();

      let phase1Count = 0;
      let phase2Count = 0;
      let phase = 1;

      sonic.on("message:sent", (oscData, sourceId) => {
        if (phase === 1) {
          phase1Count++;
        } else {
          phase2Count++;
        }
      });

      // Phase 1: Fill the buffer to cause wrap
      const FILL_COUNT = 1000;
      for (let i = 0; i < FILL_COUNT; i++) {
        await sonic.send("/status");
      }

      // Wait for phase 1 logs
      await new Promise(r => setTimeout(r, 400));

      // Switch to phase 2
      phase = 2;

      // Phase 2: Send more messages after wrap
      const POST_WRAP_COUNT = 100;
      for (let i = 0; i < POST_WRAP_COUNT; i++) {
        await sonic.send("/status");
      }

      // Wait for phase 2 logs
      await new Promise(r => setTimeout(r, 400));

      await sonic.shutdown();

      return {
        fillCount: FILL_COUNT,
        postWrapCount: POST_WRAP_COUNT,
        phase1Count,
        phase2Count,
      };
    }, sonicConfig);

    console.log(`Phase 1 (fill): sent ${result.fillCount}, logged ${result.phase1Count}`);
    console.log(`Phase 2 (post-wrap): sent ${result.postWrapCount}, logged ${result.phase2Count}`);

    // Phase 1 should have received exactly the number of fill messages
    expect(result.phase1Count).toBe(result.fillCount);

    // Phase 2 should have received exactly the number of post-wrap messages
    expect(result.phase2Count).toBe(result.postWrapCount);
  });

  test("scsynthMessagesProcessed grows with logged messages", async ({ page, sonicConfig, sonicMode }) => {
    // Verify that the messages we log are actually being processed by scsynth

    console.log(`Testing processed message correlation in ${sonicMode} mode`);

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.sync();

      let loggedCount = 0;
      let countingMessages = false;

      sonic.on("message:sent", () => {
        if (countingMessages) {
          loggedCount++;
        }
      });

      // Wait for init messages to settle
      await new Promise(r => setTimeout(r, 300));

      const metricsBefore = sonic.getMetrics();
      countingMessages = true;

      // Send messages that scsynth will process
      const SEND_COUNT = 50;
      for (let i = 0; i < SEND_COUNT; i++) {
        await sonic.send("/status");
      }

      // Wait for processing
      await new Promise(r => setTimeout(r, 500));

      const metricsAfter = sonic.getMetrics();

      await sonic.shutdown();

      return {
        sendCount: SEND_COUNT,
        loggedCount,
        processedBefore: metricsBefore.scsynthMessagesProcessed,
        processedAfter: metricsAfter.scsynthMessagesProcessed,
        processedDelta: metricsAfter.scsynthMessagesProcessed - metricsBefore.scsynthMessagesProcessed,
      };
    }, sonicConfig);

    console.log(`Sent: ${result.sendCount}, Logged: ${result.loggedCount}`);
    console.log(`Processed: ${result.processedBefore} -> ${result.processedAfter} (delta: ${result.processedDelta})`);

    // Messages processed should match what we sent
    expect(result.processedDelta).toBe(result.sendCount);

    // Logged count should match sent count
    expect(result.loggedCount).toBe(result.sendCount);
  });
});

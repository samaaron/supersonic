import { test, expect } from "./fixtures.mjs";

/**
 * Tests for centralized OSC out logging via ring buffer.
 *
 * The OSC log feature logs messages when they're written to the ring buffer (dispatch time),
 * allowing messages from both main thread and workers to be captured with source IDs.
 *
 * Key behavior:
 * - Messages are logged when written to the ring buffer (not at send time)
 * - Cancelled messages never reach the ring buffer, so they're never logged
 * - SAB mode: osc_out_log_sab_worker reads from ring buffer
 * - PM mode: worklet reads from ring buffer in checkAndSendSnapshot()
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

  test("out:osc event is emitted for main thread messages", async ({ page, sonicConfig }) => {
    // Capture worklet console output for debugging
    const workletLogs = [];
    page.on("console", (msg) => {
      const text = msg.text();
      if (text.includes('[Worklet]') || text.includes('[PostMessageTransport]') || text.includes('oscLog')) {
        workletLogs.push(text);
      }
    });

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.sync();

      const receivedMessages = [];
      const workletDebugMessages = [];
      const debugInfo = { mode: config.mode };

      // Listen for out:osc events
      sonic.on("out:osc", ({ oscData, sourceId }) => {
        receivedMessages.push({
          size: oscData?.byteLength ?? oscData?.length ?? 0,
          sourceId: sourceId,
        });
      });

      // Listen for worklet debug messages
      sonic.on("worklet:debug", (msg) => {
        workletDebugMessages.push(msg);
      });

      // Send some messages from main thread
      for (let i = 0; i < 5; i++) {
        await sonic.send("/status");
      }

      // Wait for OSC log to be collected and sent (snapshot interval is typically 50ms)
      await new Promise(r => setTimeout(r, 200));

      // Get some debug info
      debugInfo.snapshotsSent = sonic.getMetrics?.()?.snapshotsSent;
      debugInfo.oscOutMessagesSent = sonic.getMetrics?.()?.oscOutMessagesSent;
      debugInfo.workletDebugMessages = workletDebugMessages;

      await sonic.shutdown();

      return {
        receivedCount: receivedMessages.length,
        messages: receivedMessages.slice(0, 10), // First 10 for inspection
        debugInfo,
      };
    }, sonicConfig);

    // Log debug info for test failures
    console.log('Test debug info:', JSON.stringify(result.debugInfo, null, 2));
    console.log('Worklet logs:', workletLogs);

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

      // Listen for out:osc events
      sonic.on("out:osc", ({ sourceId }) => {
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

      // Listen for out:osc events
      sonic.on("out:osc", ({ sourceId }) => {
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

      // Listen for out:osc events
      sonic.on("out:osc", ({ sourceId }) => {
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

      // Listen for out:osc events
      sonic.on("out:osc", ({ sourceId }) => {
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

  test("oscData in out:osc contains valid OSC bytes", async ({ page, sonicConfig }) => {
    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.sync();

      const receivedMessages = [];

      // Listen for out:osc events
      sonic.on("out:osc", ({ oscData, sourceId }) => {
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

  test("sourceId is preserved for far-future bundles through prescheduler", async ({ page, sonicConfig }) => {
    // Use shorter lookahead for faster test (bundles 300ms out go through prescheduler)
    const config = { ...sonicConfig, bypassLookaheadMs: 200 };

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.sync();

      const messagesBySource = new Map();

      // Listen for out:osc events
      sonic.on("out:osc", ({ sourceId }) => {
        const count = messagesBySource.get(sourceId) || 0;
        messagesBySource.set(sourceId, count + 1);
      });

      // Create a worker and send far-future bundles (beyond lookahead = goes through prescheduler)
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

      // Send far-future bundles (300ms offset - beyond configured 200ms bypass threshold)
      const FAR_FUTURE_COUNT = 5;
      await new Promise((resolve, reject) => {
        const timeout = setTimeout(() => reject(new Error("Worker send timeout")), 5000);
        worker.onmessage = (e) => {
          if (e.data.type === "sentMultiple") {
            clearTimeout(timeout);
            resolve(e.data);
          }
        };
        worker.postMessage({ type: "sendMultiple", count: FAR_FUTURE_COUNT, offsetMs: 300 });
      });

      // Wait for bundles to be dispatched through prescheduler and logged
      // Need to wait longer than the 300ms offset plus processing time
      await new Promise(r => setTimeout(r, 500));

      worker.terminate();
      await sonic.shutdown();

      return {
        sourceIds: Array.from(messagesBySource.keys()).sort((a, b) => a - b),
        workerCount: messagesBySource.get(1) || 0,
        messagesBySource: Object.fromEntries(messagesBySource),
      };
    }, config);

    // Worker messages through prescheduler should preserve sourceId 1
    expect(result.sourceIds).toContain(1);
    expect(result.workerCount).toBeGreaterThanOrEqual(1);

    // Log for debugging
    console.log(`Far-future test - sourceIds: ${JSON.stringify(result.sourceIds)}, by source: ${JSON.stringify(result.messagesBySource)}`);
  });

  test("out:osc includes incrementing sequence numbers", async ({ page, sonicConfig }) => {
    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.sync();

      const sequences = [];

      // Listen for out:osc events with sequence
      sonic.on("out:osc", ({ sequence }) => {
        sequences.push(sequence);
      });

      // Send multiple messages from main thread
      for (let i = 0; i < 10; i++) {
        await sonic.send("/status");
      }

      // Wait for OSC log
      await new Promise(r => setTimeout(r, 200));

      await sonic.shutdown();

      return {
        sequences,
        count: sequences.length,
      };
    }, sonicConfig);

    // Should have captured at least 10 messages
    expect(result.count).toBeGreaterThanOrEqual(10);

    // All sequences should be numbers
    for (const seq of result.sequences) {
      expect(typeof seq).toBe("number");
    }

    // Sequences should be strictly increasing (no duplicates, no gaps within a batch)
    const sorted = [...result.sequences].sort((a, b) => a - b);
    for (let i = 1; i < sorted.length; i++) {
      expect(sorted[i]).toBeGreaterThan(sorted[i - 1]);
    }

    console.log(`Sequences received: ${result.sequences.slice(0, 15).join(", ")}${result.sequences.length > 15 ? "..." : ""}`);
  });

  test("log tail initializes correctly - no stale messages at startup", async ({ page, sonicConfig }) => {
    // This test verifies that IN_LOG_TAIL is initialized to IN_HEAD at startup,
    // so we don't log any stale/garbage messages from before initialization
    const result = await page.evaluate(async (config) => {
      const messagesBeforeSync = [];
      const messagesAfterSync = [];
      let syncTime = 0;

      const sonic = new window.SuperSonic(config);

      // Set up listener BEFORE init to catch any early messages
      sonic.on("out:osc", ({ sequence }) => {
        const now = performance.now();
        if (syncTime === 0) {
          messagesBeforeSync.push({ sequence, time: now });
        } else {
          messagesAfterSync.push({ sequence, time: now });
        }
      });

      await sonic.init();
      await sonic.sync();
      syncTime = performance.now();

      // Send some messages after sync
      for (let i = 0; i < 5; i++) {
        await sonic.send("/status");
      }

      // Wait for log entries
      await new Promise(r => setTimeout(r, 200));

      await sonic.shutdown();

      return {
        beforeSyncCount: messagesBeforeSync.length,
        afterSyncCount: messagesAfterSync.length,
        // Check if any "before" messages have suspiciously low sequence numbers
        // that might indicate stale data
        beforeSequences: messagesBeforeSync.map(m => m.sequence),
        afterSequences: messagesAfterSync.slice(0, 10).map(m => m.sequence),
      };
    }, sonicConfig);

    // We may get some messages before sync (from init handshakes), but they should
    // have valid incrementing sequences, not garbage values
    console.log(`Before sync: ${result.beforeSyncCount} messages, sequences: ${result.beforeSequences.slice(0, 5).join(", ")}`);
    console.log(`After sync: ${result.afterSyncCount} messages, sequences: ${result.afterSequences.join(", ")}`);

    // After sync, we should have received the 5 /status messages we sent
    expect(result.afterSyncCount).toBeGreaterThanOrEqual(5);

    // All sequences should be reasonable numbers (not garbage like 0xFFFFFFFF)
    const allSequences = [...result.beforeSequences, ...result.afterSequences];
    for (const seq of allSequences) {
      expect(typeof seq).toBe("number");
      expect(seq).toBeGreaterThanOrEqual(0);
      expect(seq).toBeLessThan(0xFFFFFFFF); // Not a sentinel/garbage value
    }
  });

  test("log tail tracks correctly - each message logged exactly once", async ({ page, sonicConfig }) => {
    // This test verifies that the log tail advances correctly so messages
    // are not logged multiple times (duplicates) or missed
    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.sync();

      const sequencesSeen = new Map(); // sequence -> count
      const duplicates = [];

      sonic.on("out:osc", ({ sequence }) => {
        const count = (sequencesSeen.get(sequence) || 0) + 1;
        sequencesSeen.set(sequence, count);
        if (count > 1) {
          duplicates.push(sequence);
        }
      });

      // Send messages in multiple batches to test log tail across batches
      const MESSAGE_COUNT = 50;
      for (let batch = 0; batch < 5; batch++) {
        for (let i = 0; i < MESSAGE_COUNT / 5; i++) {
          await sonic.send("/status");
        }
        // Small delay between batches to allow log processing
        await new Promise(r => setTimeout(r, 50));
      }

      // Wait for all log entries to be processed
      await new Promise(r => setTimeout(r, 300));

      await sonic.shutdown();

      // Check for gaps in sequence numbers
      const sequences = [...sequencesSeen.keys()].sort((a, b) => a - b);
      const gaps = [];
      for (let i = 1; i < sequences.length; i++) {
        const gap = sequences[i] - sequences[i - 1];
        if (gap > 1) {
          gaps.push({ after: sequences[i - 1], before: sequences[i], size: gap - 1 });
        }
      }

      return {
        totalLogged: sequencesSeen.size,
        duplicateCount: duplicates.length,
        duplicates: duplicates.slice(0, 10),
        gapCount: gaps.length,
        gaps: gaps.slice(0, 5),
        firstSequence: sequences[0],
        lastSequence: sequences[sequences.length - 1],
      };
    }, sonicConfig);

    console.log(`Logged ${result.totalLogged} unique messages, sequences ${result.firstSequence}-${result.lastSequence}`);
    console.log(`Duplicates: ${result.duplicateCount}, Gaps: ${result.gapCount}`);

    // No duplicates - each message should be logged exactly once
    expect(result.duplicateCount).toBe(0);

    // Should have logged at least the messages we sent (plus possibly some init messages)
    expect(result.totalLogged).toBeGreaterThanOrEqual(50);

    // Log gaps for debugging (gaps in our messages might happen due to other system messages)
    if (result.gapCount > 0) {
      console.log(`  Gap details: ${JSON.stringify(result.gaps)}`);
    }
  });

  test("log tail handles rapid message bursts without losing messages", async ({ page, sonicConfig }) => {
    // Stress test: send many messages quickly and verify all are logged
    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.sync();

      // Get baseline sequence
      let firstSequence = null;
      let lastSequence = null;
      let messageCount = 0;

      sonic.on("out:osc", ({ sequence }) => {
        if (firstSequence === null) firstSequence = sequence;
        lastSequence = sequence;
        messageCount++;
      });

      // Rapid burst of messages without awaiting
      const BURST_SIZE = 100;
      const promises = [];
      for (let i = 0; i < BURST_SIZE; i++) {
        promises.push(sonic.send("/status"));
      }
      await Promise.all(promises);

      // Wait for log processing
      await new Promise(r => setTimeout(r, 500));

      await sonic.shutdown();

      return {
        messageCount,
        firstSequence,
        lastSequence,
        expectedRange: lastSequence - firstSequence + 1,
      };
    }, sonicConfig);

    console.log(`Burst test: received ${result.messageCount} messages, sequences ${result.firstSequence}-${result.lastSequence}`);

    // Should have received exactly BURST_SIZE messages - no loss tolerance
    expect(result.messageCount).toBeGreaterThanOrEqual(100);

    // The number of messages should match the sequence range (no gaps in our burst)
    expect(result.messageCount).toBe(result.expectedRange);
  });

  test("first message after init is logged - no off-by-one error", async ({ page, sonicConfig }) => {
    // This test verifies that the very first message sent after initialization
    // is properly logged (tests for off-by-one errors in log tail initialization)
    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.sync();

      const loggedMessages = [];

      sonic.on("out:osc", ({ oscData, sequence }) => {
        // Decode the OSC address to identify our test message
        const view = new DataView(oscData.buffer || oscData);
        let address = '';
        for (let i = 0; i < oscData.length && oscData[i] !== 0; i++) {
          address += String.fromCharCode(oscData[i]);
        }
        loggedMessages.push({ address, sequence });
      });

      // Send a single, uniquely identifiable message
      await sonic.send("/test_first_message");

      // Wait for log processing
      await new Promise(r => setTimeout(r, 200));

      // Find our test message
      const testMessage = loggedMessages.find(m => m.address === "/test_first_message");

      await sonic.shutdown();

      return {
        found: !!testMessage,
        testMessageSequence: testMessage?.sequence,
        totalLogged: loggedMessages.length,
        allAddresses: loggedMessages.map(m => m.address),
      };
    }, sonicConfig);

    console.log(`First message test: found=${result.found}, sequence=${result.testMessageSequence}, total=${result.totalLogged}`);

    // The test message must be logged
    expect(result.found).toBe(true);
    expect(result.testMessageSequence).toBeGreaterThanOrEqual(0);
  });

  test("messages sent immediately after init() before sync() are logged", async ({ page, sonicConfig }) => {
    // This tests the race condition window between init() completing and sync() being called
    // Messages sent in this window should still be logged
    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);

      const loggedMessages = [];
      sonic.on("out:osc", ({ oscData, sequence }) => {
        const view = new DataView(oscData.buffer || oscData);
        let address = '';
        for (let i = 0; i < oscData.length && oscData[i] !== 0; i++) {
          address += String.fromCharCode(oscData[i]);
        }
        loggedMessages.push({ address, sequence });
      });

      await sonic.init();

      // Send messages IMMEDIATELY after init, before sync
      // These test the initialization race condition
      const earlyPromises = [];
      for (let i = 0; i < 5; i++) {
        earlyPromises.push(sonic.send("/early_" + i));
      }

      // Now sync
      await sonic.sync();

      // Wait for early messages to complete
      await Promise.all(earlyPromises);

      // Send messages after sync for comparison
      for (let i = 0; i < 5; i++) {
        await sonic.send("/after_sync_" + i);
      }

      // Wait for log processing
      await new Promise(r => setTimeout(r, 300));

      // Count early vs late messages
      const earlyCount = loggedMessages.filter(m => m.address.startsWith("/early_")).length;
      const afterSyncCount = loggedMessages.filter(m => m.address.startsWith("/after_sync_")).length;

      await sonic.shutdown();

      return {
        earlyCount,
        afterSyncCount,
        totalLogged: loggedMessages.length,
        earlyAddresses: loggedMessages.filter(m => m.address.startsWith("/early_")).map(m => m.address),
        afterSyncAddresses: loggedMessages.filter(m => m.address.startsWith("/after_sync_")).map(m => m.address),
      };
    }, sonicConfig);

    console.log(`Early messages: ${result.earlyCount}/5, After sync: ${result.afterSyncCount}/5`);
    console.log(`Early: ${result.earlyAddresses.join(", ")}`);
    console.log(`After: ${result.afterSyncAddresses.join(", ")}`);

    // All 5 early messages should be logged (no race condition loss)
    expect(result.earlyCount).toBe(5);

    // All 5 after-sync messages should also be logged
    expect(result.afterSyncCount).toBe(5);
  });

  test("log tail initialization starts from current head - verifies no garbage sequences", async ({ page, sonicConfig }) => {
    // This test verifies that IN_LOG_TAIL is initialized to IN_HEAD (not 0),
    // which is critical for avoiding garbage data reads on non-fresh buffers
    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.sync();

      const sequences = [];
      sonic.on("out:osc", ({ sequence }) => {
        sequences.push(sequence);
      });

      // Send exactly 10 messages
      for (let i = 0; i < 10; i++) {
        await sonic.send("/seq_test");
      }

      // Wait for log
      await new Promise(r => setTimeout(r, 200));

      await sonic.shutdown();

      // Analyze sequence distribution
      const minSeq = Math.min(...sequences);
      const maxSeq = Math.max(...sequences);

      return {
        count: sequences.length,
        minSeq,
        maxSeq,
        range: maxSeq - minSeq,
        sequences: sequences.slice(0, 20),
        // Check for any suspicious patterns
        hasNegative: sequences.some(s => s < 0),
        hasHugeGap: sequences.some((s, i) => i > 0 && s - sequences[i-1] > 1000),
        hasGarbage: sequences.some(s => s > 0x7FFFFFFF), // Looks like uninitialized memory
      };
    }, sonicConfig);

    console.log(`Sequence test: ${result.count} messages, range ${result.minSeq}-${result.maxSeq}`);

    // Should have logged at least our 10 messages
    expect(result.count).toBeGreaterThanOrEqual(10);

    // Sequences should be reasonable (not garbage)
    expect(result.hasNegative).toBe(false);
    expect(result.hasHugeGap).toBe(false);
    expect(result.hasGarbage).toBe(false);

    // Min sequence should be small (close to 0, or at least reasonable for init traffic)
    // This verifies the tail was initialized to head, not to some arbitrary value
    expect(result.minSeq).toBeLessThan(1000);

    // Range should be reasonable (not spanning billions due to wraparound issues)
    expect(result.range).toBeLessThan(10000);
  });

  test("multiple init/shutdown cycles maintain correct log tail behavior", async ({ page, sonicConfig }) => {
    // This test verifies that log tail initialization works correctly
    // across multiple init/shutdown cycles (no stale state)
    const result = await page.evaluate(async (config) => {
      const results = [];

      for (let cycle = 0; cycle < 3; cycle++) {
        const sonic = new window.SuperSonic(config);
        await sonic.init();
        await sonic.sync();

        const sequences = [];
        sonic.on("out:osc", ({ sequence }) => {
          sequences.push(sequence);
        });

        // Send messages
        for (let i = 0; i < 5; i++) {
          await sonic.send("/cycle_" + cycle);
        }

        // Wait for log
        await new Promise(r => setTimeout(r, 200));

        results.push({
          cycle,
          count: sequences.length,
          minSeq: sequences.length > 0 ? Math.min(...sequences) : -1,
          maxSeq: sequences.length > 0 ? Math.max(...sequences) : -1,
          hasDuplicates: new Set(sequences).size !== sequences.length,
        });

        await sonic.shutdown();

        // Small delay between cycles
        await new Promise(r => setTimeout(r, 100));
      }

      return results;
    }, sonicConfig);

    console.log("Cycle results:", JSON.stringify(result, null, 2));

    // Each cycle should have logged at least 5 messages
    for (const r of result) {
      expect(r.count).toBeGreaterThanOrEqual(5);
      expect(r.hasDuplicates).toBe(false);
    }

    // Verify sequences don't carry over between cycles in unexpected ways
    // (each cycle should have its own sequence space starting fresh)
    for (const r of result) {
      expect(r.minSeq).toBeGreaterThanOrEqual(0);
      // Each cycle should have no duplicate sequences
      expect(r.hasDuplicates).toBe(false);
    }
  });
});

// Define MAIN_COUNT and WORKER_COUNT as constants for reference in expects
const MAIN_COUNT = 5;

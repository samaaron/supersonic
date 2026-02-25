import { test, expect } from "./fixtures.mjs";

/**
 * Tests for nextNodeId() — range-based atomic node ID allocator.
 *
 * Verifies that SuperSonic and OscChannel produce globally unique node IDs
 * without coordination conflicts, including across multiple concurrent workers.
 */

test.describe("nextNodeId()", () => {
  test.beforeEach(async ({ page }) => {
    page.on("console", (msg) => {
      if (msg.type() === "error") {
        console.error("Browser console error:", msg.text());
      }
    });

    await page.goto("/test/harness.html");
    await page.waitForFunction(() => window.supersonicReady === true, {
      timeout: 10000,
    });
  });

  test("returns incrementing IDs starting at >= 1000", async ({ page, sonicConfig }) => {
    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();

      const ids = [];
      for (let i = 0; i < 10; i++) {
        ids.push(sonic.nextNodeId());
      }

      await sonic.shutdown();
      return { ids };
    }, sonicConfig);

    // All IDs should be >= 1000
    for (const id of result.ids) {
      expect(id).toBeGreaterThanOrEqual(1000);
    }

    // IDs should be strictly incrementing
    for (let i = 1; i < result.ids.length; i++) {
      expect(result.ids[i]).toBe(result.ids[i - 1] + 1);
    }
  });

  test("main thread and worker IDs do not overlap", async ({ page, sonicConfig }) => {
    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();

      // Generate IDs from main thread
      const mainIds = [];
      for (let i = 0; i < 50; i++) {
        mainIds.push(sonic.nextNodeId());
      }

      // Create a worker and generate IDs from it
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
        const timeout = setTimeout(() => reject(new Error("Channel ready timeout")), 5000);
        worker.onmessage = (e) => {
          if (e.data.type === "channelReady") {
            clearTimeout(timeout);
            resolve();
          }
        };
      });

      // Generate IDs from worker
      const workerIds = await new Promise((resolve, reject) => {
        const timeout = setTimeout(() => reject(new Error("Generate timeout")), 5000);
        worker.onmessage = (e) => {
          if (e.data.type === "nodeIds") {
            clearTimeout(timeout);
            resolve(e.data.ids);
          }
        };
        worker.postMessage({ type: "generateNodeIds", count: 50 });
      });

      worker.terminate();
      await sonic.shutdown();

      return { mainIds, workerIds };
    }, sonicConfig);

    // All IDs >= 1000
    for (const id of [...result.mainIds, ...result.workerIds]) {
      expect(id).toBeGreaterThanOrEqual(1000);
    }

    // No overlap between main and worker
    const mainSet = new Set(result.mainIds);
    const overlap = result.workerIds.filter(id => mainSet.has(id));
    expect(overlap).toEqual([]);

    // Each set is internally contiguous
    for (const ids of [result.mainIds, result.workerIds]) {
      for (let i = 1; i < ids.length; i++) {
        expect(ids[i]).toBe(ids[i - 1] + 1);
      }
    }
  });

  test("five concurrent workers produce non-overlapping IDs across range boundaries", async ({ page, sonicConfig }) => {
    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();

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

        await new Promise((resolve, reject) => {
          const timeout = setTimeout(() => reject(new Error(`${name} channel timeout`)), 5000);
          worker.onmessage = (e) => {
            if (e.data.type === "channelReady") {
              clearTimeout(timeout);
              resolve();
            }
          };
        });

        return worker;
      };

      const workers = [];
      for (let i = 0; i < 5; i++) {
        workers.push(await createWorker(`worker${i}`));
      }

      // Generate 10,000 IDs from each of 5 workers concurrently at 10k/sec.
      // This forces multiple range claims in both modes:
      //   SAB:  initial range = 1,000 → 10 range claims per worker
      //   PM:   initial range = 10,000 → 1 refill per worker
      // The rate-limited generation yields to the event loop between
      // batches, allowing PM workers to receive pre-fetched ranges.
      const IDS_PER_WORKER = 10000;
      const RATE = 10000;

      const generateFromWorker = (worker) => {
        return new Promise((resolve, reject) => {
          const timeout = setTimeout(() => reject(new Error("Generate timeout")), 5000);
          worker.onmessage = (e) => {
            if (e.data.type === "nodeIds") {
              clearTimeout(timeout);
              resolve(e.data.ids);
            }
          };
          worker.postMessage({ type: "generateNodeIds", count: IDS_PER_WORKER, rate: RATE });
        });
      };

      // Fire all 5 at the same time
      const allWorkerIds = await Promise.all(
        workers.map(w => generateFromWorker(w))
      );

      for (const w of workers) w.terminate();
      await sonic.shutdown();

      // Verify inside page.evaluate to avoid serializing 50k integers
      const allIds = allWorkerIds.flat();
      const uniqueIds = new Set(allIds);
      const allAbove1000 = allIds.every(id => id >= 1000);
      const minId = Math.min(...allIds);

      // Check each worker's IDs are internally incrementing
      const workerResults = allWorkerIds.map((ids, i) => {
        let incrementing = true;
        for (let j = 1; j < ids.length; j++) {
          if (ids[j] <= ids[j - 1]) {
            incrementing = false;
            break;
          }
        }
        return { count: ids.length, incrementing };
      });

      return {
        totalIds: allIds.length,
        uniqueCount: uniqueIds.size,
        allAbove1000,
        minId,
        workerResults,
      };
    }, sonicConfig);

    const totalExpected = 5 * 10000;

    expect(result.totalIds).toBe(totalExpected);
    expect(result.allAbove1000).toBe(true);
    expect(result.minId).toBeGreaterThanOrEqual(1000);

    // All 50,000 IDs should be unique (no overlaps between any worker)
    expect(result.uniqueCount).toBe(totalExpected);

    // Each worker's IDs should be internally incrementing
    for (const wr of result.workerResults) {
      expect(wr.count).toBe(10000);
      expect(wr.incrementing).toBe(true);
    }
  });

  test("IDs remain unique across range boundary", async ({ page, sonicConfig }) => {
    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();

      // Generate more than one range worth of IDs (range = 1000)
      const ids = [];
      for (let i = 0; i < 1500; i++) {
        ids.push(sonic.nextNodeId());
      }

      await sonic.shutdown();
      return { ids };
    }, sonicConfig);

    // All unique
    const uniqueIds = new Set(result.ids);
    expect(uniqueIds.size).toBe(1500);

    // All >= 1000
    for (const id of result.ids) {
      expect(id).toBeGreaterThanOrEqual(1000);
    }

    // Strictly incrementing (even across range boundary)
    for (let i = 1; i < result.ids.length; i++) {
      expect(result.ids[i]).toBe(result.ids[i - 1] + 1);
    }
  });
});

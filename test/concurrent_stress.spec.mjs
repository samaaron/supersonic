/**
 * Concurrent Operations Stress Test Suite
 *
 * Tests for race conditions, nefarious timing, and concurrent operation resilience.
 * These tests deliberately try to break SuperSonic with:
 * - Parallel operations that shouldn't interfere
 * - Rapid-fire operations without waiting
 * - Operations during sensitive state transitions
 * - Interleaved create/destroy cycles
 * - Maximum throughput stress
 */

import { test, expect } from "@playwright/test";

const SONIC_CONFIG = {
  workerBaseURL: "/dist/workers/",
  wasmBaseURL: "/dist/wasm/",
  sampleBaseURL: "/dist/samples/",
  synthdefBaseURL: "/dist/synthdefs/",
};

// =============================================================================
// PARALLEL SAMPLE LOADING
// =============================================================================

test.describe("Parallel Sample Loading", () => {
  test("loads 10 samples in parallel without corruption", async ({ page }) => {
    await page.goto("/test/harness.html");
    await page.waitForFunction(() => window.supersonicReady === true, { timeout: 10000 });

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      const messages = [];
      sonic.on('message', (msg) => messages.push(JSON.parse(JSON.stringify(msg))));

      await sonic.init();

      // Load 10 samples in parallel - all different buffers
      const samples = [
        "bd_haus.flac", "sn_dub.flac", "hat_zap.flac",
        "bd_haus.flac", "sn_dub.flac", "hat_zap.flac",
        "bd_haus.flac", "sn_dub.flac", "hat_zap.flac",
        "bd_haus.flac"
      ];

      const loadPromises = samples.map((sample, i) =>
        sonic.loadSample(i, sample)
      );

      // All should complete without error
      await Promise.all(loadPromises);
      await sonic.sync(1);

      // Query all buffers to verify they loaded correctly
      messages.length = 0;
      for (let i = 0; i < 10; i++) {
        await sonic.send("/b_query", i);
      }
      await sonic.sync(2);

      const bufferInfos = messages.filter(m => m.address === "/b_info");

      return {
        success: true,
        loadedCount: bufferInfos.length,
        allHaveFrames: bufferInfos.every(info => info.args[1] > 0),
        bufferNumbers: bufferInfos.map(info => info.args[0]).sort((a, b) => a - b),
      };
    }, SONIC_CONFIG);

    expect(result.success).toBe(true);
    expect(result.loadedCount).toBe(10);
    expect(result.allHaveFrames).toBe(true);
    expect(result.bufferNumbers).toEqual([0, 1, 2, 3, 4, 5, 6, 7, 8, 9]);
  });

  test("loads same buffer repeatedly in rapid succession", async ({ page }) => {
    await page.goto("/test/harness.html");
    await page.waitForFunction(() => window.supersonicReady === true, { timeout: 10000 });

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      const messages = [];
      sonic.on('message', (msg) => messages.push(JSON.parse(JSON.stringify(msg))));

      await sonic.init();

      // Rapidly replace buffer 0 with different samples
      // This tests buffer replacement race conditions
      const samples = ["bd_haus.flac", "sn_dub.flac", "hat_zap.flac", "bd_haus.flac", "sn_dub.flac"];

      for (const sample of samples) {
        sonic.loadSample(0, sample); // Don't await - fire rapidly
      }

      // Wait for all to complete
      await sonic.sync(1);
      await new Promise(r => setTimeout(r, 200));

      // Query final buffer state
      messages.length = 0;
      await sonic.send("/b_query", 0);
      await sonic.sync(2);

      const bufferInfo = messages.find(m => m.address === "/b_info");

      return {
        success: true,
        hasBuffer: !!bufferInfo,
        bufferNum: bufferInfo?.args[0],
        frames: bufferInfo?.args[1],
        channels: bufferInfo?.args[2],
      };
    }, SONIC_CONFIG);

    expect(result.success).toBe(true);
    expect(result.hasBuffer).toBe(true);
    expect(result.bufferNum).toBe(0);
    expect(result.frames).toBeGreaterThan(0);
  });

  test("parallel sample load while synthdef loading", async ({ page }) => {
    await page.goto("/test/harness.html");
    await page.waitForFunction(() => window.supersonicReady === true, { timeout: 10000 });

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();

      // Fire off sample loads AND synthdef loads simultaneously
      const operations = [
        sonic.loadSample(0, "bd_haus.flac"),
        sonic.loadSynthDef("sonic-pi-beep"),
        sonic.loadSample(1, "sn_dub.flac"),
        sonic.loadSynthDef("sonic-pi-saw"),
        sonic.loadSample(2, "hat_zap.flac"),
      ];

      await Promise.all(operations);
      await sonic.sync(1);

      // Verify both samples and synthdefs are available
      const synthdefsLoaded = sonic.loadedSynthDefs.size;

      // Create a synth using loaded synthdef
      await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 0);
      await sonic.sync(2);

      const tree = sonic.getTree();
      const synthExists = tree.nodes.some(n => n.id === 1000);

      await sonic.send("/n_free", 1000);

      return {
        success: true,
        synthdefsLoaded,
        synthExists,
      };
    }, SONIC_CONFIG);

    expect(result.success).toBe(true);
    expect(result.synthdefsLoaded).toBeGreaterThanOrEqual(2);
    expect(result.synthExists).toBe(true);
  });
});

// =============================================================================
// SYNTH CREATE/DESTROY RACES
// =============================================================================

test.describe("Synth Create/Destroy Races", () => {
  test("rapid create and immediate free - 100 cycles", async ({ page }) => {
    await page.goto("/test/harness.html");
    await page.waitForFunction(() => window.supersonicReady === true, { timeout: 10000 });

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      // Rapidly create and free 100 synths
      for (let i = 0; i < 100; i++) {
        const synthId = 10000 + i;
        sonic.send("/s_new", "sonic-pi-beep", synthId, 0, 0, "release", 0.001);
        sonic.send("/n_free", synthId);
      }

      await sonic.sync(1);
      await new Promise(r => setTimeout(r, 100));

      const tree = sonic.getTree();

      // All synths should be freed - only root group remains
      return {
        success: true,
        nodeCount: tree.nodeCount,
        onlyRootRemains: tree.nodeCount === 1 && tree.nodes[0].id === 0,
      };
    }, SONIC_CONFIG);

    expect(result.success).toBe(true);
    expect(result.onlyRootRemains).toBe(true);
  });

  test("free synth before it's created (reversed order)", async ({ page }) => {
    await page.goto("/test/harness.html");
    await page.waitForFunction(() => window.supersonicReady === true, { timeout: 10000 });

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      // Send free BEFORE create - should handle gracefully
      sonic.send("/n_free", 5000);
      sonic.send("/s_new", "sonic-pi-beep", 5000, 0, 0, "release", 60);

      await sonic.sync(1);
      await new Promise(r => setTimeout(r, 50));

      const tree = sonic.getTree();
      // The synth should exist because create came after free
      const synthExists = tree.nodes.some(n => n.id === 5000);

      // Clean up
      await sonic.send("/n_free", 5000);

      return {
        success: true,
        synthExists,
      };
    }, SONIC_CONFIG);

    expect(result.success).toBe(true);
    // The synth should exist because OSC messages are processed in order
    expect(result.synthExists).toBe(true);
  });

  test("interleaved create/free with overlapping IDs", async ({ page }) => {
    await page.goto("/test/harness.html");
    await page.waitForFunction(() => window.supersonicReady === true, { timeout: 10000 });

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      // Create synths, free some, create with same IDs again
      // This tests ID reuse handling
      sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 0, "release", 60);
      sonic.send("/s_new", "sonic-pi-beep", 1001, 0, 0, "release", 60);
      sonic.send("/s_new", "sonic-pi-beep", 1002, 0, 0, "release", 60);
      sonic.send("/n_free", 1001); // Free middle one
      sonic.send("/s_new", "sonic-pi-beep", 1001, 0, 0, "release", 60); // Reuse ID
      sonic.send("/n_free", 1000);
      sonic.send("/n_free", 1002);
      sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 0, "release", 60); // Reuse ID

      await sonic.sync(1);
      await new Promise(r => setTimeout(r, 50));

      const tree = sonic.getTree();
      const existingIds = tree.nodes.filter(n => !n.isGroup).map(n => n.id).sort();

      // Clean up
      await sonic.send("/n_free", 1000, 1001);

      return {
        success: true,
        existingIds,
        nodeCount: tree.nodeCount,
      };
    }, SONIC_CONFIG);

    expect(result.success).toBe(true);
    // Should have synths 1000 and 1001 remaining
    expect(result.existingIds).toEqual([1000, 1001]);
  });

  test("mass synth creation then mass free", async ({ page }) => {
    await page.goto("/test/harness.html");
    await page.waitForFunction(() => window.supersonicReady === true, { timeout: 10000 });

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      const COUNT = 200;

      // Create 200 synths as fast as possible
      for (let i = 0; i < COUNT; i++) {
        sonic.send("/s_new", "sonic-pi-beep", 10000 + i, 0, 0, "release", 60);
      }

      await sonic.sync(1);
      const treeAfterCreate = sonic.getTree();

      // Free all 200 as fast as possible
      for (let i = 0; i < COUNT; i++) {
        sonic.send("/n_free", 10000 + i);
      }

      await sonic.sync(2);
      await new Promise(r => setTimeout(r, 100));
      const treeAfterFree = sonic.getTree();

      return {
        success: true,
        countAfterCreate: treeAfterCreate.nodeCount,
        countAfterFree: treeAfterFree.nodeCount,
      };
    }, SONIC_CONFIG);

    expect(result.success).toBe(true);
    expect(result.countAfterCreate).toBe(201); // 200 synths + root group
    expect(result.countAfterFree).toBe(1); // Only root group
  });
});

// =============================================================================
// OPERATIONS DURING STATE TRANSITIONS
// =============================================================================

test.describe("Operations During State Transitions", () => {
  test("send OSC while reset() is in progress", async ({ page }) => {
    await page.goto("/test/harness.html");
    await page.waitForFunction(() => window.supersonicReady === true, { timeout: 10000 });

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      // Create some synths
      await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 0);
      await sonic.sync(1);

      // Start reset WITHOUT awaiting
      const resetPromise = sonic.reset();

      // Immediately try to send OSC - this should be handled gracefully
      // These might fail or be ignored, but shouldn't crash
      let errorOccurred = false;
      try {
        sonic.send("/status");
        sonic.send("/s_new", "sonic-pi-beep", 2000, 0, 0);
      } catch (e) {
        errorOccurred = true;
      }

      // Wait for reset to complete
      await resetPromise;

      // After reset, operations should work normally
      await sonic.loadSynthDef("sonic-pi-beep");
      await sonic.send("/s_new", "sonic-pi-beep", 3000, 0, 0);
      await sonic.sync(2);

      const tree = sonic.getTree();

      return {
        success: true,
        errorOccurred,
        initialized: sonic.initialized,
        hasNewSynth: tree.nodes.some(n => n.id === 3000),
      };
    }, SONIC_CONFIG);

    expect(result.success).toBe(true);
    expect(result.initialized).toBe(true);
    expect(result.hasNewSynth).toBe(true);
  });

  test("loadSynthDef during heavy OSC traffic", async ({ page }) => {
    await page.goto("/test/harness.html");
    await page.waitForFunction(() => window.supersonicReady === true, { timeout: 10000 });

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      // Start heavy OSC traffic
      const oscFlood = async () => {
        for (let i = 0; i < 50; i++) {
          sonic.send("/s_new", "sonic-pi-beep", 10000 + i, 0, 0, "release", 0.01);
          sonic.send("/n_free", 10000 + i);
        }
      };

      // Start OSC flood
      const floodPromise = oscFlood();

      // Simultaneously load another synthdef
      const loadPromise = sonic.loadSynthDef("sonic-pi-saw");

      await Promise.all([floodPromise, loadPromise]);
      await sonic.sync(1);

      // Verify both synthdefs work
      await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 0);
      await sonic.send("/s_new", "sonic-pi-saw", 1001, 0, 0);
      await sonic.sync(2);

      const tree = sonic.getTree();

      // Clean up
      await sonic.send("/n_free", 1000, 1001);

      return {
        success: true,
        bothSynthsCreated: tree.nodes.some(n => n.id === 1000) && tree.nodes.some(n => n.id === 1001),
        synthdefsLoaded: sonic.loadedSynthDefs.size,
      };
    }, SONIC_CONFIG);

    expect(result.success).toBe(true);
    expect(result.bothSynthsCreated).toBe(true);
    expect(result.synthdefsLoaded).toBeGreaterThanOrEqual(2);
  });

  test("loadSample during synth playback", async ({ page }) => {
    await page.goto("/test/harness.html");
    await page.waitForFunction(() => window.supersonicReady === true, { timeout: 10000 });

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      // Create synths that will be playing
      for (let i = 0; i < 20; i++) {
        await sonic.send("/s_new", "sonic-pi-beep", 1000 + i, 0, 0, "note", 60 + i, "release", 5);
      }
      await sonic.sync(1);

      // While synths are playing, load samples
      const loadPromises = [
        sonic.loadSample(0, "bd_haus.flac"),
        sonic.loadSample(1, "sn_dub.flac"),
        sonic.loadSample(2, "hat_zap.flac"),
      ];

      await Promise.all(loadPromises);
      await sonic.sync(2);

      // Verify synths still exist and samples loaded
      const tree = sonic.getTree();
      const synthCount = tree.nodes.filter(n => !n.isGroup).length;

      const messages = [];
      sonic.on('message', (msg) => messages.push(msg));
      await sonic.send("/b_query", 0, 1, 2);
      await sonic.sync(3);

      const bufferInfo = messages.find(m => m.address === "/b_info");

      // Clean up
      for (let i = 0; i < 20; i++) {
        sonic.send("/n_free", 1000 + i);
      }

      return {
        success: true,
        synthCount,
        buffersLoaded: !!bufferInfo && bufferInfo.args[1] > 0,
      };
    }, SONIC_CONFIG);

    expect(result.success).toBe(true);
    expect(result.synthCount).toBe(20);
    expect(result.buffersLoaded).toBe(true);
  });
});

// =============================================================================
// TRULY NEFARIOUS EDGE CASES
// =============================================================================

test.describe("Truly Nefarious Edge Cases", () => {
  test("double init() call", async ({ page }) => {
    await page.goto("/test/harness.html");
    await page.waitForFunction(() => window.supersonicReady === true, { timeout: 10000 });

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);

      // First init
      await sonic.init();
      const firstContext = sonic.audioContext;

      // Second init without destroy - what happens?
      let errorOnSecondInit = null;
      try {
        await sonic.init();
      } catch (e) {
        errorOnSecondInit = e.message;
      }

      const secondContext = sonic.audioContext;

      return {
        success: true,
        errorOnSecondInit,
        sameContext: firstContext === secondContext,
        initialized: sonic.initialized,
      };
    }, SONIC_CONFIG);

    expect(result.success).toBe(true);
    expect(result.initialized).toBe(true);
    // Either it should error or handle gracefully
  });

  test("operations after destroy() - should fail gracefully", async ({ page }) => {
    await page.goto("/test/harness.html");
    await page.waitForFunction(() => window.supersonicReady === true, { timeout: 10000 });

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      // Destroy
      await sonic.destroy();

      // Now try various operations - should fail gracefully, not crash
      const errors = [];

      try {
        sonic.send("/status");
      } catch (e) {
        errors.push({ op: "send", error: e.message });
      }

      try {
        await sonic.loadSynthDef("sonic-pi-beep");
      } catch (e) {
        errors.push({ op: "loadSynthDef", error: e.message });
      }

      try {
        await sonic.loadSample(0, "bd_haus.flac");
      } catch (e) {
        errors.push({ op: "loadSample", error: e.message });
      }

      try {
        sonic.getTree();
      } catch (e) {
        errors.push({ op: "getTree", error: e.message });
      }

      return {
        success: true,
        errorCount: errors.length,
        errors,
      };
    }, SONIC_CONFIG);

    expect(result.success).toBe(true);
    // Should have gotten errors but not crashed
  });

  test("extreme node ID values", async ({ page }) => {
    await page.goto("/test/harness.html");
    await page.waitForFunction(() => window.supersonicReady === true, { timeout: 10000 });

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      const extremeIds = [
        0,           // Reserved for root group
        -1,          // Invalid
        -999999,     // Very negative
        2147483647,  // Max int32
        -2147483648, // Min int32
        999999999,   // Large positive
      ];

      const results = [];

      for (const id of extremeIds) {
        try {
          if (id > 0) {
            sonic.send("/s_new", "sonic-pi-beep", id, 0, 0, "release", 0.001);
            await sonic.sync(1);
            sonic.send("/n_free", id);
          }
          results.push({ id, error: null });
        } catch (e) {
          results.push({ id, error: e.message });
        }
      }

      await sonic.sync(99);
      const tree = sonic.getTree();

      return {
        success: true,
        results,
        finalNodeCount: tree.nodeCount,
      };
    }, SONIC_CONFIG);

    expect(result.success).toBe(true);
    expect(result.finalNodeCount).toBe(1); // Only root group
  });

  test("negative buffer numbers", async ({ page }) => {
    await page.goto("/test/harness.html");
    await page.waitForFunction(() => window.supersonicReady === true, { timeout: 10000 });

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();

      const results = [];

      // Try negative buffer numbers
      for (const bufNum of [-1, -100, -2147483648]) {
        try {
          await sonic.loadSample(bufNum, "bd_haus.flac");
          results.push({ bufNum, error: null });
        } catch (e) {
          results.push({ bufNum, error: e.message });
        }
      }

      return {
        success: true,
        results,
      };
    }, SONIC_CONFIG);

    expect(result.success).toBe(true);
    // All should have errored or been handled gracefully
  });

  test("hundreds of parameters in single message", async ({ page }) => {
    await page.goto("/test/harness.html");
    await page.waitForFunction(() => window.supersonicReady === true, { timeout: 10000 });

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      // Create message with 200 fake parameters
      const args = ["sonic-pi-beep", 1000, 0, 0];
      for (let i = 0; i < 200; i++) {
        args.push(`param${i}`, Math.random());
      }

      let error = null;
      try {
        sonic.send("/s_new", ...args);
        await sonic.sync(1);
      } catch (e) {
        error = e.message;
      }

      const tree = sonic.getTree();

      // Clean up if it succeeded
      if (tree.nodes.some(n => n.id === 1000)) {
        sonic.send("/n_free", 1000);
      }

      return {
        success: true,
        error,
        nodeCount: tree.nodeCount,
      };
    }, SONIC_CONFIG);

    expect(result.success).toBe(true);
  });

  test("unicode and special characters in strings", async ({ page }) => {
    await page.goto("/test/harness.html");
    await page.waitForFunction(() => window.supersonicReady === true, { timeout: 10000 });

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      const weirdStrings = [
        "test\x00null",           // Null byte
        "test\nwith\nnewlines",   // Newlines
        "emoji🎵🎸🎹",            // Emoji
        "中文字符",                // Chinese
        "test\\path\\here",       // Backslashes
        "",                       // Empty string
        " ",                      // Just space
      ];

      const results = [];

      for (const str of weirdStrings) {
        try {
          // Try using as synthdef name
          sonic.send("/s_new", str, 1000 + results.length, 0, 0);
          results.push({ str: str.substring(0, 20), error: null });
        } catch (e) {
          results.push({ str: str.substring(0, 20), error: e.message });
        }
      }

      await sonic.sync(1);
      return { success: true, results };
    }, SONIC_CONFIG);

    expect(result.success).toBe(true);
  });

  test("send() with wrong argument types", async ({ page }) => {
    await page.goto("/test/harness.html");
    await page.waitForFunction(() => window.supersonicReady === true, { timeout: 10000 });

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      const badCalls = [
        () => sonic.send(null),
        () => sonic.send(undefined),
        () => sonic.send(123),
        () => sonic.send("/s_new", "sonic-pi-beep", "not-a-number", 0, 0),
        () => sonic.send("/s_new", "sonic-pi-beep", 1000, null, 0),
        () => sonic.send("/s_new", "sonic-pi-beep", 1000, 0, {}),
        () => sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 0, "note", NaN),
        () => sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 0, "note", Infinity),
        () => sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 0, "note", -Infinity),
      ];

      const results = [];
      for (const badCall of badCalls) {
        try {
          badCall();
          results.push({ error: null });
        } catch (e) {
          results.push({ error: e.message });
        }
      }

      await sonic.sync(1);
      return { success: true, results };
    }, SONIC_CONFIG);

    expect(result.success).toBe(true);
  });

  test("sync() with extreme IDs", async ({ page }) => {
    await page.goto("/test/harness.html");
    await page.waitForFunction(() => window.supersonicReady === true, { timeout: 10000 });

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();

      const extremeSyncIds = [0, -1, 2147483647, -2147483648, 0.5, NaN];
      const results = [];

      for (const id of extremeSyncIds) {
        try {
          await Promise.race([
            sonic.sync(id),
            new Promise((_, reject) => setTimeout(() => reject(new Error("timeout")), 1000))
          ]);
          results.push({ id: String(id), error: null });
        } catch (e) {
          results.push({ id: String(id), error: e.message });
        }
      }

      return { success: true, results };
    }, SONIC_CONFIG);

    expect(result.success).toBe(true);
  });

  test("group creation with invalid parent", async ({ page }) => {
    await page.goto("/test/harness.html");
    await page.waitForFunction(() => window.supersonicReady === true, { timeout: 10000 });

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();

      // Try to create group with non-existent parent
      // Note: scsynth may or may not reject these - we're testing it doesn't crash
      const invalidParents = [999, -1, 2147483647];
      const results = [];

      for (const parent of invalidParents) {
        try {
          sonic.send("/g_new", 100 + results.length, 0, parent);
          await sonic.sync(results.length + 1);
          results.push({ parent, error: null });
        } catch (e) {
          results.push({ parent, error: e.message });
        }
      }

      const tree = sonic.getTree();

      // Clean up any groups that were created
      for (let i = 0; i < 3; i++) {
        try { sonic.send("/n_free", 100 + i); } catch (e) {}
      }
      await sonic.sync(99);

      return {
        success: true,
        results,
        finalNodeCount: tree.nodeCount,
        // scsynth may or may not create groups with invalid parents
        // The test passes if it doesn't crash
      };
    }, SONIC_CONFIG);

    expect(result.success).toBe(true);
    // Test passes if we get here without crashing - scsynth behavior varies
  });

  test("circular group reference attempt", async ({ page }) => {
    await page.goto("/test/harness.html");
    await page.waitForFunction(() => window.supersonicReady === true, { timeout: 10000 });

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();

      // Create two groups
      sonic.send("/g_new", 100, 0, 0);
      sonic.send("/g_new", 101, 0, 100);
      await sonic.sync(1);

      // Try to move group 100 into group 101 (which is its child)
      // This would create a cycle
      sonic.send("/g_head", 101, 100);
      await sonic.sync(2);

      const tree = sonic.getTree();

      // Clean up
      sonic.send("/n_free", 100);

      return {
        success: true,
        nodeCount: tree.nodeCount,
        nodes: tree.nodes.map(n => ({ id: n.id, parent: n.parentId })),
      };
    }, SONIC_CONFIG);

    expect(result.success).toBe(true);
    // scsynth should prevent circular references
  });

  test("OSC bundle with nested bundles 10 levels deep", async ({ page }) => {
    await page.goto("/test/harness.html");
    await page.waitForFunction(() => window.supersonicReady === true, { timeout: 10000 });

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      const osc = window.SuperSonic.osc;
      const now = Date.now();

      // Create deeply nested bundle
      let innermost = {
        address: "/s_new",
        args: [
          { type: "s", value: "sonic-pi-beep" },
          { type: "i", value: 1000 },
          { type: "i", value: 0 },
          { type: "i", value: 0 },
        ],
      };

      let current = innermost;
      for (let i = 0; i < 10; i++) {
        current = {
          timeTag: { native: now + 50 },
          packets: [current],
        };
      }

      let error = null;
      try {
        const bundle = osc.encode(current);
        await sonic.sendOSC(bundle);
        await new Promise(r => setTimeout(r, 200));
        await sonic.sync(1);
      } catch (e) {
        error = e.message;
      }

      const tree = sonic.getTree();

      return {
        success: true,
        error,
        nodeCount: tree.nodeCount,
        synthCreated: tree.nodes.some(n => n.id === 1000),
      };
    }, SONIC_CONFIG);

    expect(result.success).toBe(true);
    // Should either work or fail gracefully
  });

  test("rapid AudioContext state manipulation", async ({ page }) => {
    await page.goto("/test/harness.html");
    await page.waitForFunction(() => window.supersonicReady === true, { timeout: 10000 });

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      const ctx = sonic.audioContext;

      // Rapid suspend/resume cycles
      for (let i = 0; i < 5; i++) {
        await ctx.suspend();
        // Try to send OSC while suspended
        sonic.send("/s_new", "sonic-pi-beep", 1000 + i, 0, 0, "release", 0.001);
        await ctx.resume();
        sonic.send("/n_free", 1000 + i);
      }

      await sonic.sync(1);
      await new Promise(r => setTimeout(r, 100));

      const tree = sonic.getTree();

      return {
        success: true,
        finalState: ctx.state,
        nodeCount: tree.nodeCount,
      };
    }, SONIC_CONFIG);

    expect(result.success).toBe(true);
    expect(result.finalState).toBe("running");
    expect(result.nodeCount).toBe(1); // Only root
  });

  test("messages during garbage collection pressure", async ({ page }) => {
    await page.goto("/test/harness.html");
    await page.waitForFunction(() => window.supersonicReady === true, { timeout: 10000 });

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      // Create GC pressure by allocating lots of objects
      const createGCPressure = () => {
        const arrays = [];
        for (let i = 0; i < 1000; i++) {
          arrays.push(new Array(1000).fill(Math.random()));
        }
        return arrays.length;
      };

      // Interleave GC pressure with OSC operations
      for (let i = 0; i < 10; i++) {
        createGCPressure();
        sonic.send("/s_new", "sonic-pi-beep", 10000 + i, 0, 0, "release", 0.001);
        createGCPressure();
        sonic.send("/n_free", 10000 + i);
      }

      await sonic.sync(1);
      await new Promise(r => setTimeout(r, 100));

      const tree = sonic.getTree();

      return {
        success: true,
        nodeCount: tree.nodeCount,
      };
    }, SONIC_CONFIG);

    expect(result.success).toBe(true);
    expect(result.nodeCount).toBe(1);
  });
});

// =============================================================================
// TIMING WINDOW EXPLOITS
// =============================================================================

test.describe("Timing Window Exploits", () => {
  test("send message at exact sync completion", async ({ page }) => {
    await page.goto("/test/harness.html");
    await page.waitForFunction(() => window.supersonicReady === true, { timeout: 10000 });

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      // Send a message, then immediately when sync completes, send another
      sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 0, "release", 60);

      // Chain sync and immediate send
      await sonic.sync(1).then(() => {
        sonic.send("/s_new", "sonic-pi-beep", 1001, 0, 0, "release", 60);
        sonic.send("/n_set", 1000, "note", 72);
      });

      await sonic.sync(2);
      const tree = sonic.getTree();

      // Clean up
      sonic.send("/n_free", 1000, 1001);

      return {
        success: true,
        nodeCount: tree.nodeCount,
        bothSynthsExist: tree.nodes.some(n => n.id === 1000) && tree.nodes.some(n => n.id === 1001),
      };
    }, SONIC_CONFIG);

    expect(result.success).toBe(true);
    expect(result.bothSynthsExist).toBe(true);
  });

  test("timed bundles landing exactly on process() boundary", async ({ page }) => {
    await page.goto("/test/harness.html");
    await page.waitForFunction(() => window.supersonicReady === true, { timeout: 10000 });

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      const osc = window.SuperSonic.osc;

      // Schedule bundles at process() boundaries (128 samples = 2.67ms at 48kHz)
      const processBlockMs = 128 / 48000 * 1000;
      const now = Date.now();

      for (let i = 0; i < 50; i++) {
        const targetTime = now + i * processBlockMs;
        const bundle = osc.encode({
          timeTag: { native: targetTime },
          packets: [{
            address: "/s_new",
            args: [
              { type: "s", value: "sonic-pi-beep" },
              { type: "i", value: 10000 + i },
              { type: "i", value: 0 },
              { type: "i", value: 0 },
              { type: "s", value: "release" },
              { type: "f", value: 0.001 },
            ],
          }],
        });
        await sonic.sendOSC(bundle);
      }

      // Wait for all to execute
      await new Promise(r => setTimeout(r, 500));

      // Now free them all
      for (let i = 0; i < 50; i++) {
        sonic.send("/n_free", 10000 + i);
      }

      await sonic.sync(1);
      await new Promise(r => setTimeout(r, 100));

      const tree = sonic.getTree();

      return {
        success: true,
        finalNodeCount: tree.nodeCount,
      };
    }, SONIC_CONFIG);

    expect(result.success).toBe(true);
    expect(result.finalNodeCount).toBe(1);
  });

  test("race between listener callback and next operation", async ({ page }) => {
    await page.goto("/test/harness.html");
    await page.waitForFunction(() => window.supersonicReady === true, { timeout: 10000 });

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      let operationsInCallback = 0;

      // Set up listener that does operations
      sonic.on('message', (msg) => {
        if (msg.address === "/status.reply") {
          // Do operations inside callback
          sonic.send("/s_new", "sonic-pi-beep", 50000 + operationsInCallback, 0, 0, "release", 0.001);
          sonic.send("/n_free", 50000 + operationsInCallback);
          operationsInCallback++;
        }
      });

      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      // Trigger lots of callbacks
      for (let i = 0; i < 20; i++) {
        sonic.send("/status");
      }

      await sonic.sync(1);
      await new Promise(r => setTimeout(r, 200));

      const tree = sonic.getTree();

      return {
        success: true,
        operationsInCallback,
        finalNodeCount: tree.nodeCount,
      };
    }, SONIC_CONFIG);

    expect(result.success).toBe(true);
    expect(result.operationsInCallback).toBeGreaterThan(0);
    expect(result.finalNodeCount).toBe(1);
  });

  test("promise rejection during batch operations", async ({ page }) => {
    await page.goto("/test/harness.html");
    await page.waitForFunction(() => window.supersonicReady === true, { timeout: 10000 });

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      // Mix successful operations with ones that might fail
      const operations = [];

      // Good operations
      for (let i = 0; i < 10; i++) {
        operations.push(sonic.loadSample(i, "bd_haus.flac"));
      }

      // Bad operation - non-existent file
      operations.push(
        sonic.loadSample(99, "this_file_does_not_exist_at_all.flac").catch(e => ({ error: e.message }))
      );

      // More good operations
      for (let i = 10; i < 20; i++) {
        operations.push(sonic.loadSample(i, "sn_dub.flac"));
      }

      const results = await Promise.allSettled(operations);

      await sonic.sync(1);

      const fulfilled = results.filter(r => r.status === "fulfilled").length;
      const rejected = results.filter(r => r.status === "rejected").length;

      return {
        success: true,
        fulfilled,
        rejected,
        total: results.length,
      };
    }, SONIC_CONFIG);

    expect(result.success).toBe(true);
    // Most should succeed
    expect(result.fulfilled).toBeGreaterThan(15);
  });
});

// =============================================================================
// MEMORY STABILITY
// =============================================================================

test.describe("Memory Stability", () => {
  test("repeated reset cycles don't leak memory", async ({ page }) => {
    test.setTimeout(60000);
    await page.goto("/test/harness.html");
    await page.waitForFunction(() => window.supersonicReady === true, { timeout: 10000 });

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();

      const resetCycles = 10;
      let completedCycles = 0;

      for (let i = 0; i < resetCycles; i++) {
        // Do some work
        await sonic.loadSynthDef("sonic-pi-beep");
        await sonic.loadSample(0, "bd_haus.flac");

        for (let j = 0; j < 50; j++) {
          await sonic.send("/s_new", "sonic-pi-beep", 10000 + j, 0, 0, "release", 0.001);
        }
        await sonic.sync(i * 2 + 1);

        // Reset
        await sonic.reset();
        completedCycles++;
      }

      // After all resets, should still work normally
      await sonic.loadSynthDef("sonic-pi-beep");
      await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 0);
      await sonic.sync(100);

      const tree = sonic.getTree();

      return {
        success: true,
        completedCycles,
        stillWorks: tree.nodes.some(n => n.id === 1000),
      };
    }, SONIC_CONFIG);

    expect(result.success).toBe(true);
    expect(result.completedCycles).toBe(10);
    expect(result.stillWorks).toBe(true);
  });

  test("JS heap doesn't grow unboundedly during synth churn", async ({ page }) => {
    test.setTimeout(60000);
    await page.goto("/test/harness.html");
    await page.waitForFunction(() => window.supersonicReady === true, { timeout: 10000 });

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      // Force GC if available (Chrome only)
      const forceGC = () => {
        if (window.gc) window.gc();
      };

      // Get heap size (Chrome only, returns 0 elsewhere)
      const getHeapSize = () => {
        if (performance.memory) {
          return performance.memory.usedJSHeapSize;
        }
        return 0;
      };

      forceGC();
      await new Promise(r => setTimeout(r, 100));
      const initialHeap = getHeapSize();

      // Run 5 rounds of 1000 synth create/destroy cycles
      const heapSamples = [initialHeap];

      for (let round = 0; round < 5; round++) {
        for (let i = 0; i < 1000; i++) {
          sonic.send("/s_new", "sonic-pi-beep", 10000 + i, 0, 0, "release", 0.001);
          sonic.send("/n_free", 10000 + i);
        }
        await sonic.sync(round + 1);

        forceGC();
        await new Promise(r => setTimeout(r, 100));
        heapSamples.push(getHeapSize());
      }

      // Calculate heap growth
      const finalHeap = heapSamples[heapSamples.length - 1];
      const heapGrowth = finalHeap - initialHeap;
      const heapGrowthMB = heapGrowth / (1024 * 1024);

      return {
        success: true,
        hasMemoryAPI: !!performance.memory,
        initialHeapMB: initialHeap / (1024 * 1024),
        finalHeapMB: finalHeap / (1024 * 1024),
        heapGrowthMB,
        heapSamplesMB: heapSamples.map(h => h / (1024 * 1024)),
      };
    }, SONIC_CONFIG);

    expect(result.success).toBe(true);

    if (result.hasMemoryAPI) {
      // Allow up to 10MB growth for 5000 synth cycles - more indicates leak
      expect(result.heapGrowthMB).toBeLessThan(10);
    }
  });

  test("event listeners are cleaned up after destroy", async ({ page }) => {
    await page.goto("/test/harness.html");
    await page.waitForFunction(() => window.supersonicReady === true, { timeout: 10000 });

    const result = await page.evaluate(async (config) => {
      // Create and destroy multiple instances, checking listener cleanup
      const instances = [];

      for (let i = 0; i < 10; i++) {
        const sonic = new window.SuperSonic(config);

        // Add listeners
        const callbacks = {
          message: () => {},
          ready: () => {},
          shutdown: () => {},
          error: () => {},
        };

        sonic.on('message', callbacks.message);
        sonic.on('ready', callbacks.ready);
        sonic.on('shutdown', callbacks.shutdown);
        sonic.on('error', callbacks.error);

        await sonic.init();
        await sonic.destroy();

        instances.push(sonic);
      }

      // After destroy, instances should not hold references to callbacks
      // This is hard to verify directly, but we can check the instance count
      return {
        success: true,
        instancesCreated: instances.length,
      };
    }, SONIC_CONFIG);

    expect(result.success).toBe(true);
    expect(result.instancesCreated).toBe(10);
  });

  test("message listener accumulation detection", async ({ page }) => {
    await page.goto("/test/harness.html");
    await page.waitForFunction(() => window.supersonicReady === true, { timeout: 10000 });

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();

      // Track how many times our listener fires
      let callCount = 0;
      const listener = () => callCount++;

      // Add the same listener multiple times - should only fire once per event
      // unless there's a bug that duplicates listeners
      sonic.on('message', listener);
      sonic.on('message', listener);
      sonic.on('message', listener);

      // Trigger one message
      sonic.send("/status");
      await sonic.sync(1);

      const callsForOneMessage = callCount;

      // Now test that off() works
      callCount = 0;
      sonic.off('message', listener);
      sonic.send("/status");
      await sonic.sync(2);

      const callsAfterOff = callCount;

      return {
        success: true,
        callsForOneMessage,
        callsAfterOff,
        listenerFiredCorrectly: callsForOneMessage >= 1,
        offWorked: callsAfterOff < callsForOneMessage,
      };
    }, SONIC_CONFIG);

    expect(result.success).toBe(true);
    expect(result.listenerFiredCorrectly).toBe(true);
    expect(result.offWorked).toBe(true);
  });

  test("SharedArrayBuffer not corrupted after heavy use", async ({ page }) => {
    await page.goto("/test/harness.html");
    await page.waitForFunction(() => window.supersonicReady === true, { timeout: 10000 });

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      // Get initial metrics
      const initialMetrics = sonic.getMetrics();
      const initialProcessCount = initialMetrics.workletProcessCount;

      // Heavy operations
      for (let round = 0; round < 10; round++) {
        for (let i = 0; i < 100; i++) {
          sonic.send("/s_new", "sonic-pi-beep", 10000 + i, 0, 0, "release", 0.001);
          sonic.send("/n_free", 10000 + i);
        }
        await sonic.sync(round + 1);
      }

      await new Promise(r => setTimeout(r, 200));

      // Check metrics are still valid and incrementing
      const finalMetrics = sonic.getMetrics();

      // Check buffer constants are still valid (extract numeric values only)
      const bc = sonic.bufferConstants;
      const inBufferSize = bc?.IN_BUFFER_SIZE;
      const messageMagic = bc?.MESSAGE_MAGIC;

      // Check node tree is still readable
      const tree = sonic.getTree();
      const treeValid = tree.nodeCount === 1 && tree.nodes[0].id === 0;

      return {
        success: true,
        processCountIncreased: finalMetrics.workletProcessCount > initialProcessCount,
        inBufferSize,
        messageMagic,
        treeValid,
        finalProcessCount: finalMetrics.workletProcessCount,
      };
    }, SONIC_CONFIG);

    expect(result.success).toBe(true);
    expect(result.processCountIncreased).toBe(true);
    expect(result.inBufferSize).toBeGreaterThan(0);
    expect(result.messageMagic).toBe(0xDEADBEEF);
    expect(result.treeValid).toBe(true);
  });

  test("repeated init/destroy cycles don't leak AudioContexts", async ({ page }) => {
    await page.goto("/test/harness.html");
    await page.waitForFunction(() => window.supersonicReady === true, { timeout: 10000 });

    const result = await page.evaluate(async (config) => {
      const contexts = [];

      for (let i = 0; i < 5; i++) {
        const sonic = new window.SuperSonic(config);
        await sonic.init();

        // Store reference to check it gets closed
        contexts.push(sonic.audioContext);

        await sonic.destroy();
      }

      // Check all contexts are closed
      await new Promise(r => setTimeout(r, 100));
      const closedCount = contexts.filter(ctx => ctx.state === "closed").length;

      return {
        success: true,
        totalContexts: contexts.length,
        closedCount,
        allClosed: closedCount === contexts.length,
      };
    }, SONIC_CONFIG);

    expect(result.success).toBe(true);
    expect(result.allClosed).toBe(true);
  });
});

// =============================================================================
// MEMORY LEAK DETECTION - ATTEMPT TO TRIGGER LEAKS
// =============================================================================

test.describe("Memory Leak Triggers", () => {
  test("abandoned sync promises don't leak", async ({ page }) => {
    await page.goto("/test/harness.html");
    await page.waitForFunction(() => window.supersonicReady === true, { timeout: 10000 });

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();

      const getHeapSize = () => performance.memory?.usedJSHeapSize || 0;
      const forceGC = () => { if (window.gc) window.gc(); };

      forceGC();
      await new Promise(r => setTimeout(r, 50));
      const initialHeap = getHeapSize();

      // Create many sync promises but don't await them - they should eventually resolve
      // and not accumulate
      const abandonedPromises = [];
      for (let i = 0; i < 100; i++) {
        // Send a message and create sync, but don't await
        sonic.send("/status");
        abandonedPromises.push(sonic.sync(i));
      }

      // Wait for them to resolve
      await Promise.all(abandonedPromises);

      forceGC();
      await new Promise(r => setTimeout(r, 50));
      const afterHeap = getHeapSize();

      // Do it again to see if heap grows
      const morePromises = [];
      for (let i = 100; i < 200; i++) {
        sonic.send("/status");
        morePromises.push(sonic.sync(i));
      }
      await Promise.all(morePromises);

      forceGC();
      await new Promise(r => setTimeout(r, 50));
      const finalHeap = getHeapSize();

      return {
        success: true,
        hasMemoryAPI: !!performance.memory,
        initialHeapMB: initialHeap / (1024 * 1024),
        afterHeapMB: afterHeap / (1024 * 1024),
        finalHeapMB: finalHeap / (1024 * 1024),
        growthMB: (finalHeap - initialHeap) / (1024 * 1024),
      };
    }, SONIC_CONFIG);

    expect(result.success).toBe(true);
    if (result.hasMemoryAPI) {
      expect(result.growthMB).toBeLessThan(5);
    }
  });

  test("repeatedly adding/removing same listener doesn't leak", async ({ page }) => {
    await page.goto("/test/harness.html");
    await page.waitForFunction(() => window.supersonicReady === true, { timeout: 10000 });

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();

      const getHeapSize = () => performance.memory?.usedJSHeapSize || 0;
      const forceGC = () => { if (window.gc) window.gc(); };

      forceGC();
      await new Promise(r => setTimeout(r, 50));
      const initialHeap = getHeapSize();

      // Add and remove listener 1000 times
      for (let i = 0; i < 1000; i++) {
        const listener = (msg) => { /* capture nothing */ };
        sonic.on('message', listener);
        sonic.off('message', listener);
      }

      forceGC();
      await new Promise(r => setTimeout(r, 50));
      const finalHeap = getHeapSize();

      return {
        success: true,
        hasMemoryAPI: !!performance.memory,
        growthMB: (finalHeap - initialHeap) / (1024 * 1024),
      };
    }, SONIC_CONFIG);

    expect(result.success).toBe(true);
    if (result.hasMemoryAPI) {
      expect(result.growthMB).toBeLessThan(2);
    }
  });

  test("listener with closure capturing large object doesn't leak after off()", async ({ page }) => {
    await page.goto("/test/harness.html");
    await page.waitForFunction(() => window.supersonicReady === true, { timeout: 10000 });

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();

      const getHeapSize = () => performance.memory?.usedJSHeapSize || 0;
      const forceGC = () => { if (window.gc) window.gc(); };

      forceGC();
      await new Promise(r => setTimeout(r, 50));
      const initialHeap = getHeapSize();

      // Add 100 listeners that each capture a 100KB array
      const listeners = [];
      for (let i = 0; i < 100; i++) {
        const bigArray = new Array(25000).fill(i); // ~100KB per listener
        const listener = () => { bigArray.length; };
        sonic.on('message', listener);
        listeners.push(listener);
      }

      forceGC();
      await new Promise(r => setTimeout(r, 50));
      const withListenersHeap = getHeapSize();

      // Remove all listeners
      for (const listener of listeners) {
        sonic.off('message', listener);
      }
      listeners.length = 0; // Clear our reference too

      forceGC();
      await new Promise(r => setTimeout(r, 100));
      const afterRemovalHeap = getHeapSize();

      const addedMB = (withListenersHeap - initialHeap) / (1024 * 1024);
      const reclaimedMB = (withListenersHeap - afterRemovalHeap) / (1024 * 1024);

      return {
        success: true,
        hasMemoryAPI: !!performance.memory,
        addedMB,
        reclaimedMB,
        percentReclaimed: addedMB > 0 ? (reclaimedMB / addedMB) * 100 : 100,
      };
    }, SONIC_CONFIG);

    expect(result.success).toBe(true);
    if (result.hasMemoryAPI && result.addedMB > 1) {
      // Should reclaim at least 50% of the memory after removing listeners
      expect(result.percentReclaimed).toBeGreaterThan(50);
    }
  });

  test("loadSynthDef repeated calls don't accumulate", async ({ page }) => {
    await page.goto("/test/harness.html");
    await page.waitForFunction(() => window.supersonicReady === true, { timeout: 10000 });

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();

      const getHeapSize = () => performance.memory?.usedJSHeapSize || 0;
      const forceGC = () => { if (window.gc) window.gc(); };

      forceGC();
      await new Promise(r => setTimeout(r, 50));
      const initialHeap = getHeapSize();

      // Load the same synthdef 50 times
      for (let i = 0; i < 50; i++) {
        await sonic.loadSynthDef("sonic-pi-beep");
      }

      forceGC();
      await new Promise(r => setTimeout(r, 50));
      const finalHeap = getHeapSize();

      // Should only be loaded once in the set
      const loadedCount = sonic.loadedSynthDefs.size;

      return {
        success: true,
        hasMemoryAPI: !!performance.memory,
        loadedCount,
        growthMB: (finalHeap - initialHeap) / (1024 * 1024),
      };
    }, SONIC_CONFIG);

    expect(result.success).toBe(true);
    expect(result.loadedCount).toBe(1); // Should only be in set once
    if (result.hasMemoryAPI) {
      expect(result.growthMB).toBeLessThan(5);
    }
  });

  test("loadSample to same buffer repeatedly doesn't accumulate", async ({ page }) => {
    await page.goto("/test/harness.html");
    await page.waitForFunction(() => window.supersonicReady === true, { timeout: 10000 });

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();

      const getHeapSize = () => performance.memory?.usedJSHeapSize || 0;
      const forceGC = () => { if (window.gc) window.gc(); };

      forceGC();
      await new Promise(r => setTimeout(r, 50));
      const initialHeap = getHeapSize();

      // Load to buffer 0 repeatedly - old data should be replaced, not accumulated
      for (let i = 0; i < 20; i++) {
        await sonic.loadSample(0, "bd_haus.flac");
        await sonic.sync(i + 1);
      }

      forceGC();
      await new Promise(r => setTimeout(r, 50));
      const finalHeap = getHeapSize();

      return {
        success: true,
        hasMemoryAPI: !!performance.memory,
        growthMB: (finalHeap - initialHeap) / (1024 * 1024),
      };
    }, SONIC_CONFIG);

    expect(result.success).toBe(true);
    if (result.hasMemoryAPI) {
      // Loading same sample 20 times shouldn't grow heap by more than a few MB
      expect(result.growthMB).toBeLessThan(10);
    }
  });

  test("message event handler memory pressure", async ({ page }) => {
    await page.goto("/test/harness.html");
    await page.waitForFunction(() => window.supersonicReady === true, { timeout: 10000 });

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();

      const getHeapSize = () => performance.memory?.usedJSHeapSize || 0;
      const forceGC = () => { if (window.gc) window.gc(); };

      let messageCount = 0;
      const collectedMessages = [];

      // Listener that stores all messages (potential leak pattern)
      sonic.on('message', (msg) => {
        messageCount++;
        // Storing messages would leak - don't do this in real code
        // collectedMessages.push(msg);
      });

      forceGC();
      await new Promise(r => setTimeout(r, 50));
      const initialHeap = getHeapSize();

      // Generate lots of messages
      await sonic.loadSynthDef("sonic-pi-beep");
      for (let i = 0; i < 500; i++) {
        sonic.send("/status");
      }
      await sonic.sync(1);

      forceGC();
      await new Promise(r => setTimeout(r, 50));
      const finalHeap = getHeapSize();

      return {
        success: true,
        hasMemoryAPI: !!performance.memory,
        messageCount,
        growthMB: (finalHeap - initialHeap) / (1024 * 1024),
      };
    }, SONIC_CONFIG);

    expect(result.success).toBe(true);
    expect(result.messageCount).toBeGreaterThan(0);
    if (result.hasMemoryAPI) {
      expect(result.growthMB).toBeLessThan(5);
    }
  });

  test("OSC message creation doesn't leak", async ({ page }) => {
    await page.goto("/test/harness.html");
    await page.waitForFunction(() => window.supersonicReady === true, { timeout: 10000 });

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      const getHeapSize = () => performance.memory?.usedJSHeapSize || 0;
      const forceGC = () => { if (window.gc) window.gc(); };

      const osc = window.SuperSonic.osc;

      forceGC();
      await new Promise(r => setTimeout(r, 50));
      const initialHeap = getHeapSize();

      // Create and discard many OSC messages without sending
      for (let i = 0; i < 10000; i++) {
        const bundle = osc.encode({
          timeTag: { raw: [0, 1] },
          packets: [{
            address: "/s_new",
            args: [
              { type: "s", value: "sonic-pi-beep" },
              { type: "i", value: i },
              { type: "i", value: 0 },
              { type: "i", value: 0 },
            ],
          }],
        });
        // Don't send - just discard
      }

      forceGC();
      await new Promise(r => setTimeout(r, 100));
      const finalHeap = getHeapSize();

      return {
        success: true,
        hasMemoryAPI: !!performance.memory,
        growthMB: (finalHeap - initialHeap) / (1024 * 1024),
      };
    }, SONIC_CONFIG);

    expect(result.success).toBe(true);
    if (result.hasMemoryAPI) {
      // Creating 10000 OSC messages and discarding shouldn't leak
      expect(result.growthMB).toBeLessThan(5);
    }
  });
});

// =============================================================================
// EVIL FLOAT VALUES (Public API Only)
// =============================================================================

test.describe("Evil Float Values", () => {
  test("NaN in synth parameters doesn't crash", async ({ page }) => {
    await page.goto("/test/harness.html");
    await page.waitForFunction(() => window.supersonicReady === true, { timeout: 10000 });

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");
      await sonic.sync();

      // Try to set freq to NaN
      await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 0, "freq", NaN, "amp", 0.1);
      await sonic.sync();

      // Try to set amp to NaN
      await sonic.send("/s_new", "sonic-pi-beep", 1001, 0, 0, "freq", 440, "amp", NaN);
      await sonic.sync();

      // Try n_set with NaN
      await sonic.send("/n_set", 1000, "freq", NaN);
      await sonic.sync();

      // System should still respond
      const tree = sonic.getTree();
      await sonic.destroy();

      return { success: true, nodeCount: tree.nodeCount };
    }, SONIC_CONFIG);

    expect(result.success).toBe(true);
  });

  test("Infinity in synth parameters doesn't crash", async ({ page }) => {
    await page.goto("/test/harness.html");
    await page.waitForFunction(() => window.supersonicReady === true, { timeout: 10000 });

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");
      await sonic.sync();

      // Positive infinity
      await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 0, "freq", Infinity, "amp", 0.1);
      await sonic.sync();

      // Negative infinity
      await sonic.send("/s_new", "sonic-pi-beep", 1001, 0, 0, "freq", -Infinity, "amp", 0.1);
      await sonic.sync();

      // Both infinities
      await sonic.send("/s_new", "sonic-pi-beep", 1002, 0, 0, "freq", Infinity, "amp", -Infinity);
      await sonic.sync();

      await sonic.destroy();
      return { success: true };
    }, SONIC_CONFIG);

    expect(result.success).toBe(true);
  });

  test("subnormal floats don't cause issues", async ({ page }) => {
    await page.goto("/test/harness.html");
    await page.waitForFunction(() => window.supersonicReady === true, { timeout: 10000 });

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");
      await sonic.sync();

      // Subnormal (denormalized) floats - very small numbers
      const subnormal = 1e-40;
      await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 0, "freq", subnormal, "amp", subnormal);
      await sonic.sync();

      // Negative subnormal
      await sonic.send("/s_new", "sonic-pi-beep", 1001, 0, 0, "freq", -subnormal, "amp", subnormal);
      await sonic.sync();

      await sonic.destroy();
      return { success: true };
    }, SONIC_CONFIG);

    expect(result.success).toBe(true);
  });

  test("extreme float values don't crash", async ({ page }) => {
    await page.goto("/test/harness.html");
    await page.waitForFunction(() => window.supersonicReady === true, { timeout: 10000 });

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");
      await sonic.sync();

      const extremes = [
        Number.MAX_VALUE,
        Number.MIN_VALUE,
        Number.MAX_SAFE_INTEGER,
        Number.MIN_SAFE_INTEGER,
        -Number.MAX_VALUE,
        1.7976931348623157e+308,  // Near max
        5e-324,  // Min positive
      ];

      for (let i = 0; i < extremes.length; i++) {
        await sonic.send("/s_new", "sonic-pi-beep", 1000 + i, 0, 0, "freq", extremes[i], "amp", 0.1);
      }
      await sonic.sync();

      await sonic.destroy();
      return { success: true };
    }, SONIC_CONFIG);

    expect(result.success).toBe(true);
  });
});

// =============================================================================
// RAPID INIT/DESTROY CYCLES (Public API Only)
// =============================================================================

test.describe("Rapid Lifecycle Abuse", () => {
  test("10 rapid init/destroy cycles", async ({ page }) => {
    await page.goto("/test/harness.html");
    await page.waitForFunction(() => window.supersonicReady === true, { timeout: 10000 });

    const result = await page.evaluate(async (config) => {
      for (let i = 0; i < 10; i++) {
        const sonic = new window.SuperSonic(config);
        await sonic.init();
        await sonic.destroy();
      }
      return { success: true, cycles: 10 };
    }, SONIC_CONFIG);

    expect(result.success).toBe(true);
    expect(result.cycles).toBe(10);
  });

  test("init/destroy with operations in between", async ({ page }) => {
    await page.goto("/test/harness.html");
    await page.waitForFunction(() => window.supersonicReady === true, { timeout: 10000 });

    const result = await page.evaluate(async (config) => {
      for (let i = 0; i < 5; i++) {
        const sonic = new window.SuperSonic(config);
        await sonic.init();
        await sonic.loadSynthDef("sonic-pi-beep");
        await sonic.sync();
        await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 0);
        await sonic.sync();
        await sonic.send("/n_free", 1000);
        await sonic.destroy();
      }
      return { success: true };
    }, SONIC_CONFIG);

    expect(result.success).toBe(true);
  });

  test("reset() called 20 times rapidly", async ({ page }) => {
    await page.goto("/test/harness.html");
    await page.waitForFunction(() => window.supersonicReady === true, { timeout: 10000 });

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();

      for (let i = 0; i < 20; i++) {
        await sonic.reset();
      }

      // Should still work
      await sonic.loadSynthDef("sonic-pi-beep");
      await sonic.sync();

      await sonic.destroy();
      return { success: true };
    }, SONIC_CONFIG);

    expect(result.success).toBe(true);
  });
});

// =============================================================================
// PROMISE FLOODING (Public API Only)
// =============================================================================

test.describe("Promise Flooding", () => {
  test("1000 sync() calls without await", async ({ page }) => {
    await page.goto("/test/harness.html");
    await page.waitForFunction(() => window.supersonicReady === true, { timeout: 10000 });

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");
      await sonic.sync();

      // Fire off 1000 sync() calls without awaiting
      const promises = [];
      for (let i = 0; i < 1000; i++) {
        promises.push(sonic.sync(i).catch(() => null));  // Catch any timeouts
      }

      // Wait for all to settle
      const results = await Promise.allSettled(promises);
      const fulfilled = results.filter(r => r.status === "fulfilled").length;

      await sonic.destroy();
      return { success: true, fulfilled, total: 1000 };
    }, SONIC_CONFIG);

    expect(result.success).toBe(true);
    // At least some should succeed (server might drop some under load)
    expect(result.fulfilled).toBeGreaterThan(0);
  });

  test("1000 send() calls without await", async ({ page }) => {
    await page.goto("/test/harness.html");
    await page.waitForFunction(() => window.supersonicReady === true, { timeout: 10000 });

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");
      await sonic.sync();

      // Fire off 1000 sends without awaiting
      const promises = [];
      for (let i = 0; i < 1000; i++) {
        promises.push(sonic.send("/s_new", "sonic-pi-beep", 2000 + i, 0, 0));
      }

      // Wait for all
      await Promise.all(promises);
      await sonic.sync();

      const tree = sonic.getTree();
      await sonic.destroy();

      return { success: true, nodeCount: tree.nodeCount };
    }, SONIC_CONFIG);

    expect(result.success).toBe(true);
  });

  test("loadSample() and loadSynthDef() called 100 times each without await", async ({ page }) => {
    await page.goto("/test/harness.html");
    await page.waitForFunction(() => window.supersonicReady === true, { timeout: 10000 });

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();

      const samplePromises = [];
      const synthdefPromises = [];

      // Fire off loads without awaiting
      for (let i = 0; i < 100; i++) {
        samplePromises.push(sonic.loadSample(i % 10, "bd_haus.flac").catch(() => null));
        synthdefPromises.push(sonic.loadSynthDef("sonic-pi-beep").catch(() => null));
      }

      // Wait for all to settle
      await Promise.allSettled([...samplePromises, ...synthdefPromises]);
      await sonic.sync();

      await sonic.destroy();
      return { success: true };
    }, SONIC_CONFIG);

    expect(result.success).toBe(true);
  });
});


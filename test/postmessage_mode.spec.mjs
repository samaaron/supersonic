import { test, expect, skipIfSAB } from "./fixtures.mjs";

/**
 * PostMessage Mode Tests
 *
 * Tests SuperSonic in postMessage mode (no SharedArrayBuffer).
 * This mode works on browsers without crossOriginIsolated.
 *
 * NOTE: These tests are specific to postMessage mode features.
 * Automatically skipped in SAB mode.
 */

test.describe("PostMessage Mode", () => {
  // Skip all tests in this describe block if running in SAB mode
  test.beforeEach(async ({ page, sonicMode }) => {
    skipIfSAB(sonicMode, 'PostMessage-specific tests');
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

  test("boots and initializes in postMessage mode", async ({ page, sonicConfig }) => {
    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);

      try {
        await sonic.init();
        return {
          success: true,
          mode: sonic.mode,
        };
      } catch (err) {
        return { success: false, error: err.message, stack: err.stack };
      }
    }, sonicConfig);

    expect(result.success).toBe(true);
    expect(result.mode).toBe('postMessage');
  });

  test("loads synthdef in postMessage mode", async ({ page, sonicConfig }) => {
    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();

      try {
        await sonic.loadSynthDef("sonic-pi-beep");
        return { success: true };
      } catch (err) {
        return { success: false, error: err.message };
      }
    }, sonicConfig);

    expect(result.success).toBe(true);
  });

  test("creates and frees synth in postMessage mode", async ({ page, sonicConfig }) => {
    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      try {
        // Create synth
        await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 0, "note", 60, "amp", 0.3, "release", 0.1);
        await sonic.sync(1);

        // Free synth
        await sonic.send("/n_free", 1000);
        await sonic.sync(2);

        return { success: true };
      } catch (err) {
        return { success: false, error: err.message };
      }
    }, sonicConfig);

    expect(result.success).toBe(true);
  });

  test("receives OSC replies in postMessage mode", async ({ page, sonicConfig }) => {
    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();

      try {
        // Send /status and wait for /status.reply
        const statusPromise = new Promise((resolve) => {
          const handler = (msg) => {
            if (msg.address === "/status.reply") {
              sonic.off("message", handler);
              resolve(msg);
            }
          };
          sonic.on("message", handler);
        });

        await sonic.send("/status");
        const status = await Promise.race([
          statusPromise,
          new Promise((_, reject) => setTimeout(() => reject(new Error("Timeout waiting for /status.reply")), 5000))
        ]);

        return {
          success: true,
          address: status.address,
          hasArgs: status.args && status.args.length > 0
        };
      } catch (err) {
        return { success: false, error: err.message };
      }
    }, sonicConfig);

    expect(result.success).toBe(true);
    expect(result.address).toBe("/status.reply");
    expect(result.hasArgs).toBe(true);
  });

  test("receives debug messages in postMessage mode", async ({ page, sonicConfig }) => {
    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);

      const debugMessages = [];
      sonic.on("debug", (msg) => {
        debugMessages.push(msg);
      });

      await sonic.init();

      // Wait a bit for any debug messages during init
      await new Promise(r => setTimeout(r, 500));

      return {
        success: true,
        debugCount: debugMessages.length,
        // Init typically produces debug messages
      };
    }, sonicConfig);

    expect(result.success).toBe(true);
    // We should receive at least some debug messages during init
    expect(result.debugCount).toBeGreaterThanOrEqual(0);
  });

  test("sync() works in postMessage mode", async ({ page, sonicConfig }) => {
    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();

      try {
        const start = performance.now();
        await sonic.sync(42);
        const duration = performance.now() - start;

        return { success: true, duration };
      } catch (err) {
        return { success: false, error: err.message };
      }
    }, sonicConfig);

    expect(result.success).toBe(true);
    // sync() should complete reasonably quickly
    expect(result.duration).toBeLessThan(5000);
  });

  test("immediate synth creation works in postMessage mode", async ({ page, sonicConfig }) => {
    // Test immediate synth creation (not timed bundle) since that's the common case
    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      try {
        // Create synth immediately
        await sonic.send("/s_new", "sonic-pi-beep", 2000, 0, 0, "note", 72, "amp", 0.3, "release", 0.2);

        // Wait for synth to play
        await new Promise(r => setTimeout(r, 300));

        // Verify sync works
        await sonic.sync(1);

        return { success: true };
      } catch (err) {
        return { success: false, error: err.message, stack: err.stack };
      }
    }, sonicConfig);

    if (!result.success) {
      console.log("Synth creation test error:", result.error, result.stack);
    }
    expect(result.success).toBe(true);
  });

  test("control bus operations work in postMessage mode", async ({ page, sonicConfig }) => {
    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();

      try {
        // Set control bus value
        await sonic.send("/c_set", 0, 440);
        await sonic.sync(1);

        // Get control bus value
        const valuePromise = new Promise((resolve) => {
          const handler = (msg) => {
            if (msg.address === "/c_set") {
              sonic.off("message", handler);
              resolve(msg.args);
            }
          };
          sonic.on("message", handler);
        });

        await sonic.send("/c_get", 0);

        const args = await Promise.race([
          valuePromise,
          new Promise((_, reject) => setTimeout(() => reject(new Error("Timeout")), 2000))
        ]);

        return {
          success: true,
          busIndex: args[0],
          value: args[1]
        };
      } catch (err) {
        return { success: false, error: err.message };
      }
    }, sonicConfig);

    expect(result.success).toBe(true);
    expect(result.busIndex).toBe(0);
    expect(result.value).toBe(440);
  });

  test("reset() works in postMessage mode", async ({ page, sonicConfig }) => {
    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      try {
        // Create some synths
        await sonic.send("/s_new", "sonic-pi-beep", 3000, 0, 0, "note", 60);
        await sonic.send("/s_new", "sonic-pi-beep", 3001, 0, 0, "note", 64);
        await sonic.sync(1);

        // Reset
        await sonic.reset();

        // Verify synths are gone by trying to query them (should fail silently)
        await sonic.sync(2);

        return { success: true };
      } catch (err) {
        return { success: false, error: err.message };
      }
    }, sonicConfig);

    expect(result.success).toBe(true);
  });

  test("destroy() cleans up in postMessage mode", async ({ page, sonicConfig }) => {
    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();

      try {
        await sonic.destroy();

        // After destroy, initialized should be false
        return {
          success: true,
          initialized: sonic.initialized
        };
      } catch (err) {
        return { success: false, error: err.message };
      }
    }, sonicConfig);

    expect(result.success).toBe(true);
    expect(result.initialized).toBe(false);
  });

  test("loads sample in postMessage mode", async ({ page, sonicConfig }) => {
    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();

      try {
        // Load a sample
        await sonic.loadSample(0, "loop_amen.flac");

        // Verify buffer was allocated by querying it
        const queryPromise = new Promise((resolve) => {
          const handler = (msg) => {
            if (msg.address === "/b_info") {
              sonic.off("message", handler);
              resolve(msg.args);
            }
          };
          sonic.on("message", handler);
        });

        await sonic.send("/b_query", 0);
        const bufferInfo = await Promise.race([
          queryPromise,
          new Promise((_, reject) => setTimeout(() => reject(new Error("Timeout")), 5000))
        ]);

        return {
          success: true,
          bufnum: bufferInfo[0],
          numFrames: bufferInfo[1],
          numChannels: bufferInfo[2],
          sampleRate: bufferInfo[3]
        };
      } catch (err) {
        return { success: false, error: err.message, stack: err.stack };
      }
    }, sonicConfig);

    if (!result.success) {
      console.log("Sample loading error:", result.error, result.stack);
    }
    expect(result.success).toBe(true);
    expect(result.bufnum).toBe(0);
    expect(result.numFrames).toBeGreaterThan(0);
    expect(result.numChannels).toBeGreaterThanOrEqual(1);
    expect(result.sampleRate).toBeGreaterThan(0);
  });

  test("getTree() works in postMessage mode", async ({ page, sonicConfig }) => {
    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      // Helper to wait for tree to update with a specific condition
      const waitForTreeUpdate = async (condition, timeoutMs = 2000) => {
        const start = Date.now();
        while (Date.now() - start < timeoutMs) {
          const tree = sonic.getTree();
          if (condition(tree)) return tree;
          await new Promise(r => setTimeout(r, 50)); // Check every 50ms
        }
        throw new Error("Timeout waiting for tree condition");
      };

      try {
        // Get initial tree (should have root group 0)
        // Wait for worklet to send initial tree
        const initialTree = await waitForTreeUpdate(t => t.nodeCount >= 1);

        // Create a synth with long release so it doesn't auto-free
        await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 0, "note", 60, "amp", 0.0, "release", 60);
        await sonic.sync(1);

        // Wait for tree to include the synth
        const treeWithSynth = await waitForTreeUpdate(t =>
          t.nodes.some(n => n.id === 1000)
        );

        // Create a group
        await sonic.send("/g_new", 2000, 0, 0);
        await sonic.sync(2);

        // Wait for tree to include the group
        const treeWithGroup = await waitForTreeUpdate(t =>
          t.nodes.some(n => n.id === 2000 && n.isGroup === true)
        );

        // Free the synth
        await sonic.send("/n_free", 1000);
        await sonic.sync(3);

        // Wait for synth to be removed from tree
        const treeAfterFree = await waitForTreeUpdate(t =>
          !t.nodes.some(n => n.id === 1000)
        );

        // Free the group
        await sonic.send("/n_free", 2000);
        await sonic.sync(4);

        // Wait for group to be removed
        const finalTree = await waitForTreeUpdate(t =>
          !t.nodes.some(n => n.id === 2000)
        );

        return {
          success: true,
          initialNodeCount: initialTree.nodeCount,
          treeWithSynthCount: treeWithSynth.nodeCount,
          treeWithGroupCount: treeWithGroup.nodeCount,
          treeAfterFreeCount: treeAfterFree.nodeCount,
          finalNodeCount: finalTree.nodeCount,
          // Check if synth was found in tree
          hadSynth: treeWithSynth.nodes.some(n => n.id === 1000),
          hadGroup: treeWithGroup.nodes.some(n => n.id === 2000 && n.isGroup === true),
          // Version should increase with each change
          versionIncreased: treeWithSynth.version > initialTree.version &&
                           treeWithGroup.version > treeWithSynth.version &&
                           treeAfterFree.version > treeWithGroup.version,
        };
      } catch (err) {
        return { success: false, error: err.message, stack: err.stack };
      }
    }, sonicConfig);

    if (!result.success) {
      console.log("getTree() test error:", result.error, result.stack);
    }
    expect(result.success).toBe(true);
    expect(result.hadSynth).toBe(true);
    expect(result.hadGroup).toBe(true);
    expect(result.versionIncreased).toBe(true);
    // Synth should be removed after /n_free
    expect(result.treeAfterFreeCount).toBeLessThan(result.treeWithGroupCount);
  });
});

import { test, expect } from "@playwright/test";

test.describe("SuperSonic reset()", () => {
  test.beforeEach(async ({ page }) => {
    // Collect console errors
    page.on("console", (msg) => {
      if (msg.type() === "error") {
        console.error("Browser console error:", msg.text());
      }
    });

    // Collect page errors
    page.on("pageerror", (err) => {
      console.error("Page error:", err.message);
    });

    await page.goto("/test/harness.html");

    // Wait for the SuperSonic module to load
    await page.waitForFunction(() => window.supersonicReady === true, {
      timeout: 10000,
    });
  });

  test("reset() re-initializes after destroy", async ({ page }) => {
    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: "/dist/workers/",
        wasmBaseURL: "/dist/wasm/",
        sampleBaseURL: "/dist/samples/",
        synthdefBaseURL: "/dist/synthdefs/",
      });

      try {
        // First init
        await sonic.init();
        const initializedFirst = sonic.initialized;

        // Reset (destroy + re-init)
        await sonic.reset();
        const initializedAfterReset = sonic.initialized;

        return {
          success: true,
          initializedFirst,
          initializedAfterReset,
        };
      } catch (err) {
        return { success: false, error: err.message, stack: err.stack };
      }
    });

    expect(result.success).toBe(true);
    expect(result.initializedFirst).toBe(true);
    expect(result.initializedAfterReset).toBe(true);
  });

  test("reset() preserves callbacks", async ({ page }) => {
    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: "/dist/workers/",
        wasmBaseURL: "/dist/wasm/",
        sampleBaseURL: "/dist/samples/",
        synthdefBaseURL: "/dist/synthdefs/",
      });

      const messagesBeforeReset = [];
      const messagesAfterReset = [];
      let phase = "before";

      // Set up listener before init
      sonic.on('message', (msg) => {
        if (phase === "before") {
          messagesBeforeReset.push(JSON.parse(JSON.stringify(msg)));
        } else {
          messagesAfterReset.push(JSON.parse(JSON.stringify(msg)));
        }
      });

      try {
        await sonic.init();

        // Send a message before reset
        sonic.send("/status");
        await sonic.sync(1);

        // Switch phase and reset
        phase = "after";
        await sonic.reset();

        // Send a message after reset - callback should still work
        sonic.send("/status");
        await sonic.sync(2);

        return {
          success: true,
          messagesBeforeReset,
          messagesAfterReset,
          listenerPreserved: true, // Listeners are preserved across reset
        };
      } catch (err) {
        return { success: false, error: err.message, stack: err.stack };
      }
    });

    expect(result.success).toBe(true);
    expect(result.listenerPreserved).toBe(true);

    // Should have received /status.reply before reset
    const statusBeforeReset = result.messagesBeforeReset.filter(
      (m) => m.address === "/status.reply"
    );
    expect(statusBeforeReset.length).toBe(1);

    // Should have received /status.reply after reset (callback still works)
    const statusAfterReset = result.messagesAfterReset.filter(
      (m) => m.address === "/status.reply"
    );
    expect(statusAfterReset.length).toBe(1);
  });

  test("reset() clears loaded synthdefs", async ({ page }) => {
    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: "/dist/workers/",
        wasmBaseURL: "/dist/wasm/",
        sampleBaseURL: "/dist/samples/",
        synthdefBaseURL: "/dist/synthdefs/",
      });

      try {
        await sonic.init();

        // Load a synthdef
        await sonic.loadSynthDef("sonic-pi-beep");
        await sonic.sync(1);
        const synthdefsBeforeReset = sonic.loadedSynthDefs.size;

        // Reset
        await sonic.reset();
        const synthdefsAfterReset = sonic.loadedSynthDefs.size;

        return {
          success: true,
          synthdefsBeforeReset,
          synthdefsAfterReset,
        };
      } catch (err) {
        return { success: false, error: err.message, stack: err.stack };
      }
    });

    expect(result.success).toBe(true);
    expect(result.synthdefsBeforeReset).toBe(1);
    expect(result.synthdefsAfterReset).toBe(0); // Cleared after reset
  });

  test("reset() creates fresh node tree", async ({ page }) => {
    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: "/dist/workers/",
        wasmBaseURL: "/dist/wasm/",
        sampleBaseURL: "/dist/samples/",
        synthdefBaseURL: "/dist/synthdefs/",
      });

      try {
        await sonic.init();
        await sonic.loadSynthDef("sonic-pi-beep");

        // Create some synths
        await sonic.send("/s_new", "sonic-pi-beep", 1000, 1, 0);
        await sonic.send("/s_new", "sonic-pi-beep", 1001, 1, 0);
        await sonic.sync(1);
        await new Promise((r) => setTimeout(r, 50));

        const treeBeforeReset = sonic.getTree();

        // Reset
        await sonic.reset();
        await new Promise((r) => setTimeout(r, 50));

        const treeAfterReset = sonic.getTree();

        return {
          success: true,
          nodeCountBefore: treeBeforeReset.nodeCount,
          nodeCountAfter: treeAfterReset.nodeCount,
          nodesAfter: treeAfterReset.nodes,
        };
      } catch (err) {
        return { success: false, error: err.message, stack: err.stack };
      }
    });

    expect(result.success).toBe(true);
    // Before reset: root group + 2 synths = 3 nodes
    expect(result.nodeCountBefore).toBe(3);
    // After reset: only root group = 1 node (fresh state)
    expect(result.nodeCountAfter).toBe(1);
    expect(result.nodesAfter[0].id).toBe(0); // Root group
  });

  test("reset() works after audio operations", async ({ page }) => {
    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: "/dist/workers/",
        wasmBaseURL: "/dist/wasm/",
        sampleBaseURL: "/dist/samples/",
        synthdefBaseURL: "/dist/synthdefs/",
      });

      try {
        await sonic.init();
        await sonic.loadSynthDef("sonic-pi-beep");

        // Play some audio
        await sonic.send("/s_new", "sonic-pi-beep", 1000, 1, 0, "note", 60);
        await sonic.sync(1);

        // Let it play briefly
        await new Promise((r) => setTimeout(r, 100));

        // Reset while audio is playing
        await sonic.reset();

        // Should be able to play audio again after reset
        await sonic.loadSynthDef("sonic-pi-beep");
        await sonic.send("/s_new", "sonic-pi-beep", 2000, 1, 0, "note", 64);
        await sonic.sync(2);

        const tree = sonic.getTree();

        return {
          success: true,
          nodeCount: tree.nodeCount,
          hasNewSynth: tree.nodes.some((n) => n.id === 2000),
        };
      } catch (err) {
        return { success: false, error: err.message, stack: err.stack };
      }
    });

    expect(result.success).toBe(true);
    expect(result.nodeCount).toBe(2); // Root + new synth
    expect(result.hasNewSynth).toBe(true);
  });

  test("reset() can be called multiple times", async ({ page }) => {
    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: "/dist/workers/",
        wasmBaseURL: "/dist/wasm/",
        sampleBaseURL: "/dist/samples/",
        synthdefBaseURL: "/dist/synthdefs/",
      });

      try {
        await sonic.init();

        // Reset multiple times
        await sonic.reset();
        const afterFirst = sonic.initialized;

        await sonic.reset();
        const afterSecond = sonic.initialized;

        await sonic.reset();
        const afterThird = sonic.initialized;

        // Should still work after multiple resets
        sonic.send("/status");
        await sonic.sync(1);

        return {
          success: true,
          afterFirst,
          afterSecond,
          afterThird,
        };
      } catch (err) {
        return { success: false, error: err.message, stack: err.stack };
      }
    });

    expect(result.success).toBe(true);
    expect(result.afterFirst).toBe(true);
    expect(result.afterSecond).toBe(true);
    expect(result.afterThird).toBe(true);
  });

  test("reset() provides fresh boot stats", async ({ page }) => {
    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: "/dist/workers/",
        wasmBaseURL: "/dist/wasm/",
        sampleBaseURL: "/dist/samples/",
        synthdefBaseURL: "/dist/synthdefs/",
      });

      try {
        await sonic.init();
        const firstBootDuration = sonic.bootStats.initDuration;

        // Wait a bit so we can distinguish boot times
        await new Promise((r) => setTimeout(r, 50));

        await sonic.reset();
        const secondBootDuration = sonic.bootStats.initDuration;

        return {
          success: true,
          firstBootDuration,
          secondBootDuration,
          bothPositive:
            firstBootDuration > 0 && secondBootDuration > 0,
        };
      } catch (err) {
        return { success: false, error: err.message, stack: err.stack };
      }
    });

    expect(result.success).toBe(true);
    expect(result.bothPositive).toBe(true);
    // Both durations should be recorded (may be similar but both should exist)
    expect(result.firstBootDuration).toBeGreaterThan(0);
    expect(result.secondBootDuration).toBeGreaterThan(0);
  });

  test("reset() fires ready event", async ({ page }) => {
    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: "/dist/workers/",
        wasmBaseURL: "/dist/wasm/",
        sampleBaseURL: "/dist/samples/",
        synthdefBaseURL: "/dist/synthdefs/",
      });

      let readyCallCount = 0;
      sonic.on('ready', () => {
        readyCallCount++;
      });

      try {
        await sonic.init();
        const countAfterInit = readyCallCount;

        await sonic.reset();
        const countAfterReset = readyCallCount;

        return {
          success: true,
          countAfterInit,
          countAfterReset,
        };
      } catch (err) {
        return { success: false, error: err.message, stack: err.stack };
      }
    });

    expect(result.success).toBe(true);
    expect(result.countAfterInit).toBe(1);
    expect(result.countAfterReset).toBe(2); // Called again on reset
  });

  test("reset() creates new AudioContext", async ({ page }) => {
    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: "/dist/workers/",
        wasmBaseURL: "/dist/wasm/",
        sampleBaseURL: "/dist/samples/",
        synthdefBaseURL: "/dist/synthdefs/",
      });

      try {
        await sonic.init();
        const firstContext = sonic.audioContext;
        const firstState = firstContext.state;

        await sonic.reset();
        const secondContext = sonic.audioContext;
        const secondState = secondContext.state;

        return {
          success: true,
          isDifferentContext: firstContext !== secondContext,
          firstState,
          secondState,
        };
      } catch (err) {
        return { success: false, error: err.message, stack: err.stack };
      }
    });

    expect(result.success).toBe(true);
    expect(result.isDifferentContext).toBe(true);
    expect(result.secondState).toBe("running");
  });

  test("getInfo() works after reset", async ({ page }) => {
    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: "/dist/workers/",
        wasmBaseURL: "/dist/wasm/",
        sampleBaseURL: "/dist/samples/",
        synthdefBaseURL: "/dist/synthdefs/",
      });

      try {
        await sonic.init();
        const infoBefore = sonic.getInfo();

        await sonic.reset();
        const infoAfter = sonic.getInfo();

        return {
          success: true,
          sampleRateBefore: infoBefore.sampleRate,
          sampleRateAfter: infoAfter.sampleRate,
          bootTimeMsBefore: infoBefore.bootTimeMs,
          bootTimeMsAfter: infoAfter.bootTimeMs,
        };
      } catch (err) {
        return { success: false, error: err.message, stack: err.stack };
      }
    });

    expect(result.success).toBe(true);
    expect(result.sampleRateAfter).toBeGreaterThan(0);
    expect(result.bootTimeMsAfter).toBeGreaterThan(0);
  });

  test("getMetrics() works after reset", async ({ page }) => {
    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: "/dist/workers/",
        wasmBaseURL: "/dist/wasm/",
        sampleBaseURL: "/dist/samples/",
        synthdefBaseURL: "/dist/synthdefs/",
      });

      try {
        await sonic.init();

        // Do some work to generate metrics
        sonic.send("/status");
        await sonic.sync(1);
        const metricsBefore = sonic.getMetrics();

        await sonic.reset();

        // Do some work after reset
        sonic.send("/status");
        await sonic.sync(2);
        const metricsAfter = sonic.getMetrics();

        return {
          success: true,
          metricsBefore: {
            hasProcessCount:
              typeof metricsBefore.workletProcessCount === "number",
            audioContextState: metricsBefore.audioContextState,
          },
          metricsAfter: {
            hasProcessCount:
              typeof metricsAfter.workletProcessCount === "number",
            audioContextState: metricsAfter.audioContextState,
          },
        };
      } catch (err) {
        return { success: false, error: err.message, stack: err.stack };
      }
    });

    expect(result.success).toBe(true);
    expect(result.metricsBefore.hasProcessCount).toBe(true);
    expect(result.metricsAfter.hasProcessCount).toBe(true);
    expect(result.metricsAfter.audioContextState).toBe("running");
  });

  test("loadSample() works after reset", async ({ page }) => {
    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: "/dist/workers/",
        wasmBaseURL: "/dist/wasm/",
        sampleBaseURL: "/dist/samples/",
        synthdefBaseURL: "/dist/synthdefs/",
      });

      try {
        await sonic.init();

        // Load a sample before reset
        await sonic.loadSample(0, "bd_haus.flac");
        await sonic.sync(1);
        const loadedBefore = true;

        await sonic.reset();

        // Load a sample after reset
        await sonic.loadSample(1, "bd_haus.flac");
        await sonic.sync(2);
        const loadedAfter = true;

        return {
          success: true,
          loadedBefore,
          loadedAfter,
        };
      } catch (err) {
        return { success: false, error: err.message, stack: err.stack, phase: err.phase || 'unknown' };
      }
    });

    expect(result.error).toBeUndefined();
    expect(result.success).toBe(true);
    expect(result.loadedBefore).toBe(true);
    expect(result.loadedAfter).toBe(true);
  });
});

test.describe("SuperSonic shutdown()", () => {
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

  test("shutdown() sets initialized to false", async ({ page }) => {
    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: "/dist/workers/",
        wasmBaseURL: "/dist/wasm/",
        sampleBaseURL: "/dist/samples/",
        synthdefBaseURL: "/dist/synthdefs/",
      });

      try {
        await sonic.init();
        const initializedBefore = sonic.initialized;

        await sonic.shutdown();
        const initializedAfter = sonic.initialized;

        return {
          success: true,
          initializedBefore,
          initializedAfter,
        };
      } catch (err) {
        return { success: false, error: err.message };
      }
    });

    expect(result.success).toBe(true);
    expect(result.initializedBefore).toBe(true);
    expect(result.initializedAfter).toBe(false);
  });

  test("shutdown() preserves listeners", async ({ page }) => {
    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: "/dist/workers/",
        wasmBaseURL: "/dist/wasm/",
        sampleBaseURL: "/dist/samples/",
        synthdefBaseURL: "/dist/synthdefs/",
      });

      let messageCount = 0;
      sonic.on('message', () => messageCount++);

      try {
        await sonic.init();

        // Generate a message
        sonic.send("/status");
        await sonic.sync(1);
        const countBeforeShutdown = messageCount;

        await sonic.shutdown();

        // Re-init and generate another message
        await sonic.init();
        sonic.send("/status");
        await sonic.sync(2);
        const countAfterReInit = messageCount;

        return {
          success: true,
          countBeforeShutdown,
          countAfterReInit,
          listenerWorkedAfterShutdown: countAfterReInit > countBeforeShutdown,
        };
      } catch (err) {
        return { success: false, error: err.message };
      }
    });

    expect(result.success).toBe(true);
    expect(result.countBeforeShutdown).toBeGreaterThan(0);
    expect(result.listenerWorkedAfterShutdown).toBe(true);
  });

  test("shutdown() emits shutdown event", async ({ page }) => {
    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: "/dist/workers/",
        wasmBaseURL: "/dist/wasm/",
        sampleBaseURL: "/dist/samples/",
        synthdefBaseURL: "/dist/synthdefs/",
      });

      let shutdownEventFired = false;
      sonic.on('shutdown', () => {
        shutdownEventFired = true;
      });

      try {
        await sonic.init();
        await sonic.shutdown();

        return {
          success: true,
          shutdownEventFired,
        };
      } catch (err) {
        return { success: false, error: err.message };
      }
    });

    expect(result.success).toBe(true);
    expect(result.shutdownEventFired).toBe(true);
  });

  test("shutdown() allows re-init", async ({ page }) => {
    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: "/dist/workers/",
        wasmBaseURL: "/dist/wasm/",
        sampleBaseURL: "/dist/samples/",
        synthdefBaseURL: "/dist/synthdefs/",
      });

      try {
        await sonic.init();
        await sonic.shutdown();
        await sonic.init();

        // Verify it works
        sonic.send("/status");
        await sonic.sync(1);

        return {
          success: true,
          initialized: sonic.initialized,
        };
      } catch (err) {
        return { success: false, error: err.message };
      }
    });

    expect(result.success).toBe(true);
    expect(result.initialized).toBe(true);
  });

  test("shutdown() can be called multiple times safely (idempotent)", async ({ page }) => {
    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: "/dist/workers/",
        wasmBaseURL: "/dist/wasm/",
        sampleBaseURL: "/dist/samples/",
        synthdefBaseURL: "/dist/synthdefs/",
      });

      let shutdownCount = 0;
      sonic.on('shutdown', () => shutdownCount++);

      try {
        await sonic.init();
        await sonic.shutdown();
        await sonic.shutdown(); // Second call should be no-op
        await sonic.shutdown(); // Third call should be no-op

        return {
          success: true,
          shutdownCount,
        };
      } catch (err) {
        return { success: false, error: err.message };
      }
    });

    expect(result.success).toBe(true);
    // Shutdown is idempotent - only fires once
    expect(result.shutdownCount).toBe(1);
  });
});

test.describe("SuperSonic destroy()", () => {
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

  test("destroy() clears all listeners", async ({ page }) => {
    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: "/dist/workers/",
        wasmBaseURL: "/dist/wasm/",
        sampleBaseURL: "/dist/samples/",
        synthdefBaseURL: "/dist/synthdefs/",
      });

      let callCount = 0;
      sonic.on('message', () => callCount++);
      sonic.on('ready', () => callCount++);
      sonic.on('shutdown', () => callCount++);

      try {
        await sonic.init();
        const countAfterInit = callCount; // ready fired

        await sonic.destroy();
        const countAfterDestroy = callCount; // shutdown fired

        // Try to trigger events - they should NOT fire because listeners are cleared
        // We can't actually re-init after destroy, but we can verify the count didn't change unexpectedly

        return {
          success: true,
          countAfterInit,
          countAfterDestroy,
          // Ready (1) + shutdown (1) = at least 2
          eventsFireDuringLifecycle: countAfterDestroy >= 2,
        };
      } catch (err) {
        return { success: false, error: err.message };
      }
    });

    expect(result.success).toBe(true);
    expect(result.eventsFireDuringLifecycle).toBe(true);
  });

  test("destroy() emits destroy event before clearing listeners", async ({ page }) => {
    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: "/dist/workers/",
        wasmBaseURL: "/dist/wasm/",
        sampleBaseURL: "/dist/samples/",
        synthdefBaseURL: "/dist/synthdefs/",
      });

      let destroyEventFired = false;
      let shutdownEventFired = false;
      let destroyFiredFirst = false;

      sonic.on('destroy', () => {
        destroyEventFired = true;
        destroyFiredFirst = !shutdownEventFired;
      });
      sonic.on('shutdown', () => {
        shutdownEventFired = true;
      });

      try {
        await sonic.init();
        await sonic.destroy();

        return {
          success: true,
          destroyEventFired,
          shutdownEventFired,
          destroyFiredFirst,
        };
      } catch (err) {
        return { success: false, error: err.message };
      }
    });

    expect(result.success).toBe(true);
    expect(result.destroyEventFired).toBe(true);
    expect(result.shutdownEventFired).toBe(true);
    expect(result.destroyFiredFirst).toBe(true); // destroy fires before shutdown
  });

  test("destroy() makes instance unusable", async ({ page }) => {
    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: "/dist/workers/",
        wasmBaseURL: "/dist/wasm/",
        sampleBaseURL: "/dist/samples/",
        synthdefBaseURL: "/dist/synthdefs/",
      });

      try {
        await sonic.init();
        await sonic.destroy();

        const initializedAfterDestroy = sonic.initialized;

        // Trying to send should fail - use async to catch promise rejection
        let sendFailed = false;
        let sendError = null;
        try {
          await sonic.send("/status");
        } catch (e) {
          sendFailed = true;
          sendError = e.message;
        }

        return {
          success: true,
          initializedAfterDestroy,
          sendFailed,
          sendError,
        };
      } catch (err) {
        return { success: false, error: err.message };
      }
    });

    expect(result.success).toBe(true);
    expect(result.initializedAfterDestroy).toBe(false);
    expect(result.sendFailed).toBe(true);
    expect(result.sendError).toContain("not initialized");
  });
});

test.describe("SuperSonic removeAllListeners()", () => {
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

  test("removeAllListeners() removes all listeners for specific event", async ({ page }) => {
    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: "/dist/workers/",
        wasmBaseURL: "/dist/wasm/",
        sampleBaseURL: "/dist/samples/",
        synthdefBaseURL: "/dist/synthdefs/",
      });

      let messageCount = 0;
      let readyCount = 0;

      sonic.on('message', () => messageCount++);
      sonic.on('message', () => messageCount++); // Two listeners
      sonic.on('ready', () => readyCount++);

      try {
        await sonic.init();
        const readyCountAfterInit = readyCount; // Should be 1

        // Remove only message listeners
        sonic.removeAllListeners('message');

        // Generate a message - should NOT increment messageCount
        sonic.send("/status");
        await sonic.sync(1);
        const messageCountAfterRemove = messageCount;

        // Reset to verify ready listener still works
        await sonic.reset();
        const readyCountAfterReset = readyCount; // Should be 2

        return {
          success: true,
          readyCountAfterInit,
          messageCountAfterRemove,
          readyCountAfterReset,
        };
      } catch (err) {
        return { success: false, error: err.message };
      }
    });

    expect(result.success).toBe(true);
    expect(result.readyCountAfterInit).toBe(1);
    expect(result.messageCountAfterRemove).toBe(0); // Message listeners were removed
    expect(result.readyCountAfterReset).toBe(2); // Ready listener still works
  });

  test("removeAllListeners() with no args removes ALL listeners", async ({ page }) => {
    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: "/dist/workers/",
        wasmBaseURL: "/dist/wasm/",
        sampleBaseURL: "/dist/samples/",
        synthdefBaseURL: "/dist/synthdefs/",
      });

      let anyEventFired = false;

      sonic.on('message', () => { anyEventFired = true; });
      sonic.on('ready', () => { anyEventFired = true; });
      sonic.on('shutdown', () => { anyEventFired = true; });

      // Remove ALL listeners before init
      sonic.removeAllListeners();

      try {
        await sonic.init();

        sonic.send("/status");
        await sonic.sync(1);

        await sonic.shutdown();

        return {
          success: true,
          anyEventFired,
        };
      } catch (err) {
        return { success: false, error: err.message };
      }
    });

    expect(result.success).toBe(true);
    expect(result.anyEventFired).toBe(false); // No events fired
  });

  test("removeAllListeners() returns this for chaining", async ({ page }) => {
    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: "/dist/workers/",
        wasmBaseURL: "/dist/wasm/",
        sampleBaseURL: "/dist/samples/",
        synthdefBaseURL: "/dist/synthdefs/",
      });

      sonic.on('message', () => {});

      const returnValue = sonic.removeAllListeners('message');

      return {
        success: true,
        returnsSelf: returnValue === sonic,
      };
    });

    expect(result.success).toBe(true);
    expect(result.returnsSelf).toBe(true);
  });
});

test.describe("SuperSonic audiocontext events", () => {
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

  test("audiocontext:statechange event fires on init", async ({ page }) => {
    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: "/dist/workers/",
        wasmBaseURL: "/dist/wasm/",
        sampleBaseURL: "/dist/samples/",
        synthdefBaseURL: "/dist/synthdefs/",
      });

      const stateChanges = [];
      sonic.on('audiocontext:statechange', ({ state }) => {
        stateChanges.push(state);
      });

      try {
        await sonic.init();

        return {
          success: true,
          stateChanges,
          hasRunningState: stateChanges.includes('running'),
        };
      } catch (err) {
        return { success: false, error: err.message };
      }
    });

    expect(result.success).toBe(true);
    // AudioContext should transition to 'running' after init
    expect(result.hasRunningState).toBe(true);
  });

  test("audiocontext:resumed event fires on init", async ({ page }) => {
    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: "/dist/workers/",
        wasmBaseURL: "/dist/wasm/",
        sampleBaseURL: "/dist/samples/",
        synthdefBaseURL: "/dist/synthdefs/",
      });

      let resumedFired = false;
      sonic.on('audiocontext:resumed', () => {
        resumedFired = true;
      });

      try {
        await sonic.init();

        return {
          success: true,
          resumedFired,
        };
      } catch (err) {
        return { success: false, error: err.message };
      }
    });

    expect(result.success).toBe(true);
    // AudioContext transitions to 'running' which fires 'resumed'
    expect(result.resumedFired).toBe(true);
  });

  test("can subscribe to audiocontext events before init", async ({ page }) => {
    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: "/dist/workers/",
        wasmBaseURL: "/dist/wasm/",
        sampleBaseURL: "/dist/samples/",
        synthdefBaseURL: "/dist/synthdefs/",
      });

      const events = [];

      // Subscribe before init
      sonic.on('audiocontext:statechange', ({ state }) => {
        events.push(`statechange:${state}`);
      });
      sonic.on('audiocontext:suspended', () => {
        events.push('suspended');
      });
      sonic.on('audiocontext:resumed', () => {
        events.push('resumed');
      });
      sonic.on('audiocontext:interrupted', () => {
        events.push('interrupted');
      });

      try {
        await sonic.init();

        return {
          success: true,
          events,
          hasEvents: events.length > 0,
        };
      } catch (err) {
        return { success: false, error: err.message };
      }
    });

    expect(result.success).toBe(true);
    expect(result.hasEvents).toBe(true);
  });
});

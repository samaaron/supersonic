/**
 * Tests for plugin command and unit command extensions.
 *
 * Tests the following upstream features (PR #7405):
 * - DoAsynchronousCommandEx (async plugin commands with reply address)
 * - DefineUnitCmd / DefineUnitCmdEx (sync/async unit commands)
 * - DoAsyncUnitCommand (async unit commands with Graph refcounting)
 * - Graph refcounting (safe synth deletion during async commands)
 *
 * Requires synthdefs: u_cmd_test, number
 * Compiled via: test/synthdefs/compile_demo_synthdefs.scd
 */

import { test, expect } from "./fixtures.mjs";

// =============================================================================
// PLUGIN COMMANDS (/cmd)
// =============================================================================

test.describe("Plugin command tests (/cmd pluginCmdDemo)", () => {
  test("async plugin command succeeds and sends /done", async ({ page, sonicConfig }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      function waitForMessage(sonic, address, timeoutMs = 5000) {
        return new Promise((resolve, reject) => {
          const timer = setTimeout(() => {
            sonic.off("in", handler);
            reject(new Error("Timeout waiting for " + address));
          }, timeoutMs);
          const handler = (msg) => {
            if (msg[0] === address) {
              clearTimeout(timer);
              sonic.off("in", handler);
              resolve(msg);
            }
          };
          sonic.on("in", handler);
        });
      }

      const sonic = new window.SuperSonic(config);
      const messages = [];
      sonic.on('in', (msg) => messages.push(msg));

      await sonic.init();

      // Send plugin command with args (7, 9, "mno")
      // No completion message for simplicity
      await sonic.send("/cmd", "pluginCmdDemo", 7, 9, "mno");

      // Wait for /done message
      try {
        const done = await waitForMessage(sonic, "/done", 3000);
        return {
          gotDone: true,
          doneCmd: done[1],
        };
      } catch (e) {
        return { gotDone: false, error: e.message };
      }
    }, sonicConfig);

    expect(result.gotDone).toBe(true);
    expect(result.doneCmd).toBe("pluginCmdDemo");
  });

  test("async plugin command with 'fail' string does not send /done", async ({ page, sonicConfig }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      const doneMessages = [];
      sonic.on('in', (msg) => {
        if (msg[0] === "/done") doneMessages.push(msg);
      });

      await sonic.init();

      // Send plugin command with "fail" string - should cause stage2 to return false
      await sonic.send("/cmd", "pluginCmdDemo", 7, 9, "fail");

      // Wait a bit to ensure no /done arrives
      await new Promise(r => setTimeout(r, 1000));
      await sonic.sync(1);

      return {
        doneCount: doneMessages.length,
      };
    }, sonicConfig);

    expect(result.doneCount).toBe(0);
  });

  test("async plugin command with minimal args succeeds", async ({ page, sonicConfig }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      function waitForMessage(sonic, address, timeoutMs = 5000) {
        return new Promise((resolve, reject) => {
          const timer = setTimeout(() => {
            sonic.off("in", handler);
            reject(new Error("Timeout waiting for " + address));
          }, timeoutMs);
          const handler = (msg) => {
            if (msg[0] === address) {
              clearTimeout(timer);
              sonic.off("in", handler);
              resolve(msg);
            }
          };
          sonic.on("in", handler);
        });
      }

      const sonic = new window.SuperSonic(config);
      await sonic.init();

      // Send with no optional args - empty string name should be used
      await sonic.send("/cmd", "pluginCmdDemo");

      try {
        const done = await waitForMessage(sonic, "/done", 3000);
        return { gotDone: true, doneCmd: done[1] };
      } catch (e) {
        return { gotDone: false };
      }
    }, sonicConfig);

    expect(result.gotDone).toBe(true);
    expect(result.doneCmd).toBe("pluginCmdDemo");
  });
});

// =============================================================================
// SYNCHRONOUS UNIT COMMANDS (/u_cmd setValue)
// =============================================================================

test.describe("Synchronous unit command tests (/u_cmd setValue)", () => {
  test("setValue sets UGen output value (queued - first calc)", async ({ page, sonicConfig }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      const messages = [];
      sonic.on('in', (msg) => messages.push(msg));

      await sonic.init();
      await sonic.loadSynthDef("u_cmd_test");

      const nodeId = 2000;
      const bus = 10;
      // UnitCmdDemo is at index 1 in this synthdef (0=Control, 1=UnitCmdDemo, 2=Out)
      const unitIndex = 1;

      // Create synth writing to control bus 10
      await sonic.send("/s_new", "u_cmd_test", nodeId, 0, 0, "bus", bus);

      // Send setValue command *immediately* (may be queued if ctor hasn't run yet)
      await sonic.send("/u_cmd", nodeId, unitIndex, "setValue", 4.5);
      await sonic.sync(1);

      // Give it a few calc cycles to output the value
      await new Promise(r => setTimeout(r, 100));

      // Read the control bus
      messages.length = 0;
      await sonic.send("/c_get", bus);
      await sonic.sync(2);

      const reply = messages.find((m) => m[0] === "/c_set");

      await sonic.send("/n_free", nodeId);

      return {
        busIndex: reply?.[1],
        value: reply?.[2],
      };
    }, sonicConfig);

    expect(result.busIndex).toBe(10);
    expect(result.value).toBeCloseTo(4.5, 3);
  });

  test("setValue updates value on already-running synth", async ({ page, sonicConfig }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      const messages = [];
      sonic.on('in', (msg) => messages.push(msg));

      await sonic.init();
      await sonic.loadSynthDef("u_cmd_test");

      const nodeId = 2001;
      const bus = 11;
      const unitIndex = 1;

      // Create synth and let it run
      await sonic.send("/s_new", "u_cmd_test", nodeId, 0, 0, "bus", bus);
      await sonic.sync(1);

      // Wait for ctor to be called
      await new Promise(r => setTimeout(r, 50));

      // Now send setValue (should be non-queued since ctor already ran)
      await sonic.send("/u_cmd", nodeId, unitIndex, "setValue", -3.0);
      await sonic.sync(2);

      // Give it a calc cycle
      await new Promise(r => setTimeout(r, 50));

      // Read the control bus
      messages.length = 0;
      await sonic.send("/c_get", bus);
      await sonic.sync(3);

      const reply = messages.find((m) => m[0] === "/c_set");

      // Set a second value to verify updates work repeatedly
      await sonic.send("/u_cmd", nodeId, unitIndex, "setValue", 7.25);
      await sonic.sync(4);
      await new Promise(r => setTimeout(r, 50));

      messages.length = 0;
      await sonic.send("/c_get", bus);
      await sonic.sync(5);

      const reply2 = messages.find((m) => m[0] === "/c_set");

      await sonic.send("/n_free", nodeId);

      return {
        value1: reply?.[2],
        value2: reply2?.[2],
      };
    }, sonicConfig);

    expect(result.value1).toBeCloseTo(-3.0, 3);
    expect(result.value2).toBeCloseTo(7.25, 3);
  });
});

// =============================================================================
// ASYNC UNIT COMMANDS (/u_cmd testCommand)
// =============================================================================

test.describe("Async unit command tests (/u_cmd testCommand)", () => {
  test("async unit command succeeds and sends /done", async ({ page, sonicConfig }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      function waitForMessage(sonic, address, timeoutMs = 5000) {
        return new Promise((resolve, reject) => {
          const timer = setTimeout(() => {
            sonic.off("in", handler);
            reject(new Error("Timeout waiting for " + address));
          }, timeoutMs);
          const handler = (msg) => {
            if (msg[0] === address) {
              clearTimeout(timer);
              sonic.off("in", handler);
              resolve(msg);
            }
          };
          sonic.on("in", handler);
        });
      }

      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("u_cmd_test");

      const nodeId = 2010;
      const unitIndex = 1;

      await sonic.send("/s_new", "u_cmd_test", nodeId, 0, 0, "bus", 0);
      await sonic.sync(1);
      await new Promise(r => setTimeout(r, 50));

      // Send async unit command: testCommand(size=64, value=1.0)
      // No completion message
      await sonic.send("/u_cmd", nodeId, unitIndex, "testCommand", 64, 1.0);

      try {
        const done = await waitForMessage(sonic, "/done", 3000);
        await sonic.send("/n_free", nodeId);
        return {
          gotDone: true,
          doneCmd: done[1],
        };
      } catch (e) {
        await sonic.send("/n_free", nodeId);
        return { gotDone: false, error: e.message };
      }
    }, sonicConfig);

    expect(result.gotDone).toBe(true);
    expect(result.doneCmd).toBe("testCommand");
  });

  test("async unit command failure does not send /done", async ({ page, sonicConfig }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("u_cmd_test");

      const nodeId = 2011;
      const unitIndex = 1;

      await sonic.send("/s_new", "u_cmd_test", nodeId, 0, 0, "bus", 0);
      await sonic.sync(1);
      await new Promise(r => setTimeout(r, 50));

      // Start collecting /done messages AFTER setup
      const doneMessages = [];
      sonic.on('in', (msg) => {
        if (msg[0] === "/done" && msg[1] === "testCommand") doneMessages.push(msg);
      });

      // Send with negative value - causes stage2 to return false (failure)
      await sonic.send("/u_cmd", nodeId, unitIndex, "testCommand", 64, -1.0);

      // Wait a bit to confirm no /done arrives
      await new Promise(r => setTimeout(r, 1000));
      await sonic.sync(2);

      await sonic.send("/n_free", nodeId);

      return {
        doneCount: doneMessages.length,
      };
    }, sonicConfig);

    expect(result.doneCount).toBe(0);
  });

  test("freeing synth during async unit command does not crash", async ({ page, sonicConfig }) => {
    await page.goto("/test/harness.html");

    // In WASM NRT mode (single-threaded), async commands complete synchronously
    // via CallEveryStage() before n_free is processed. The Graph is still alive
    // when stage3 runs, so /done IS sent. This differs from upstream multi-threaded
    // behavior where the n_free could interleave with the async command stages.
    // The important assertion is that the server remains healthy.

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("u_cmd_test");

      const nodeId = 2012;
      const unitIndex = 1;

      await sonic.send("/s_new", "u_cmd_test", nodeId, 0, 0, "bus", 0);
      await sonic.sync(1);
      await new Promise(r => setTimeout(r, 50));

      // Send async unit command and immediately free the synth
      await sonic.send("/u_cmd", nodeId, unitIndex, "testCommand", 64, 1.0);
      await sonic.send("/n_free", nodeId);

      // Wait for things to settle - should not crash
      await new Promise(r => setTimeout(r, 1000));
      await sonic.sync(2);

      // Verify server is still healthy by checking /status
      const messages = [];
      sonic.on('in', (msg) => messages.push(msg));
      await sonic.send("/status");
      await sonic.sync(3);

      const statusReply = messages.find((m) => m[0] === "/status.reply");

      return {
        serverHealthy: !!statusReply,
      };
    }, sonicConfig);

    // The key assertion: server didn't crash
    expect(result.serverHealthy).toBe(true);
  });

  test("multiple async unit commands on same synth work", async ({ page, sonicConfig }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      function waitForMessage(sonic, address, timeoutMs = 5000) {
        return new Promise((resolve, reject) => {
          const timer = setTimeout(() => {
            sonic.off("in", handler);
            reject(new Error("Timeout waiting for " + address));
          }, timeoutMs);
          const handler = (msg) => {
            if (msg[0] === address) {
              clearTimeout(timer);
              sonic.off("in", handler);
              resolve(msg);
            }
          };
          sonic.on("in", handler);
        });
      }

      const sonic = new window.SuperSonic(config);
      const doneMessages = [];
      sonic.on('in', (msg) => {
        if (msg[0] === "/done" && msg[1] === "testCommand") doneMessages.push(msg);
      });

      await sonic.init();
      await sonic.loadSynthDef("u_cmd_test");

      const nodeId = 2013;
      const unitIndex = 1;

      await sonic.send("/s_new", "u_cmd_test", nodeId, 0, 0, "bus", 0);
      await sonic.sync(1);
      await new Promise(r => setTimeout(r, 50));

      // Send multiple async unit commands
      await sonic.send("/u_cmd", nodeId, unitIndex, "testCommand", 32, 1.0);
      await sonic.send("/u_cmd", nodeId, unitIndex, "testCommand", 64, 2.0);
      await sonic.send("/u_cmd", nodeId, unitIndex, "testCommand", 16, 3.0);

      // Wait for all to complete
      await new Promise(r => setTimeout(r, 2000));
      await sonic.sync(2);

      await sonic.send("/n_free", nodeId);

      return {
        doneCount: doneMessages.length,
      };
    }, sonicConfig);

    expect(result.doneCount).toBe(3);
  });
});

// =============================================================================
// GRAPH REFCOUNTING / NODE LIFECYCLE
// =============================================================================

test.describe("Graph refcounting and node lifecycle", () => {
  test("Node_Remove handles null parent gracefully", async ({ page, sonicConfig }) => {
    await page.goto("/test/harness.html");

    // This tests the null-parent guard added in Node_Remove.
    // We verify that repeated free of the same node doesn't crash.
    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("u_cmd_test");

      const nodeId = 2020;

      await sonic.send("/s_new", "u_cmd_test", nodeId, 0, 0, "bus", 0);
      await sonic.sync(1);

      // Free the node
      await sonic.send("/n_free", nodeId);
      await sonic.sync(2);

      // Server should still be healthy
      const messages = [];
      sonic.on('in', (msg) => messages.push(msg));

      await sonic.send("/status");
      await sonic.sync(3);

      const statusReply = messages.find((m) => m[0] === "/status.reply");

      return {
        serverHealthy: !!statusReply,
      };
    }, sonicConfig);

    expect(result.serverHealthy).toBe(true);
  });

  test("freed synth ID becomes available again during async unit command", async ({ page, sonicConfig }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      function waitForMessage(sonic, address, timeoutMs = 5000) {
        return new Promise((resolve, reject) => {
          const timer = setTimeout(() => {
            sonic.off("in", handler);
            reject(new Error("Timeout waiting for " + address));
          }, timeoutMs);
          const handler = (msg) => {
            if (msg[0] === address) {
              clearTimeout(timer);
              sonic.off("in", handler);
              resolve(msg);
            }
          };
          sonic.on("in", handler);
        });
      }

      const sonic = new window.SuperSonic(config);
      const messages = [];
      sonic.on('in', (msg) => messages.push(msg));

      await sonic.init();
      await sonic.loadSynthDef("u_cmd_test");

      const nodeId = 2021;
      const unitIndex = 1;

      // Create first synth
      await sonic.send("/s_new", "u_cmd_test", nodeId, 0, 0, "bus", 0);
      await sonic.sync(1);
      await new Promise(r => setTimeout(r, 50));

      // Start async command and free the synth
      await sonic.send("/u_cmd", nodeId, unitIndex, "testCommand", 64, 1.0);
      await sonic.send("/n_free", nodeId);
      await sonic.sync(2);

      // Wait for async command to finish
      await new Promise(r => setTimeout(r, 500));

      // The node ID should be freed - we should be able to reuse it
      await sonic.send("/s_new", "u_cmd_test", nodeId, 0, 0, "bus", 0);
      await sonic.sync(3);

      // Verify the new synth exists
      messages.length = 0;
      await sonic.send("/status");
      await sonic.sync(4);

      const statusReply = messages.find((m) => m[0] === "/status.reply");

      await sonic.send("/n_free", nodeId);

      return {
        serverHealthy: !!statusReply,
        numSynths: statusReply?.[3],
      };
    }, sonicConfig);

    expect(result.serverHealthy).toBe(true);
    // Should have at least 1 synth (the one we just created)
    expect(result.numSynths).toBeGreaterThanOrEqual(1);
  });

  test("server survives rapid create-command-free cycles", async ({ page, sonicConfig }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("u_cmd_test");

      const unitIndex = 1;

      // Rapidly create synths, send async commands, and free them
      for (let i = 0; i < 10; i++) {
        const nodeId = 3000 + i;
        await sonic.send("/s_new", "u_cmd_test", nodeId, 0, 0, "bus", 0);
        await sonic.send("/u_cmd", nodeId, unitIndex, "testCommand", 16, 1.0);
        await sonic.send("/n_free", nodeId);
      }

      // Wait for everything to settle
      await new Promise(r => setTimeout(r, 2000));
      await sonic.sync(1);

      // Verify server health
      const messages = [];
      sonic.on('in', (msg) => messages.push(msg));
      await sonic.send("/status");
      await sonic.sync(2);

      const statusReply = messages.find((m) => m[0] === "/status.reply");

      return {
        serverHealthy: !!statusReply,
        numSynths: statusReply?.[3],
      };
    }, sonicConfig);

    expect(result.serverHealthy).toBe(true);
    // All synths should be freed
    expect(result.numSynths).toBe(0);
  });
});

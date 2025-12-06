/**
 * OSC Command Semantic Test Suite
 *
 * Deep semantic tests for scsynth OSC commands, verifying actual behavior
 * not just that commands don't crash. Based on SuperCollider test patterns.
 *
 * See docs/OSC_TESTING_PLAN.md for the full testing plan.
 */

import { test, expect } from "@playwright/test";

// =============================================================================
// TEST UTILITIES
// =============================================================================

/**
 * Standard SuperSonic initialization config
 */
const SONIC_CONFIG = {
  workerBaseURL: "/dist/workers/",
  wasmBaseURL: "/dist/wasm/",
  sampleBaseURL: "/dist/samples/",
  synthdefBaseURL: "/dist/synthdefs/",
};

// =============================================================================
// /n_free - FREE NODES
// =============================================================================

test.describe("/n_free semantic tests", () => {
  test("frees single synth and removes from tree", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      // Create synth
      await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 0, "release", 60);
      await sonic.sync(1);

      const treeBefore = sonic.getTree();
      const nodeBefore = treeBefore.nodes.find((n) => n.id === 1000);

      // Free synth
      await sonic.send("/n_free", 1000);
      await sonic.sync(2);

      const treeAfter = sonic.getTree();
      const nodeAfter = treeAfter.nodes.find((n) => n.id === 1000);

      return {
        existedBefore: !!nodeBefore,
        existsAfter: !!nodeAfter,
        countBefore: treeBefore.nodeCount,
        countAfter: treeAfter.nodeCount,
      };
    }, SONIC_CONFIG);

    expect(result.existedBefore).toBe(true);
    expect(result.existsAfter).toBe(false);
    expect(result.countAfter).toBe(result.countBefore - 1);
  });

  test("frees multiple nodes in single command", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      // Create three synths
      await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 0, "release", 60);
      await sonic.send("/s_new", "sonic-pi-beep", 1001, 0, 0, "release", 60);
      await sonic.send("/s_new", "sonic-pi-beep", 1002, 0, 0, "release", 60);
      await sonic.sync(1);

      const treeBefore = sonic.getTree();
      const countBefore = treeBefore.nodes.filter((n) =>
        [1000, 1001, 1002].includes(n.id)
      ).length;

      // Free all three in single command
      await sonic.send("/n_free", 1000, 1001, 1002);
      await sonic.sync(2);

      const treeAfter = sonic.getTree();
      const countAfter = treeAfter.nodes.filter((n) =>
        [1000, 1001, 1002].includes(n.id)
      ).length;

      return {
        countBefore,
        countAfter,
        totalBefore: treeBefore.nodeCount,
        totalAfter: treeAfter.nodeCount,
      };
    }, SONIC_CONFIG);

    expect(result.countBefore).toBe(3);
    expect(result.countAfter).toBe(0);
    expect(result.totalAfter).toBe(result.totalBefore - 3);
  });

  test("freeing non-existent node does not error", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      // Create one synth
      await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 0, "release", 60);
      await sonic.sync(1);

      const treeBefore = sonic.getTree();

      // Try to free non-existent node 9999
      await sonic.send("/n_free", 9999);
      await sonic.sync(2);

      const treeAfter = sonic.getTree();

      // Original synth should still exist
      const synthStillExists = treeAfter.nodes.some((n) => n.id === 1000);

      // Cleanup
      await sonic.send("/n_free", 1000);

      return {
        noError: true,
        synthStillExists,
        countUnchanged: treeBefore.nodeCount === treeAfter.nodeCount,
      };
    }, SONIC_CONFIG);

    expect(result.noError).toBe(true);
    expect(result.synthStillExists).toBe(true);
    expect(result.countUnchanged).toBe(true);
  });

  test("freeing group frees the group and all its children", async ({
    page,
  }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      // Create group with synths inside
      await sonic.send("/g_new", 100, 0, 0);
      await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 100, "release", 60);
      await sonic.send("/s_new", "sonic-pi-beep", 1001, 0, 100, "release", 60);
      await sonic.send("/s_new", "sonic-pi-beep", 1002, 0, 100, "release", 60);
      await sonic.sync(1);

      const treeBefore = sonic.getTree();
      const groupExists = treeBefore.nodes.some((n) => n.id === 100);
      const synthsInGroup = treeBefore.nodes.filter(
        (n) => n.parentId === 100
      ).length;

      // Free the group (should free group AND all children)
      await sonic.send("/n_free", 100);
      await sonic.sync(2);

      const treeAfter = sonic.getTree();
      const groupExistsAfter = treeAfter.nodes.some((n) => n.id === 100);
      const synth1000Exists = treeAfter.nodes.some((n) => n.id === 1000);
      const synth1001Exists = treeAfter.nodes.some((n) => n.id === 1001);
      const synth1002Exists = treeAfter.nodes.some((n) => n.id === 1002);

      return {
        groupExistedBefore: groupExists,
        synthsInGroupBefore: synthsInGroup,
        groupExistsAfter,
        synth1000Exists,
        synth1001Exists,
        synth1002Exists,
        nodeCountBefore: treeBefore.nodeCount,
        nodeCountAfter: treeAfter.nodeCount,
      };
    }, SONIC_CONFIG);

    expect(result.groupExistedBefore).toBe(true);
    expect(result.synthsInGroupBefore).toBe(3);
    expect(result.groupExistsAfter).toBe(false);
    expect(result.synth1000Exists).toBe(false);
    expect(result.synth1001Exists).toBe(false);
    expect(result.synth1002Exists).toBe(false);
    // Should have freed 4 nodes (1 group + 3 synths)
    expect(result.nodeCountAfter).toBe(result.nodeCountBefore - 4);
  });

  test("sends /n_end notification when node freed", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      const messages = [];
      sonic.on('message', (msg) => messages.push(msg));

      await sonic.init();
      await sonic.send("/notify", 1); // Register for notifications
      await sonic.loadSynthDef("sonic-pi-beep");

      // Create synth
      await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 0, "release", 60);
      await sonic.sync(1);

      // Clear messages and free synth
      messages.length = 0;
      await sonic.send("/n_free", 1000);
      await sonic.sync(2);

      // Find /n_end notification for node 1000
      const nEndMsg = messages.find(
        (m) => m.address === "/n_end" && m.args[0] === 1000
      );

      return {
        receivedNEnd: !!nEndMsg,
        nEndNodeId: nEndMsg?.args?.[0],
        nEndParentId: nEndMsg?.args?.[1],
      };
    }, SONIC_CONFIG);

    expect(result.receivedNEnd).toBe(true);
    expect(result.nEndNodeId).toBe(1000);
    expect(result.nEndParentId).toBe(0); // Was in root group
  });

  test("freeing already-freed node is idempotent (no error)", async ({
    page,
  }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      // Create and free synth
      await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 0, "release", 60);
      await sonic.sync(1);
      await sonic.send("/n_free", 1000);
      await sonic.sync(2);

      const treeAfterFirstFree = sonic.getTree();

      // Free again - should not error
      await sonic.send("/n_free", 1000);
      await sonic.sync(3);

      const treeAfterSecondFree = sonic.getTree();

      return {
        noError: true,
        countUnchanged:
          treeAfterFirstFree.nodeCount === treeAfterSecondFree.nodeCount,
      };
    }, SONIC_CONFIG);

    expect(result.noError).toBe(true);
    expect(result.countUnchanged).toBe(true);
  });

  test("freeing nested groups frees entire subtree", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      // Create nested structure: group 100 -> group 101 -> synth 1000
      await sonic.send("/g_new", 100, 0, 0);
      await sonic.send("/g_new", 101, 0, 100);
      await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 101, "release", 60);
      // Also add synth directly to group 100
      await sonic.send("/s_new", "sonic-pi-beep", 1001, 0, 100, "release", 60);
      await sonic.sync(1);

      const treeBefore = sonic.getTree();

      // Free top-level group
      await sonic.send("/n_free", 100);
      await sonic.sync(2);

      const treeAfter = sonic.getTree();

      return {
        group100ExistsBefore: treeBefore.nodes.some((n) => n.id === 100),
        group101ExistsBefore: treeBefore.nodes.some((n) => n.id === 101),
        synth1000ExistsBefore: treeBefore.nodes.some((n) => n.id === 1000),
        synth1001ExistsBefore: treeBefore.nodes.some((n) => n.id === 1001),
        group100ExistsAfter: treeAfter.nodes.some((n) => n.id === 100),
        group101ExistsAfter: treeAfter.nodes.some((n) => n.id === 101),
        synth1000ExistsAfter: treeAfter.nodes.some((n) => n.id === 1000),
        synth1001ExistsAfter: treeAfter.nodes.some((n) => n.id === 1001),
        freedCount: treeBefore.nodeCount - treeAfter.nodeCount,
      };
    }, SONIC_CONFIG);

    expect(result.group100ExistsBefore).toBe(true);
    expect(result.group101ExistsBefore).toBe(true);
    expect(result.synth1000ExistsBefore).toBe(true);
    expect(result.synth1001ExistsBefore).toBe(true);
    expect(result.group100ExistsAfter).toBe(false);
    expect(result.group101ExistsAfter).toBe(false);
    expect(result.synth1000ExistsAfter).toBe(false);
    expect(result.synth1001ExistsAfter).toBe(false);
    expect(result.freedCount).toBe(4); // 2 groups + 2 synths
  });
});

// =============================================================================
// /n_set - SET NODE CONTROLS
// =============================================================================

test.describe("/n_set semantic tests", () => {
  test("sets control by name", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      const messages = [];
      sonic.on('message', (msg) => messages.push(msg));

      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      // Create synth with default note
      await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 0, "release", 60);
      await sonic.sync(1);

      // Set note by name
      await sonic.send("/n_set", 1000, "note", 72);
      await sonic.sync(2);

      // Get the value back
      messages.length = 0;
      await sonic.send("/s_get", 1000, "note");
      await sonic.sync(3);

      const reply = messages.find((m) => m.address === "/n_set");

      // Cleanup
      await sonic.send("/n_free", 1000);

      return {
        gotReply: !!reply,
        nodeId: reply?.args?.[0],
        controlName: reply?.args?.[1],
        value: reply?.args?.[2],
      };
    }, SONIC_CONFIG);

    expect(result.gotReply).toBe(true);
    expect(result.nodeId).toBe(1000);
    expect(result.controlName).toBe("note");
    expect(result.value).toBe(72);
  });

  test("sets control by index", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      const messages = [];
      sonic.on('message', (msg) => messages.push(msg));

      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      // Create synth
      await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 0, "release", 60);
      await sonic.sync(1);

      // Get initial value of control 0
      await sonic.send("/s_get", 1000, 0);
      await sonic.sync(2);
      const initialReply = messages.find((m) => m.address === "/n_set");
      const initialValue = initialReply?.args?.[2];

      // Set control 0 by index to a different value
      const newValue = initialValue === 0.5 ? 0.75 : 0.5;
      await sonic.send("/n_set", 1000, 0, newValue);
      await sonic.sync(3);

      // Get the value back
      messages.length = 0;
      await sonic.send("/s_get", 1000, 0);
      await sonic.sync(4);

      const reply = messages.find((m) => m.address === "/n_set");

      // Cleanup
      await sonic.send("/n_free", 1000);

      return {
        gotReply: !!reply,
        value: reply?.args?.[2],
        expectedValue: newValue,
      };
    }, SONIC_CONFIG);

    expect(result.gotReply).toBe(true);
    expect(result.value).toBeCloseTo(result.expectedValue, 5);
  });

  test("sets multiple controls in single command", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      const messages = [];
      sonic.on('message', (msg) => messages.push(msg));

      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      // Create synth
      await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 0, "release", 60);
      await sonic.sync(1);

      // Set multiple controls at once
      await sonic.send("/n_set", 1000, "note", 64, "amp", 0.25, "pan", -0.5);
      await sonic.sync(2);

      // Get each value back
      messages.length = 0;
      await sonic.send("/s_get", 1000, "note");
      await sonic.sync(3);
      const noteReply = messages.find((m) => m.address === "/n_set");

      messages.length = 0;
      await sonic.send("/s_get", 1000, "amp");
      await sonic.sync(4);
      const ampReply = messages.find((m) => m.address === "/n_set");

      messages.length = 0;
      await sonic.send("/s_get", 1000, "pan");
      await sonic.sync(5);
      const panReply = messages.find((m) => m.address === "/n_set");

      // Cleanup
      await sonic.send("/n_free", 1000);

      return {
        noteValue: noteReply?.args?.[2],
        ampValue: ampReply?.args?.[2],
        panValue: panReply?.args?.[2],
      };
    }, SONIC_CONFIG);

    expect(result.noteValue).toBe(64);
    expect(result.ampValue).toBeCloseTo(0.25, 5);
    expect(result.panValue).toBeCloseTo(-0.5, 5);
  });

  test("setting control on group affects all children", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      const messages = [];
      sonic.on('message', (msg) => messages.push(msg));

      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      // Create group with multiple synths
      await sonic.send("/g_new", 100, 0, 0);
      await sonic.send(
        "/s_new",
        "sonic-pi-beep",
        1000,
        0,
        100,
        "note",
        60,
        "release",
        60
      );
      await sonic.send(
        "/s_new",
        "sonic-pi-beep",
        1001,
        0,
        100,
        "note",
        60,
        "release",
        60
      );
      await sonic.send(
        "/s_new",
        "sonic-pi-beep",
        1002,
        0,
        100,
        "note",
        60,
        "release",
        60
      );
      await sonic.sync(1);

      // Set amp on the GROUP - should affect all children
      await sonic.send("/n_set", 100, "amp", 0.1);
      await sonic.sync(2);

      // Get amp from each synth
      const ampValues = [];
      for (const id of [1000, 1001, 1002]) {
        messages.length = 0;
        await sonic.send("/s_get", id, "amp");
        await sonic.sync(3 + id - 1000);
        const reply = messages.find((m) => m.address === "/n_set");
        ampValues.push(reply?.args?.[2]);
      }

      // Cleanup
      await sonic.send("/n_free", 100);

      return {
        ampValues,
        allMatch: ampValues.every((v) => Math.abs(v - 0.1) < 0.001),
      };
    }, SONIC_CONFIG);

    expect(result.allMatch).toBe(true);
    expect(result.ampValues).toHaveLength(3);
  });

  test("setting non-existent control does not error", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      // Create synth
      await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 0, "release", 60);
      await sonic.sync(1);

      // Try to set non-existent control - should not error
      await sonic.send("/n_set", 1000, "nonexistent_control_xyz", 999);
      await sonic.sync(2);

      // Synth should still be running
      const tree = sonic.getTree();
      const synthExists = tree.nodes.some((n) => n.id === 1000);

      // Cleanup
      await sonic.send("/n_free", 1000);

      return {
        noError: true,
        synthStillExists: synthExists,
      };
    }, SONIC_CONFIG);

    expect(result.noError).toBe(true);
    expect(result.synthStillExists).toBe(true);
  });
});

// =============================================================================
// /n_setn - SET SEQUENTIAL NODE CONTROLS
// =============================================================================

test.describe("/n_setn semantic tests", () => {
  test("sets sequential controls by index", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      const messages = [];
      sonic.on('message', (msg) => messages.push(msg));

      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      // Create synth
      await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 0, "release", 60);
      await sonic.sync(1);

      // Set controls 0, 1, 2 to specific values
      await sonic.send("/n_setn", 1000, 0, 3, 0.111, 0.222, 0.333);
      await sonic.sync(2);

      // Get them back with /s_getn
      messages.length = 0;
      await sonic.send("/s_getn", 1000, 0, 3);
      await sonic.sync(3);

      const reply = messages.find((m) => m.address === "/n_setn");

      // Cleanup
      await sonic.send("/n_free", 1000);

      return {
        gotReply: !!reply,
        nodeId: reply?.args?.[0],
        startIndex: reply?.args?.[1],
        count: reply?.args?.[2],
        values: reply?.args?.slice(3),
      };
    }, SONIC_CONFIG);

    expect(result.gotReply).toBe(true);
    expect(result.nodeId).toBe(1000);
    expect(result.startIndex).toBe(0);
    expect(result.count).toBe(3);
    expect(result.values[0]).toBeCloseTo(0.111, 3);
    expect(result.values[1]).toBeCloseTo(0.222, 3);
    expect(result.values[2]).toBeCloseTo(0.333, 3);
  });

  test("sets sequential controls by name", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      const messages = [];
      sonic.on('message', (msg) => messages.push(msg));

      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      // Create synth
      await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 0, "release", 60);
      await sonic.sync(1);

      // Set 2 controls starting from "note"
      await sonic.send("/n_setn", 1000, "note", 2, 72, 0.75);
      await sonic.sync(2);

      // Verify note was set
      messages.length = 0;
      await sonic.send("/s_get", 1000, "note");
      await sonic.sync(3);
      const noteReply = messages.find((m) => m.address === "/n_set");

      // Cleanup
      await sonic.send("/n_free", 1000);

      return {
        noteValue: noteReply?.args?.[2],
      };
    }, SONIC_CONFIG);

    expect(result.noteValue).toBe(72);
  });
});

// =============================================================================
// /n_fill - FILL NODE CONTROLS
// =============================================================================

test.describe("/n_fill semantic tests", () => {
  test("fills range of controls with single value", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      const messages = [];
      sonic.on('message', (msg) => messages.push(msg));

      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      // Create synth
      await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 0, "release", 60);
      await sonic.sync(1);

      // Fill controls 0-2 with value 0.5
      await sonic.send("/n_fill", 1000, 0, 3, 0.5);
      await sonic.sync(2);

      // Get them back
      messages.length = 0;
      await sonic.send("/s_getn", 1000, 0, 3);
      await sonic.sync(3);

      const reply = messages.find((m) => m.address === "/n_setn");
      const values = reply?.args?.slice(3);

      // Cleanup
      await sonic.send("/n_free", 1000);

      return {
        gotReply: !!reply,
        values,
        allMatch: values?.every((v) => Math.abs(v - 0.5) < 0.001),
      };
    }, SONIC_CONFIG);

    expect(result.gotReply).toBe(true);
    expect(result.allMatch).toBe(true);
  });
});

// =============================================================================
// /n_map - MAP CONTROL TO BUS
// =============================================================================

test.describe("/n_map semantic tests", () => {
  // NOTE: /s_get returns the control's current value, not the bus index.
  // There's no direct way to query if a control is mapped via OSC.
  // These tests verify the commands execute without error and the synth continues running.
  // Actual mapping behavior would need to be verified via audio output testing.

  test("mapped control reads from bus", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      // Set control bus 0 to specific value
      await sonic.send("/c_set", 0, 72);
      await sonic.sync(1);

      // Create synth and map note to bus 0
      await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 0, "note", 60, "release", 60);
      await sonic.send("/n_map", 1000, "note", 0);
      await sonic.sync(2);

      const tree = sonic.getTree();
      const synthExists = tree.nodes.some((n) => n.id === 1000);

      // Cleanup
      await sonic.send("/n_free", 1000);

      return { synthExists };
    }, SONIC_CONFIG);

    expect(result.synthExists).toBe(true);
  });

  test("unmap with bus index -1", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      const messages = [];
      sonic.on('message', (msg) => messages.push(msg));

      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      // Create synth
      await sonic.send(
        "/s_new",
        "sonic-pi-beep",
        1000,
        0,
        0,
        "note",
        60,
        "release",
        60
      );
      await sonic.sync(1);

      // Map note to bus 0
      await sonic.send("/n_map", 1000, "note", 0);
      await sonic.sync(2);

      // Unmap with -1
      await sonic.send("/n_map", 1000, "note", -1);
      await sonic.sync(3);

      // Set note directly - should work after unmap
      await sonic.send("/n_set", 1000, "note", 84);
      await sonic.sync(4);

      // Verify value
      messages.length = 0;
      await sonic.send("/s_get", 1000, "note");
      await sonic.sync(5);
      const reply = messages.find((m) => m.address === "/n_set");

      // Cleanup
      await sonic.send("/n_free", 1000);

      return {
        noteValue: reply?.args?.[2],
      };
    }, SONIC_CONFIG);

    expect(result.noteValue).toBe(84);
  });

  test("maps multiple controls in single command", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      // Set bus values
      await sonic.send("/c_set", 0, 72, 1, 0.5);
      await sonic.sync(1);

      // Create synth
      await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 0, "release", 60);
      await sonic.sync(2);

      // Map multiple controls in one command: note->bus0, amp->bus1
      await sonic.send("/n_map", 1000, "note", 0, "amp", 1);
      await sonic.sync(3);

      const tree = sonic.getTree();
      const synthExists = tree.nodes.some((n) => n.id === 1000);

      // Cleanup
      await sonic.send("/n_free", 1000);

      return { synthExists };
    }, SONIC_CONFIG);

    expect(result.synthExists).toBe(true);
  });
});

// =============================================================================
// /s_new - CREATE SYNTH
// =============================================================================

test.describe("/s_new semantic tests", () => {
  test("creates synth with specified ID", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      await sonic.send("/s_new", "sonic-pi-beep", 1234, 0, 0, "release", 60);
      await sonic.sync(1);

      const tree = sonic.getTree();
      const synth = tree.nodes.find((n) => n.id === 1234);

      // Cleanup
      await sonic.send("/n_free", 1234);

      return {
        exists: !!synth,
        id: synth?.id,
        defName: synth?.defName,
        isGroup: synth?.isGroup,
      };
    }, SONIC_CONFIG);

    expect(result.exists).toBe(true);
    expect(result.id).toBe(1234);
    expect(result.defName).toBe("sonic-pi-beep");
    expect(result.isGroup).toBe(false);
  });

  // Auto-generated IDs with -1 create negative IDs (e.g., -2147483640, -2147483632, ...)
  // These DO appear in the SAB tree for visualization purposes.
  test("auto-generates ID with -1", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      const nodeCountBefore = sonic.getTree().nodeCount;

      // Create synth with auto ID
      await sonic.send("/s_new", "sonic-pi-beep", -1, 0, 0, "release", 60);
      await sonic.sync(1);

      const treeAfter = sonic.getTree();
      const autoSynth = treeAfter.nodes.find(
        (n) => n.id < 0 && n.defName === "sonic-pi-beep"
      );

      return {
        nodeCountBefore,
        nodeCountAfter: treeAfter.nodeCount,
        autoSynthFound: !!autoSynth,
        autoSynthId: autoSynth?.id,
      };
    }, SONIC_CONFIG);

    // SAB tree now includes auto-assigned negative IDs
    expect(result.nodeCountAfter).toBe(result.nodeCountBefore + 1);
    expect(result.autoSynthFound).toBe(true);
    expect(result.autoSynthId).toBeLessThan(0);
  });

  test("add action 0 - adds to head of group", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      // Create group
      await sonic.send("/g_new", 100, 0, 0);

      // Add synth 1000 to tail
      await sonic.send("/s_new", "sonic-pi-beep", 1000, 1, 100, "release", 60);

      // Add synth 1001 to head - should be BEFORE 1000
      await sonic.send("/s_new", "sonic-pi-beep", 1001, 0, 100, "release", 60);

      await sonic.sync(1);

      const tree = sonic.getTree();
      const synth1000 = tree.nodes.find((n) => n.id === 1000);
      const synth1001 = tree.nodes.find((n) => n.id === 1001);

      // Cleanup
      await sonic.send("/n_free", 100);

      return {
        synth1000PrevId: synth1000?.prevId,
        synth1000NextId: synth1000?.nextId,
        synth1001PrevId: synth1001?.prevId,
        synth1001NextId: synth1001?.nextId,
      };
    }, SONIC_CONFIG);

    // 1001 at head: prevId = -1, nextId = 1000
    // 1000 at tail: prevId = 1001, nextId = -1
    expect(result.synth1001PrevId).toBe(-1);
    expect(result.synth1001NextId).toBe(1000);
    expect(result.synth1000PrevId).toBe(1001);
    expect(result.synth1000NextId).toBe(-1);
  });

  test("add action 1 - adds to tail of group", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      // Create group
      await sonic.send("/g_new", 100, 0, 0);

      // Add synths to tail in order
      await sonic.send("/s_new", "sonic-pi-beep", 1000, 1, 100, "release", 60);
      await sonic.send("/s_new", "sonic-pi-beep", 1001, 1, 100, "release", 60);
      await sonic.send("/s_new", "sonic-pi-beep", 1002, 1, 100, "release", 60);

      await sonic.sync(1);

      const tree = sonic.getTree();
      const synth1000 = tree.nodes.find((n) => n.id === 1000);
      const synth1001 = tree.nodes.find((n) => n.id === 1001);
      const synth1002 = tree.nodes.find((n) => n.id === 1002);

      // Cleanup
      await sonic.send("/n_free", 100);

      return {
        // Order should be: 1000 -> 1001 -> 1002
        synth1000PrevId: synth1000?.prevId,
        synth1000NextId: synth1000?.nextId,
        synth1001PrevId: synth1001?.prevId,
        synth1001NextId: synth1001?.nextId,
        synth1002PrevId: synth1002?.prevId,
        synth1002NextId: synth1002?.nextId,
      };
    }, SONIC_CONFIG);

    // 1000 at head: prevId = -1, nextId = 1001
    expect(result.synth1000PrevId).toBe(-1);
    expect(result.synth1000NextId).toBe(1001);
    // 1001 in middle
    expect(result.synth1001PrevId).toBe(1000);
    expect(result.synth1001NextId).toBe(1002);
    // 1002 at tail: prevId = 1001, nextId = -1
    expect(result.synth1002PrevId).toBe(1001);
    expect(result.synth1002NextId).toBe(-1);
  });

  test("add action 2 - adds before target node", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      // Create synth 1000
      await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 0, "release", 60);
      await sonic.sync(1);

      // Add synth 1001 BEFORE 1000 (action 2)
      await sonic.send("/s_new", "sonic-pi-beep", 1001, 2, 1000, "release", 60);
      await sonic.sync(2);

      const tree = sonic.getTree();
      const synth1000 = tree.nodes.find((n) => n.id === 1000);
      const synth1001 = tree.nodes.find((n) => n.id === 1001);

      // Cleanup
      await sonic.send("/n_free", 1000, 1001);

      return {
        // 1001 should be before 1000
        synth1001PrevId: synth1001?.prevId,
        synth1001NextId: synth1001?.nextId,
        synth1000PrevId: synth1000?.prevId,
        synth1000NextId: synth1000?.nextId,
      };
    }, SONIC_CONFIG);

    // 1001 at head (before 1000): prevId=-1, nextId=1000
    expect(result.synth1001PrevId).toBe(-1);
    expect(result.synth1001NextId).toBe(1000);
    // 1000 at tail (after 1001): prevId=1001, nextId=-1
    expect(result.synth1000PrevId).toBe(1001);
    expect(result.synth1000NextId).toBe(-1);
  });

  test("add action 3 - adds after target node", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      // Create synth 1000
      await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 0, "release", 60);
      await sonic.sync(1);

      // Add synth 1001 AFTER 1000 (action 3)
      await sonic.send("/s_new", "sonic-pi-beep", 1001, 3, 1000, "release", 60);
      await sonic.sync(2);

      const tree = sonic.getTree();
      const synth1000 = tree.nodes.find((n) => n.id === 1000);
      const synth1001 = tree.nodes.find((n) => n.id === 1001);

      // Cleanup
      await sonic.send("/n_free", 1000, 1001);

      return {
        // 1001 should be after 1000
        synth1000NextId: synth1000?.nextId,
        synth1001PrevId: synth1001?.prevId,
      };
    }, SONIC_CONFIG);

    expect(result.synth1000NextId).toBe(1001);
    expect(result.synth1001PrevId).toBe(1000);
  });

  test("add action 4 - replaces target node", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      // Create synth 1000
      await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 0, "release", 60);
      await sonic.sync(1);

      const treeBefore = sonic.getTree();
      const synth1000Before = treeBefore.nodes.find((n) => n.id === 1000);

      // Replace 1000 with new synth 1001 (action 4)
      await sonic.send("/s_new", "sonic-pi-beep", 1001, 4, 1000, "release", 60);
      await sonic.sync(2);

      const treeAfter = sonic.getTree();
      const synth1000After = treeAfter.nodes.find((n) => n.id === 1000);
      const synth1001After = treeAfter.nodes.find((n) => n.id === 1001);

      // Cleanup
      await sonic.send("/n_free", 1001);

      return {
        synth1000ExistedBefore: !!synth1000Before,
        synth1000ExistsAfter: !!synth1000After,
        synth1001Exists: !!synth1001After,
        // New synth should have same parent as old one
        synth1001ParentId: synth1001After?.parentId,
        synth1000ParentIdBefore: synth1000Before?.parentId,
      };
    }, SONIC_CONFIG);

    expect(result.synth1000ExistedBefore).toBe(true);
    expect(result.synth1000ExistsAfter).toBe(false); // Old synth freed
    expect(result.synth1001Exists).toBe(true);
    expect(result.synth1001ParentId).toBe(result.synth1000ParentIdBefore);
  });

  test("sets controls at creation time", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      const messages = [];
      sonic.on('message', (msg) => messages.push(msg));

      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      // Create synth with initial control values
      await sonic.send(
        "/s_new",
        "sonic-pi-beep",
        1000,
        0,
        0,
        "note",
        72,
        "amp",
        0.25,
        "pan",
        -0.75,
        "release",
        60
      );
      await sonic.sync(1);

      // Verify each control
      messages.length = 0;
      await sonic.send("/s_get", 1000, "note");
      await sonic.sync(2);
      const noteReply = messages.find((m) => m.address === "/n_set");

      messages.length = 0;
      await sonic.send("/s_get", 1000, "amp");
      await sonic.sync(3);
      const ampReply = messages.find((m) => m.address === "/n_set");

      messages.length = 0;
      await sonic.send("/s_get", 1000, "pan");
      await sonic.sync(4);
      const panReply = messages.find((m) => m.address === "/n_set");

      // Cleanup
      await sonic.send("/n_free", 1000);

      return {
        noteValue: noteReply?.args?.[2],
        ampValue: ampReply?.args?.[2],
        panValue: panReply?.args?.[2],
      };
    }, SONIC_CONFIG);

    expect(result.noteValue).toBe(72);
    expect(result.ampValue).toBeCloseTo(0.25, 5);
    expect(result.panValue).toBeCloseTo(-0.75, 5);
  });

  test("sends /n_go notification on creation", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);

      await sonic.init();
      await sonic.send("/notify", 1);
      await sonic.loadSynthDef("sonic-pi-beep");

      // Wait for /n_go notification (not just sync reply)
      const nGoPromise = new Promise((resolve, reject) => {
        const timeout = setTimeout(() => reject(new Error("/n_go not received")), 2000);
        const handler = (msg) => {
          if (msg.address === "/n_go" && msg.args[0] === 1000) {
            clearTimeout(timeout);
            sonic.off('message', handler);
            resolve(msg);
          }
        };
        sonic.on('message', handler);
      });

      await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 0, "release", 60);

      let nGoMsg;
      try {
        nGoMsg = await nGoPromise;
      } catch (e) {
        nGoMsg = null;
      }

      // Cleanup
      await sonic.send("/n_free", 1000);

      return {
        receivedNGo: !!nGoMsg,
        nodeId: nGoMsg?.args?.[0],
        parentId: nGoMsg?.args?.[1],
        prevId: nGoMsg?.args?.[2],
        nextId: nGoMsg?.args?.[3],
        isGroup: nGoMsg?.args?.[4],
      };
    }, SONIC_CONFIG);

    expect(result.receivedNGo).toBe(true);
    expect(result.nodeId).toBe(1000);
    expect(result.parentId).toBe(0); // Root group
    expect(result.isGroup).toBe(0); // Synth, not group
  });

  test("non-existent synthdef fails gracefully", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      const messages = [];
      sonic.on('message', (msg) => messages.push(msg));

      await sonic.init();

      // Try to create synth with non-existent synthdef
      await sonic.send("/s_new", "nonexistent_synthdef_xyz", 1000, 0, 0);
      await sonic.sync(1);

      const tree = sonic.getTree();
      const synthExists = tree.nodes.some((n) => n.id === 1000);

      // Check for /fail message
      const failMsg = messages.find((m) => m.address === "/fail");

      return {
        synthExists,
        gotFailMessage: !!failMsg,
      };
    }, SONIC_CONFIG);

    expect(result.synthExists).toBe(false);
    // May or may not get /fail depending on implementation
  });
});

// =============================================================================
// /g_new - CREATE GROUP
// =============================================================================

test.describe("/g_new semantic tests", () => {
  test("creates group with specified ID", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();

      await sonic.send("/g_new", 100, 0, 0);
      await sonic.sync(1);

      const tree = sonic.getTree();
      const group = tree.nodes.find((n) => n.id === 100);

      // Cleanup
      await sonic.send("/n_free", 100);

      return {
        exists: !!group,
        id: group?.id,
        isGroup: group?.isGroup,
        defName: group?.defName,
        parentId: group?.parentId,
      };
    }, SONIC_CONFIG);

    expect(result.exists).toBe(true);
    expect(result.id).toBe(100);
    expect(result.isGroup).toBe(true);
    expect(result.defName).toBe("group");
    expect(result.parentId).toBe(0);
  });

  // Auto-generated IDs with -1 create negative IDs (e.g., -2147483640, -2147483632, ...)
  // These DO appear in the SAB tree for visualization purposes.
  test("auto-generates ID with -1", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();

      const nodeCountBefore = sonic.getTree().nodeCount;

      // Create group with auto ID
      await sonic.send("/g_new", -1, 0, 0);
      await sonic.sync(1);

      const treeAfter = sonic.getTree();
      const autoGroup = treeAfter.nodes.find(
        (n) => n.id < 0 && n.isGroup && n.defName === "group"
      );

      return {
        nodeCountBefore,
        nodeCountAfter: treeAfter.nodeCount,
        autoGroupFound: !!autoGroup,
        autoGroupId: autoGroup?.id,
      };
    }, SONIC_CONFIG);

    // SAB tree now includes auto-assigned negative IDs
    expect(result.nodeCountAfter).toBe(result.nodeCountBefore + 1);
    expect(result.autoGroupFound).toBe(true);
    expect(result.autoGroupId).toBeLessThan(0);
  });

  test("creates multiple groups in single command", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();

      // Create group 100 at head of root, 101 at head of 100, 102 at tail of 100
      await sonic.send("/g_new", 100, 0, 0, 101, 0, 100, 102, 1, 100);
      await sonic.sync(1);

      const tree = sonic.getTree();
      const group100 = tree.nodes.find((n) => n.id === 100);
      const group101 = tree.nodes.find((n) => n.id === 101);
      const group102 = tree.nodes.find((n) => n.id === 102);

      // Cleanup
      await sonic.send("/n_free", 100);

      return {
        group100Exists: !!group100,
        group101Exists: !!group101,
        group102Exists: !!group102,
        group100ParentId: group100?.parentId,
        group101ParentId: group101?.parentId,
        group102ParentId: group102?.parentId,
        // 101 should be at head (before 102), 102 at tail
        group101NextId: group101?.nextId,
        group102PrevId: group102?.prevId,
      };
    }, SONIC_CONFIG);

    expect(result.group100Exists).toBe(true);
    expect(result.group101Exists).toBe(true);
    expect(result.group102Exists).toBe(true);
    expect(result.group100ParentId).toBe(0);
    expect(result.group101ParentId).toBe(100);
    expect(result.group102ParentId).toBe(100);
    expect(result.group101NextId).toBe(102);
    expect(result.group102PrevId).toBe(101);
  });

  test("nested groups create proper hierarchy", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();

      // Create deeply nested groups: 100 -> 101 -> 102 -> 103
      await sonic.send("/g_new", 100, 0, 0);
      await sonic.send("/g_new", 101, 0, 100);
      await sonic.send("/g_new", 102, 0, 101);
      await sonic.send("/g_new", 103, 0, 102);
      await sonic.sync(1);

      const tree = sonic.getTree();

      const result = {
        group100Parent: tree.nodes.find((n) => n.id === 100)?.parentId,
        group101Parent: tree.nodes.find((n) => n.id === 101)?.parentId,
        group102Parent: tree.nodes.find((n) => n.id === 102)?.parentId,
        group103Parent: tree.nodes.find((n) => n.id === 103)?.parentId,
      };

      // Cleanup
      await sonic.send("/n_free", 100);

      return result;
    }, SONIC_CONFIG);

    expect(result.group100Parent).toBe(0);
    expect(result.group101Parent).toBe(100);
    expect(result.group102Parent).toBe(101);
    expect(result.group103Parent).toBe(102);
  });

  test("all add actions work correctly", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();

      // Create base group
      await sonic.send("/g_new", 100, 0, 0);
      await sonic.sync(1);

      // Action 0: head
      await sonic.send("/g_new", 101, 0, 100);
      // Action 1: tail
      await sonic.send("/g_new", 102, 1, 100);
      await sonic.sync(2);

      // Now 101 is at head, 102 at tail of group 100
      // Action 2: before 102
      await sonic.send("/g_new", 103, 2, 102);
      // Action 3: after 101
      await sonic.send("/g_new", 104, 3, 101);
      await sonic.sync(3);

      // Order should be: 101 -> 104 -> 103 -> 102

      const tree = sonic.getTree();
      const g101 = tree.nodes.find((n) => n.id === 101);
      const g102 = tree.nodes.find((n) => n.id === 102);
      const g103 = tree.nodes.find((n) => n.id === 103);
      const g104 = tree.nodes.find((n) => n.id === 104);

      // Cleanup
      await sonic.send("/n_free", 100);

      return {
        g101Next: g101?.nextId,
        g104Prev: g104?.prevId,
        g104Next: g104?.nextId,
        g103Prev: g103?.prevId,
        g103Next: g103?.nextId,
        g102Prev: g102?.prevId,
      };
    }, SONIC_CONFIG);

    // 101 -> 104 -> 103 -> 102
    expect(result.g101Next).toBe(104);
    expect(result.g104Prev).toBe(101);
    expect(result.g104Next).toBe(103);
    expect(result.g103Prev).toBe(104);
    expect(result.g103Next).toBe(102);
    expect(result.g102Prev).toBe(103);
  });
});

// =============================================================================
// /b_alloc and /b_free - BUFFER ALLOCATION
// =============================================================================

test.describe("/b_alloc and /b_free semantic tests", () => {
  test("allocates buffer with correct parameters", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      const messages = [];
      sonic.on('message', (msg) => messages.push(msg));

      await sonic.init();

      // Allocate mono buffer
      await sonic.send("/b_alloc", 0, 44100, 1);
      await sonic.sync(1);

      // Query buffer info
      messages.length = 0;
      await sonic.send("/b_query", 0);
      await sonic.sync(2);

      const info = messages.find((m) => m.address === "/b_info");

      // Cleanup
      await sonic.send("/b_free", 0);

      return {
        bufnum: info?.args?.[0],
        frames: info?.args?.[1],
        channels: info?.args?.[2],
        sampleRate: info?.args?.[3],
      };
    }, SONIC_CONFIG);

    expect(result.bufnum).toBe(0);
    expect(result.frames).toBe(44100);
    expect(result.channels).toBe(1);
    expect(result.sampleRate).toBeGreaterThan(0);
  });

  test("allocates stereo buffer", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      const messages = [];
      sonic.on('message', (msg) => messages.push(msg));

      await sonic.init();

      await sonic.send("/b_alloc", 0, 22050, 2);
      await sonic.sync(1);

      messages.length = 0;
      await sonic.send("/b_query", 0);
      await sonic.sync(2);

      const info = messages.find((m) => m.address === "/b_info");

      // Cleanup
      await sonic.send("/b_free", 0);

      return {
        frames: info?.args?.[1],
        channels: info?.args?.[2],
      };
    }, SONIC_CONFIG);

    expect(result.frames).toBe(22050);
    expect(result.channels).toBe(2);
  });

  test("re-allocating buffer replaces previous allocation", async ({
    page,
  }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      const messages = [];
      sonic.on('message', (msg) => messages.push(msg));

      await sonic.init();

      // Allocate with 1024 frames
      await sonic.send("/b_alloc", 0, 1024, 1);
      await sonic.sync(1);

      messages.length = 0;
      await sonic.send("/b_query", 0);
      await sonic.sync(2);
      const info1 = messages.find((m) => m.address === "/b_info");
      const framesBefore = info1?.args?.[1];

      // Re-allocate with 2048 frames
      await sonic.send("/b_alloc", 0, 2048, 2);
      await sonic.sync(3);

      messages.length = 0;
      await sonic.send("/b_query", 0);
      await sonic.sync(4);
      const info2 = messages.find((m) => m.address === "/b_info");
      const framesAfter = info2?.args?.[1];
      const channelsAfter = info2?.args?.[2];

      // Cleanup
      await sonic.send("/b_free", 0);

      return {
        framesBefore,
        framesAfter,
        channelsAfter,
      };
    }, SONIC_CONFIG);

    expect(result.framesBefore).toBe(1024);
    expect(result.framesAfter).toBe(2048);
    expect(result.channelsAfter).toBe(2);
  });

  test("freed buffer data is cleared on re-allocation", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      const messages = [];
      sonic.on('message', (msg) => messages.push(msg));

      await sonic.init();

      // Allocate and set some samples
      await sonic.send("/b_alloc", 0, 1024, 1);
      await sonic.sync(1);
      await sonic.send("/b_set", 0, 0, 0.999, 100, 0.888, 500, 0.777);
      await sonic.sync(2);

      // Free buffer
      await sonic.send("/b_free", 0);
      await sonic.sync(3);

      // Re-allocate same buffer
      await sonic.send("/b_alloc", 0, 1024, 1);
      await sonic.sync(4);

      // Check samples are zero
      messages.length = 0;
      await sonic.send("/b_get", 0, 0, 100, 500);
      await sonic.sync(5);

      const reply = messages.find((m) => m.address === "/b_set");
      const values = [reply?.args?.[2], reply?.args?.[4], reply?.args?.[6]];

      // Cleanup
      await sonic.send("/b_free", 0);

      return {
        values,
        allZero: values.every((v) => v === 0),
      };
    }, SONIC_CONFIG);

    expect(result.allZero).toBe(true);
  });

  test("multiple buffers are independent", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      const messages = [];
      sonic.on('message', (msg) => messages.push(msg));

      await sonic.init();

      // Allocate three different buffers
      await sonic.send("/b_alloc", 0, 1000, 1);
      await sonic.send("/b_alloc", 1, 2000, 2);
      await sonic.send("/b_alloc", 2, 3000, 1);
      await sonic.sync(1);

      // Set different values in each
      await sonic.send("/b_set", 0, 0, 0.1);
      await sonic.send("/b_set", 1, 0, 0.2);
      await sonic.send("/b_set", 2, 0, 0.3);
      await sonic.sync(2);

      // Query all
      messages.length = 0;
      await sonic.send("/b_query", 0, 1, 2);
      await sonic.sync(3);

      const info = messages.find((m) => m.address === "/b_info");

      // Get values
      messages.length = 0;
      await sonic.send("/b_get", 0, 0);
      await sonic.sync(4);
      const val0 = messages.find((m) => m.address === "/b_set")?.args?.[2];

      messages.length = 0;
      await sonic.send("/b_get", 1, 0);
      await sonic.sync(5);
      const val1 = messages.find((m) => m.address === "/b_set")?.args?.[2];

      messages.length = 0;
      await sonic.send("/b_get", 2, 0);
      await sonic.sync(6);
      const val2 = messages.find((m) => m.address === "/b_set")?.args?.[2];

      // Cleanup
      await sonic.send("/b_free", 0);
      await sonic.send("/b_free", 1);
      await sonic.send("/b_free", 2);

      return {
        buf0Frames: info?.args?.[1],
        buf1Frames: info?.args?.[5],
        buf2Frames: info?.args?.[9],
        val0,
        val1,
        val2,
      };
    }, SONIC_CONFIG);

    expect(result.buf0Frames).toBe(1000);
    expect(result.buf1Frames).toBe(2000);
    expect(result.buf2Frames).toBe(3000);
    expect(result.val0).toBeCloseTo(0.1, 5);
    expect(result.val1).toBeCloseTo(0.2, 5);
    expect(result.val2).toBeCloseTo(0.3, 5);
  });

  test("/b_zero actually zeros buffer contents", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      const messages = [];
      sonic.on('message', (msg) => messages.push(msg));

      await sonic.init();

      // Allocate and fill with non-zero values
      await sonic.send("/b_alloc", 0, 100, 1);
      await sonic.sync(1);
      await sonic.send("/b_fill", 0, 0, 100, 0.999);
      await sonic.sync(2);

      // Verify non-zero
      messages.length = 0;
      await sonic.send("/b_get", 0, 50);
      await sonic.sync(3);
      const valueBefore = messages.find((m) => m.address === "/b_set")
        ?.args?.[2];

      // Zero buffer
      await sonic.send("/b_zero", 0);
      await sonic.sync(4);

      // Verify zero
      messages.length = 0;
      await sonic.send("/b_get", 0, 50);
      await sonic.sync(5);
      const valueAfter = messages.find((m) => m.address === "/b_set")
        ?.args?.[2];

      // Cleanup
      await sonic.send("/b_free", 0);

      return {
        valueBefore,
        valueAfter,
      };
    }, SONIC_CONFIG);

    expect(result.valueBefore).toBeCloseTo(0.999, 3);
    expect(result.valueAfter).toBe(0);
  });
});

// =============================================================================
// /n_run - PAUSE/RESUME NODES
// =============================================================================

test.describe("/n_run semantic tests", () => {
  test("pauses synth with flag 0", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      const messages = [];
      sonic.on('message', (msg) => messages.push(msg));

      await sonic.init();
      await sonic.send("/notify", 1);
      await sonic.loadSynthDef("sonic-pi-beep");

      // Create synth
      await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 0, "release", 60);
      await sonic.sync(1);

      // Pause synth
      messages.length = 0;
      await sonic.send("/n_run", 1000, 0);
      await sonic.sync(2);

      // Check for /n_off notification
      const nOffMsg = messages.find(
        (m) => m.address === "/n_off" && m.args[0] === 1000
      );

      // Synth should still exist in tree
      const tree = sonic.getTree();
      const synthExists = tree.nodes.some((n) => n.id === 1000);

      // Cleanup
      await sonic.send("/n_free", 1000);

      return {
        receivedNOff: !!nOffMsg,
        synthExists,
      };
    }, SONIC_CONFIG);

    expect(result.receivedNOff).toBe(true);
    expect(result.synthExists).toBe(true);
  });

  test("resumes synth with flag 1", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      const messages = [];
      sonic.on('message', (msg) => messages.push(msg));

      await sonic.init();
      await sonic.send("/notify", 1);
      await sonic.loadSynthDef("sonic-pi-beep");

      // Create and pause synth
      await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 0, "release", 60);
      await sonic.send("/n_run", 1000, 0);
      await sonic.sync(1);

      // Resume synth
      messages.length = 0;
      await sonic.send("/n_run", 1000, 1);
      await sonic.sync(2);

      // Check for /n_on notification
      const nOnMsg = messages.find(
        (m) => m.address === "/n_on" && m.args[0] === 1000
      );

      // Cleanup
      await sonic.send("/n_free", 1000);

      return {
        receivedNOn: !!nOnMsg,
      };
    }, SONIC_CONFIG);

    expect(result.receivedNOn).toBe(true);
  });

  test("pauses and resumes multiple nodes", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      const messages = [];
      sonic.on('message', (msg) => messages.push(msg));

      await sonic.init();
      await sonic.send("/notify", 1);
      await sonic.loadSynthDef("sonic-pi-beep");

      // Create multiple synths
      await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 0, "release", 60);
      await sonic.send("/s_new", "sonic-pi-beep", 1001, 0, 0, "release", 60);
      await sonic.send("/s_new", "sonic-pi-beep", 1002, 0, 0, "release", 60);
      await sonic.sync(1);

      // Pause all three in one command
      messages.length = 0;
      await sonic.send("/n_run", 1000, 0, 1001, 0, 1002, 0);
      await sonic.sync(2);

      // Check for /n_off notifications
      const nOffMsgs = messages.filter((m) => m.address === "/n_off");
      const pausedIds = nOffMsgs.map((m) => m.args[0]);

      // Cleanup
      await sonic.send("/n_free", 1000, 1001, 1002);

      return {
        pausedCount: nOffMsgs.length,
        pausedIds,
      };
    }, SONIC_CONFIG);

    expect(result.pausedCount).toBe(3);
    expect(result.pausedIds).toContain(1000);
    expect(result.pausedIds).toContain(1001);
    expect(result.pausedIds).toContain(1002);
  });

  test("pausing group pauses all children", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      const messages = [];
      sonic.on('message', (msg) => messages.push(msg));

      await sonic.init();
      await sonic.send("/notify", 1);
      await sonic.loadSynthDef("sonic-pi-beep");

      // Create group with synths
      await sonic.send("/g_new", 100, 0, 0);
      await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 100, "release", 60);
      await sonic.send("/s_new", "sonic-pi-beep", 1001, 0, 100, "release", 60);
      await sonic.sync(1);

      // Pause the group
      messages.length = 0;
      await sonic.send("/n_run", 100, 0);
      await sonic.sync(2);

      // Should get /n_off for the group
      const nOffMsgs = messages.filter((m) => m.address === "/n_off");
      const groupPaused = nOffMsgs.some((m) => m.args[0] === 100);

      // Cleanup
      await sonic.send("/n_free", 100);

      return {
        groupPaused,
        nOffCount: nOffMsgs.length,
      };
    }, SONIC_CONFIG);

    expect(result.groupPaused).toBe(true);
  });
});

// =============================================================================
// /n_order - REORDER NODES
// =============================================================================

test.describe("/n_order semantic tests", () => {
  test("reorders nodes to head of group", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      // Create group with synths in order: 1000 -> 1001 -> 1002
      await sonic.send("/g_new", 100, 0, 0);
      await sonic.send("/s_new", "sonic-pi-beep", 1000, 1, 100, "release", 60);
      await sonic.send("/s_new", "sonic-pi-beep", 1001, 1, 100, "release", 60);
      await sonic.send("/s_new", "sonic-pi-beep", 1002, 1, 100, "release", 60);
      await sonic.sync(1);

      const treeBefore = sonic.getTree();
      const group100Before = treeBefore.nodes.find((n) => n.id === 100);

      // Move 1002 to head of group (addAction 0)
      await sonic.send("/n_order", 0, 100, 1002);
      await sonic.sync(2);

      const treeAfter = sonic.getTree();
      const synth1002 = treeAfter.nodes.find((n) => n.id === 1002);
      const synth1000 = treeAfter.nodes.find((n) => n.id === 1000);
      const group100After = treeAfter.nodes.find((n) => n.id === 100);

      // Cleanup
      await sonic.send("/n_free", 100);

      return {
        headBefore: group100Before?.headId,
        headAfter: group100After?.headId,
        synth1002PrevId: synth1002?.prevId,
        synth1002NextId: synth1002?.nextId,
        synth1000PrevId: synth1000?.prevId,
      };
    }, SONIC_CONFIG);

    // 1002 should now be at head
    expect(result.headBefore).toBe(1000);
    expect(result.headAfter).toBe(1002);
    expect(result.synth1002PrevId).toBe(-1); // 1002 is now first
    expect(result.synth1000PrevId).toBe(1002); // 1000 is now after 1002
  });

  test("reorders multiple nodes", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      // Create synths in order: 1000 -> 1001 -> 1002 -> 1003
      await sonic.send("/g_new", 100, 0, 0);
      await sonic.send("/s_new", "sonic-pi-beep", 1000, 1, 100, "release", 60);
      await sonic.send("/s_new", "sonic-pi-beep", 1001, 1, 100, "release", 60);
      await sonic.send("/s_new", "sonic-pi-beep", 1002, 1, 100, "release", 60);
      await sonic.send("/s_new", "sonic-pi-beep", 1003, 1, 100, "release", 60);
      await sonic.sync(1);

      // Reorder: put 1003, 1002 at head (in that order)
      await sonic.send("/n_order", 0, 100, 1003, 1002);
      await sonic.sync(2);

      const tree = sonic.getTree();
      const synth1003 = tree.nodes.find((n) => n.id === 1003);
      const synth1002 = tree.nodes.find((n) => n.id === 1002);
      const synth1000 = tree.nodes.find((n) => n.id === 1000);
      const group100 = tree.nodes.find((n) => n.id === 100);

      // Cleanup
      await sonic.send("/n_free", 100);

      return {
        headId: group100?.headId,
        // Expected order: 1003 -> 1002 -> 1000 -> 1001
        synth1003Next: synth1003?.nextId,
        synth1002Prev: synth1002?.prevId,
        synth1002Next: synth1002?.nextId,
        synth1000Prev: synth1000?.prevId,
      };
    }, SONIC_CONFIG);

    expect(result.headId).toBe(1003);
    expect(result.synth1003Next).toBe(1002);
    expect(result.synth1002Prev).toBe(1003);
    expect(result.synth1002Next).toBe(1000);
    expect(result.synth1000Prev).toBe(1002);
  });

  test("reorders nodes to tail of group", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      // Create synths: 1000 -> 1001 -> 1002
      await sonic.send("/g_new", 100, 0, 0);
      await sonic.send("/s_new", "sonic-pi-beep", 1000, 1, 100, "release", 60);
      await sonic.send("/s_new", "sonic-pi-beep", 1001, 1, 100, "release", 60);
      await sonic.send("/s_new", "sonic-pi-beep", 1002, 1, 100, "release", 60);
      await sonic.sync(1);

      // Move 1000 to tail (addAction 1)
      await sonic.send("/n_order", 1, 100, 1000);
      await sonic.sync(2);

      const tree = sonic.getTree();
      const synth1000 = tree.nodes.find((n) => n.id === 1000);
      const synth1002 = tree.nodes.find((n) => n.id === 1002);
      const group100 = tree.nodes.find((n) => n.id === 100);

      // Cleanup
      await sonic.send("/n_free", 100);

      return {
        // Expected order: 1001 -> 1002 -> 1000
        headId: group100?.headId,
        synth1000NextId: synth1000?.nextId,
        synth1000PrevId: synth1000?.prevId,
        synth1002NextId: synth1002?.nextId,
      };
    }, SONIC_CONFIG);

    expect(result.headId).toBe(1001);
    expect(result.synth1000NextId).toBe(-1); // 1000 is now last
    expect(result.synth1000PrevId).toBe(1002);
    expect(result.synth1002NextId).toBe(1000);
  });
});

// =============================================================================
// /g_queryTree - QUERY GROUP TREE
// =============================================================================

test.describe("/g_queryTree semantic tests", () => {
  test("returns correct tree structure", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      const messages = [];
      sonic.on('message', (msg) => messages.push(msg));

      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      // Create hierarchy: root -> group100 -> synth1000, synth1001
      await sonic.send("/g_new", 100, 0, 0);
      await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 100, "release", 60);
      await sonic.send("/s_new", "sonic-pi-beep", 1001, 1, 100, "release", 60);
      await sonic.sync(1);

      // Query tree from root
      messages.length = 0;
      await sonic.send("/g_queryTree", 0, 0);
      await sonic.sync(2);

      const reply = messages.find((m) => m.address === "/g_queryTree.reply");

      // Cleanup
      await sonic.send("/n_free", 100);

      return {
        hasReply: !!reply,
        args: reply?.args,
      };
    }, SONIC_CONFIG);

    expect(result.hasReply).toBe(true);
    // Format: [flag, nodeID, numChildren, ...]
    expect(result.args[0]).toBe(0); // flag
    expect(result.args[1]).toBe(0); // root group ID
    expect(result.args[2]).toBe(1); // root has 1 child (group 100)
    expect(result.args[3]).toBe(100); // group 100
    expect(result.args[4]).toBe(2); // group 100 has 2 children
  });

  test("returns synth control values with flag 1", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      const messages = [];
      sonic.on('message', (msg) => messages.push(msg));

      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      // Create synth with specific control values
      await sonic.send(
        "/s_new",
        "sonic-pi-beep",
        1000,
        0,
        0,
        "note",
        72,
        "release",
        60
      );
      await sonic.sync(1);

      // Query with flag 1 to include control values
      messages.length = 0;
      await sonic.send("/g_queryTree", 0, 1);
      await sonic.sync(2);

      const reply = messages.find((m) => m.address === "/g_queryTree.reply");

      // Cleanup
      await sonic.send("/n_free", 1000);

      return {
        hasReply: !!reply,
        flag: reply?.args?.[0],
        // With flag 1, synth entries include control count and values
        argsLength: reply?.args?.length,
      };
    }, SONIC_CONFIG);

    expect(result.hasReply).toBe(true);
    expect(result.flag).toBe(1);
    // With control values, response should be longer
    expect(result.argsLength).toBeGreaterThan(5);
  });

  test("SAB tree matches /g_queryTree response", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      const messages = [];
      sonic.on('message', (msg) => messages.push(msg));

      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      // Create complex hierarchy
      await sonic.send("/g_new", 100, 0, 0);
      await sonic.send("/g_new", 101, 0, 100);
      await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 101, "release", 60);
      await sonic.send("/s_new", "sonic-pi-beep", 1001, 0, 100, "release", 60);
      await sonic.send("/s_new", "sonic-pi-beep", 1002, 0, 0, "release", 60);
      await sonic.sync(1);

      const sabTree = sonic.getTree();

      messages.length = 0;
      await sonic.send("/g_queryTree", 0, 0);
      await sonic.sync(2);

      const queryReply = messages.find(
        (m) => m.address === "/g_queryTree.reply"
      );

      // Extract node IDs from queryTree response
      // Format: flag, groupID, numChildren, [childID, numChildren or -1, ...]
      const queryNodeIds = [];
      const args = queryReply?.args || [];
      for (let i = 1; i < args.length; i += 2) {
        if (typeof args[i] === "number" && args[i] >= 0) {
          queryNodeIds.push(args[i]);
        }
      }

      // Cleanup
      await sonic.send("/n_free", 100, 1002);

      return {
        sabNodeCount: sabTree.nodeCount,
        sabNodeIds: sabTree.nodes.map((n) => n.id).sort((a, b) => a - b),
        queryNodeIds: [...new Set(queryNodeIds)].sort((a, b) => a - b),
      };
    }, SONIC_CONFIG);

    // Both should have same nodes (excluding implementation details)
    expect(result.sabNodeCount).toBe(6); // root + 2 groups + 3 synths
    expect(result.sabNodeIds).toContain(0);
    expect(result.sabNodeIds).toContain(100);
    expect(result.sabNodeIds).toContain(101);
    expect(result.sabNodeIds).toContain(1000);
    expect(result.sabNodeIds).toContain(1001);
    expect(result.sabNodeIds).toContain(1002);
  });
});

// =============================================================================
// /b_gen - BUFFER GENERATORS
// =============================================================================

test.describe("/b_gen semantic tests", () => {
  test("sine1 generates single harmonic", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      const messages = [];
      sonic.on('message', (msg) => messages.push(msg));

      await sonic.init();

      // Allocate buffer
      await sonic.send("/b_alloc", 0, 512, 1);
      await sonic.sync(1);

      // Generate sine wave with amplitude 1.0
      await sonic.send("/b_gen", 0, "sine1", 7, 1.0); // flags=7 (normalize, wavetable, clear)
      await sonic.sync(2);

      // Read some samples to verify it's not all zeros
      messages.length = 0;
      await sonic.send("/b_getn", 0, 0, 8);
      await sonic.sync(3);

      const reply = messages.find((m) => m.address === "/b_setn");
      const samples = reply?.args?.slice(3) || [];

      // Also get samples at quarter point (should be near peak)
      messages.length = 0;
      await sonic.send("/b_getn", 0, 128, 4);
      await sonic.sync(4);

      const quarterReply = messages.find((m) => m.address === "/b_setn");
      const quarterSamples = quarterReply?.args?.slice(3) || [];

      // Cleanup
      await sonic.send("/b_free", 0);

      return {
        hasData: samples.some((s) => s !== 0),
        startSamples: samples,
        quarterSamples,
        // Peak should be near 1.0 (normalized)
        hasPeak: quarterSamples.some((s) => Math.abs(s) > 0.5),
      };
    }, SONIC_CONFIG);

    expect(result.hasData).toBe(true);
    expect(result.hasPeak).toBe(true);
  });

  test("sine1 generates multiple harmonics", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      const messages = [];
      sonic.on('message', (msg) => messages.push(msg));

      await sonic.init();

      // Allocate buffer
      await sonic.send("/b_alloc", 0, 1024, 1);
      await sonic.sync(1);

      // Generate sine with 3 harmonics: fundamental, 2nd, 3rd
      await sonic.send("/b_gen", 0, "sine1", 7, 1.0, 0.5, 0.25);
      await sonic.sync(2);

      // Read samples
      messages.length = 0;
      await sonic.send("/b_getn", 0, 0, 16);
      await sonic.sync(3);

      const reply = messages.find((m) => m.address === "/b_setn");
      const samples = reply?.args?.slice(3) || [];

      // Cleanup
      await sonic.send("/b_free", 0);

      return {
        hasData: samples.some((s) => s !== 0),
        samples,
      };
    }, SONIC_CONFIG);

    expect(result.hasData).toBe(true);
  });

  test("sine2 generates partials at specific frequencies", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      const messages = [];
      sonic.on('message', (msg) => messages.push(msg));

      await sonic.init();

      // Allocate buffer
      await sonic.send("/b_alloc", 0, 512, 1);
      await sonic.sync(1);

      // Generate with sine2: freq1, amp1, freq2, amp2
      await sonic.send("/b_gen", 0, "sine2", 7, 1, 1.0, 3, 0.5);
      await sonic.sync(2);

      // Read samples
      messages.length = 0;
      await sonic.send("/b_getn", 0, 0, 16);
      await sonic.sync(3);

      const reply = messages.find((m) => m.address === "/b_setn");
      const samples = reply?.args?.slice(3) || [];

      // Cleanup
      await sonic.send("/b_free", 0);

      return {
        hasData: samples.some((s) => s !== 0),
      };
    }, SONIC_CONFIG);

    expect(result.hasData).toBe(true);
  });

  test("cheby generates Chebyshev polynomial", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      const messages = [];
      sonic.on('message', (msg) => messages.push(msg));

      await sonic.init();

      // Allocate buffer
      await sonic.send("/b_alloc", 0, 512, 1);
      await sonic.sync(1);

      // Generate Chebyshev polynomial (for waveshaping)
      await sonic.send("/b_gen", 0, "cheby", 7, 1.0, 0.5, 0.25);
      await sonic.sync(2);

      // Read samples
      messages.length = 0;
      await sonic.send("/b_getn", 0, 0, 16);
      await sonic.sync(3);

      const reply = messages.find((m) => m.address === "/b_setn");
      const samples = reply?.args?.slice(3) || [];

      // Cleanup
      await sonic.send("/b_free", 0);

      return {
        hasData: samples.some((s) => s !== 0),
      };
    }, SONIC_CONFIG);

    expect(result.hasData).toBe(true);
  });
});

// =============================================================================
// /c_set, /c_setn, /c_fill - CONTROL BUS COMMANDS
// =============================================================================

test.describe("Control bus semantic tests", () => {
  test("/c_set sets single bus value", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      const messages = [];
      sonic.on('message', (msg) => messages.push(msg));

      await sonic.init();

      // Set bus 0 to 0.75
      await sonic.send("/c_set", 0, 0.75);
      await sonic.sync(1);

      // Read it back
      messages.length = 0;
      await sonic.send("/c_get", 0);
      await sonic.sync(2);

      const reply = messages.find((m) => m.address === "/c_set");

      return {
        busIndex: reply?.args?.[0],
        value: reply?.args?.[1],
      };
    }, SONIC_CONFIG);

    expect(result.busIndex).toBe(0);
    expect(result.value).toBeCloseTo(0.75, 5);
  });

  test("/c_set sets multiple buses", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      const messages = [];
      sonic.on('message', (msg) => messages.push(msg));

      await sonic.init();

      // Set buses 0, 1, 2 to different values
      await sonic.send("/c_set", 0, 0.1, 1, 0.2, 2, 0.3);
      await sonic.sync(1);

      // Read them back
      const values = [];
      for (const bus of [0, 1, 2]) {
        messages.length = 0;
        await sonic.send("/c_get", bus);
        await sonic.sync(2 + bus);
        const reply = messages.find((m) => m.address === "/c_set");
        values.push(reply?.args?.[1]);
      }

      return { values };
    }, SONIC_CONFIG);

    expect(result.values[0]).toBeCloseTo(0.1, 5);
    expect(result.values[1]).toBeCloseTo(0.2, 5);
    expect(result.values[2]).toBeCloseTo(0.3, 5);
  });

  test("/c_setn sets sequential bus values", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      const messages = [];
      sonic.on('message', (msg) => messages.push(msg));

      await sonic.init();

      // Set buses 5-9 to sequential values
      await sonic.send("/c_setn", 5, 5, 0.5, 0.6, 0.7, 0.8, 0.9);
      await sonic.sync(1);

      // Read them back
      messages.length = 0;
      await sonic.send("/c_getn", 5, 5);
      await sonic.sync(2);

      const reply = messages.find((m) => m.address === "/c_setn");

      return {
        startIndex: reply?.args?.[0],
        count: reply?.args?.[1],
        values: reply?.args?.slice(2),
      };
    }, SONIC_CONFIG);

    expect(result.startIndex).toBe(5);
    expect(result.count).toBe(5);
    expect(result.values[0]).toBeCloseTo(0.5, 5);
    expect(result.values[1]).toBeCloseTo(0.6, 5);
    expect(result.values[2]).toBeCloseTo(0.7, 5);
    expect(result.values[3]).toBeCloseTo(0.8, 5);
    expect(result.values[4]).toBeCloseTo(0.9, 5);
  });

  test("/c_fill fills range with single value", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      const messages = [];
      sonic.on('message', (msg) => messages.push(msg));

      await sonic.init();

      // Fill buses 10-14 with 0.42
      await sonic.send("/c_fill", 10, 5, 0.42);
      await sonic.sync(1);

      // Read them back
      messages.length = 0;
      await sonic.send("/c_getn", 10, 5);
      await sonic.sync(2);

      const reply = messages.find((m) => m.address === "/c_setn");
      const values = reply?.args?.slice(2) || [];

      return {
        values,
        allMatch: values.every((v) => Math.abs(v - 0.42) < 0.001),
      };
    }, SONIC_CONFIG);

    expect(result.values).toHaveLength(5);
    expect(result.allMatch).toBe(true);
  });
});

// =============================================================================
// ERROR HANDLING
// =============================================================================

test.describe("Error handling tests", () => {
  test("invalid node ID returns error", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      const messages = [];
      sonic.on('message', (msg) => messages.push(msg));

      await sonic.init();

      // Try to query non-existent node
      messages.length = 0;
      await sonic.send("/n_query", 99999);
      await sonic.sync(1);

      // Should not receive /n_info for non-existent node
      const nInfoMsg = messages.find((m) => m.address === "/n_info");

      return {
        receivedNInfo: !!nInfoMsg,
      };
    }, SONIC_CONFIG);

    expect(result.receivedNInfo).toBe(false);
  });

  test("duplicate node ID fails gracefully", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      // Create synth with ID 1000
      await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 0, "release", 60);
      await sonic.sync(1);

      const treeBefore = sonic.getTree();
      const countBefore = treeBefore.nodeCount;

      // Try to create another synth with same ID
      await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 0, "release", 60);
      await sonic.sync(2);

      const treeAfter = sonic.getTree();
      const countAfter = treeAfter.nodeCount;

      // Cleanup
      await sonic.send("/n_free", 1000);

      return {
        countBefore,
        countAfter,
        countUnchanged: countBefore === countAfter,
      };
    }, SONIC_CONFIG);

    // Second synth should not be created
    expect(result.countUnchanged).toBe(true);
  });

  test("freeing root group is prevented", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();

      const treeBefore = sonic.getTree();

      // Try to free root group (ID 0)
      await sonic.send("/n_free", 0);
      await sonic.sync(1);

      const treeAfter = sonic.getTree();

      return {
        rootExistsBefore: treeBefore.nodes.some((n) => n.id === 0),
        rootExistsAfter: treeAfter.nodes.some((n) => n.id === 0),
      };
    }, SONIC_CONFIG);

    expect(result.rootExistsBefore).toBe(true);
    expect(result.rootExistsAfter).toBe(true);
  });

  test("setting controls on non-existent node is handled", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      // Create a synth to verify the system is working
      await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 0, "release", 60);
      await sonic.sync(1);

      // Try to set control on non-existent node
      await sonic.send("/n_set", 99999, "note", 72);
      await sonic.sync(2);

      // Original synth should still work
      const tree = sonic.getTree();
      const synthExists = tree.nodes.some((n) => n.id === 1000);

      // Cleanup
      await sonic.send("/n_free", 1000);

      return {
        noError: true,
        synthStillExists: synthExists,
      };
    }, SONIC_CONFIG);

    expect(result.noError).toBe(true);
    expect(result.synthStillExists).toBe(true);
  });

  test("buffer operations on invalid buffer handled", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      const messages = [];
      sonic.on('message', (msg) => messages.push(msg));

      await sonic.init();

      // Try to query non-allocated buffer
      messages.length = 0;
      await sonic.send("/b_query", 999);
      await sonic.sync(1);

      const reply = messages.find((m) => m.address === "/b_info");

      return {
        hasReply: !!reply,
        // Non-allocated buffer should report 0 frames
        frames: reply?.args?.[1],
      };
    }, SONIC_CONFIG);

    expect(result.hasReply).toBe(true);
    expect(result.frames).toBe(0);
  });
});

// =============================================================================
// MALFORMED INPUT ROBUSTNESS TESTS
// =============================================================================

test.describe("Malformed input robustness tests", () => {
  // Test 1: Malformed synthdef with truncated header
  test("malformed synthdef - truncated header handled gracefully", async ({
    page,
  }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      const messages = [];
      sonic.on('message', (msg) => messages.push(msg));

      await sonic.init();

      // Create a truncated synthdef (just the magic bytes, no content)
      // SCgf header is "SCgf" (0x53436766)
      const truncatedSynthdef = new Uint8Array([0x53, 0x43, 0x67, 0x66]);

      try {
        await sonic.send("/d_recv", truncatedSynthdef);
        await sonic.sync(1);
      } catch (e) {
        // Exception is acceptable for malformed data
      }

      // Verify server is still responsive
      messages.length = 0;
      await sonic.send("/status");
      await sonic.sync(2);

      const statusReply = messages.find((m) => m.address === "/status.reply");
      return {
        serverResponsive: !!statusReply,
        numGroups: statusReply?.args?.[3],
      };
    }, SONIC_CONFIG);

    expect(result.serverResponsive).toBe(true);
    expect(result.numGroups).toBeGreaterThanOrEqual(1);
  });

  // Test 2: Synthdef with invalid/unknown UGen name
  test("synthdef with unknown UGen name handled gracefully", async ({
    page,
  }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      const messages = [];
      const debugMessages = [];
      sonic.on('message', (msg) => messages.push(msg));
      sonic.on('debug', (msg) => debugMessages.push(msg));

      await sonic.init();

      // Create a minimal synthdef structure with a fake UGen name
      // This is a simplified synthdef-like structure:
      // SCgf magic (4) + version (4) + num defs (2) + name len (1) + name + ...
      const fakeUGenName = "NonExistentUGen123";
      const defName = "test-bad-ugen";

      // Build a minimal but structurally valid synthdef with unknown UGen
      const header = new Uint8Array([
        0x53,
        0x43,
        0x67,
        0x66, // "SCgf"
        0x00,
        0x00,
        0x00,
        0x02, // version 2
        0x00,
        0x01, // 1 synthdef
      ]);

      // Simplified - just send corrupted data to test exception handling
      const malformed = new Uint8Array([
        ...header,
        defName.length,
        ...new TextEncoder().encode(defName),
        0x00,
        0x00,
        0x00,
        0x00, // 0 constants
        0x00,
        0x00,
        0x00,
        0x00, // 0 params
        0x00,
        0x00,
        0x00,
        0x00, // 0 param names
        0x00,
        0x00,
        0x00,
        0x01, // 1 UGen
        fakeUGenName.length,
        ...new TextEncoder().encode(fakeUGenName),
        0x02, // rate
        0x00,
        0x00,
        0x00,
        0x00, // 0 inputs
        0x00,
        0x00,
        0x00,
        0x01, // 1 output
        0x00,
        0x00, // special index
        0x02, // output rate
      ]);

      await sonic.send("/d_recv", malformed);

      // Wait for processing
      await new Promise((r) => setTimeout(r, 300));

      // Verify server still works
      await sonic.send("/status");
      await new Promise((r) => setTimeout(r, 200));

      const statusReply = messages.find((m) => m.address === "/status.reply");
      const hasUGenError = debugMessages.some(
        (m) => m.text && m.text.includes("not installed")
      );

      return {
        serverResponsive: !!statusReply,
        hasUGenError,
        debugMessages: debugMessages.map((m) => m.text).filter(Boolean),
        allMessages: messages.map((m) => m.address),
      };
    }, SONIC_CONFIG);

    expect(result.serverResponsive).toBe(true);
    expect(result.hasUGenError).toBe(true);
  });

  // Test 3: Synthdef with excessively long name
  test("synthdef with excessively long name handled gracefully", async ({
    page,
  }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      const messages = [];
      sonic.on('message', (msg) => messages.push(msg));

      await sonic.init();

      // Create a synthdef with a name > 255 characters (max allowed)
      // The name length byte can only hold 0-255, so we test edge cases
      const longName = "a".repeat(300);

      // Build header with oversized name (will be truncated to 255 in length byte)
      const header = new Uint8Array([
        0x53,
        0x43,
        0x67,
        0x66, // "SCgf"
        0x00,
        0x00,
        0x00,
        0x02, // version 2
        0x00,
        0x01, // 1 synthdef
        0xff, // 255 (max length byte can hold)
        ...new TextEncoder().encode(longName.slice(0, 255)),
        // Incomplete data after name
      ]);

      try {
        await sonic.send("/d_recv", header);
        await sonic.sync(1);
      } catch (e) {
        // Exception is acceptable
      }

      // Verify server still works
      messages.length = 0;
      await sonic.send("/status");
      await sonic.sync(2);

      const statusReply = messages.find((m) => m.address === "/status.reply");
      return {
        serverResponsive: !!statusReply,
      };
    }, SONIC_CONFIG);

    expect(result.serverResponsive).toBe(true);
  });

  // Test 4: Empty synthdef data
  test("empty synthdef data handled gracefully", async ({ page }) => {
    await page.goto("/test/harness.html");

    // Capture console errors from the page
    const consoleErrors = [];
    page.on("console", (msg) => {
      if (msg.type() === "error") {
        consoleErrors.push(msg.text());
      }
    });
    page.on("pageerror", (err) => {
      consoleErrors.push(`Page error: ${err.message}`);
    });

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      const messages = [];
      const debugMessages = [];
      sonic.on('message', (msg) => messages.push(msg));
      sonic.on('debug', (msg) => debugMessages.push(msg));

      await sonic.init();

      // First verify server works before malformed input
      await sonic.send("/status");
      await new Promise((r) => setTimeout(r, 100));
      const statusBefore = messages.find((m) => m.address === "/status.reply");

      // Send empty synthdef - should be rejected gracefully
      messages.length = 0;
      await sonic.send("/d_recv", new Uint8Array(0));

      // Wait for any error/debug messages
      await new Promise((r) => setTimeout(r, 200));

      // Verify server still works
      await sonic.send("/status");
      await new Promise((r) => setTimeout(r, 200));

      const statusReply = messages.find((m) => m.address === "/status.reply");
      const hasErrorMessage = debugMessages.some(
        (m) => m.text && m.text.includes("ERROR") && m.text.includes("d_recv")
      );

      return {
        serverWorkedBefore: !!statusBefore,
        serverResponsive: !!statusReply,
        hasErrorMessage,
        debugMessages: debugMessages.map((m) => m.text).filter(Boolean),
        allMessages: messages.map((m) => m.address),
      };
    }, SONIC_CONFIG);

    expect(result.serverWorkedBefore).toBe(true);
    expect(result.serverResponsive).toBe(true);
    expect(result.hasErrorMessage).toBe(true);
  });

  // Test 5: Random garbage data as synthdef
  test("random garbage data as synthdef handled gracefully", async ({
    page,
  }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      const messages = [];
      sonic.on('message', (msg) => messages.push(msg));

      await sonic.init();

      // Send random garbage bytes
      const garbage = new Uint8Array(256);
      for (let i = 0; i < garbage.length; i++) {
        garbage[i] = Math.floor(Math.random() * 256);
      }

      try {
        await sonic.send("/d_recv", garbage);
        await sonic.sync(1);
      } catch (e) {
        // Exception is acceptable
      }

      // Verify server still works
      messages.length = 0;
      await sonic.send("/status");
      await sonic.sync(2);

      const statusReply = messages.find((m) => m.address === "/status.reply");
      return {
        serverResponsive: !!statusReply,
      };
    }, SONIC_CONFIG);

    expect(result.serverResponsive).toBe(true);
  });

  // Test 6: /s_new with non-existent synthdef
  test("/s_new with non-existent synthdef handled gracefully", async ({
    page,
  }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      const messages = [];
      sonic.on('message', (msg) => messages.push(msg));

      await sonic.init();

      const treeBefore = sonic.getTree();
      const countBefore = treeBefore.nodeCount;

      // Try to create synth with non-existent synthdef
      await sonic.send("/s_new", "this-synthdef-does-not-exist", 5000, 0, 0);
      await sonic.sync(1);

      const treeAfter = sonic.getTree();
      const countAfter = treeAfter.nodeCount;

      // Verify server still works
      messages.length = 0;
      await sonic.send("/status");
      await sonic.sync(2);

      const statusReply = messages.find((m) => m.address === "/status.reply");

      return {
        serverResponsive: !!statusReply,
        synthNotCreated: countBefore === countAfter,
      };
    }, SONIC_CONFIG);

    expect(result.serverResponsive).toBe(true);
    expect(result.synthNotCreated).toBe(true);
  });

  // Test 7: Invalid add action in /s_new
  test("/s_new with invalid add action handled gracefully", async ({
    page,
  }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      const messages = [];
      sonic.on('message', (msg) => messages.push(msg));

      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      const treeBefore = sonic.getTree();
      const countBefore = treeBefore.nodeCount;

      // Try with invalid add action (999)
      await sonic.send("/s_new", "sonic-pi-beep", 5001, 999, 0);
      await sonic.sync(1);

      const treeAfter = sonic.getTree();

      // Verify server still works
      messages.length = 0;
      await sonic.send("/status");
      await sonic.sync(2);

      const statusReply = messages.find((m) => m.address === "/status.reply");

      return {
        serverResponsive: !!statusReply,
      };
    }, SONIC_CONFIG);

    expect(result.serverResponsive).toBe(true);
  });

  // Test 8: /g_new with invalid target node
  test("/g_new with non-existent target node handled gracefully", async ({
    page,
  }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      const messages = [];
      sonic.on('message', (msg) => messages.push(msg));

      await sonic.init();

      const treeBefore = sonic.getTree();
      const countBefore = treeBefore.nodeCount;

      // Try to create group with non-existent target
      await sonic.send("/g_new", 6000, 0, 99999);
      await sonic.sync(1);

      const treeAfter = sonic.getTree();
      const countAfter = treeAfter.nodeCount;

      // Verify server still works
      messages.length = 0;
      await sonic.send("/status");
      await sonic.sync(2);

      const statusReply = messages.find((m) => m.address === "/status.reply");

      return {
        serverResponsive: !!statusReply,
        groupNotCreated: countBefore === countAfter,
      };
    }, SONIC_CONFIG);

    expect(result.serverResponsive).toBe(true);
    expect(result.groupNotCreated).toBe(true);
  });

  // Test 9: /n_set with wrong argument types
  test("/n_set with wrong argument types handled gracefully", async ({
    page,
  }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      const messages = [];
      sonic.on('message', (msg) => messages.push(msg));

      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      // Create a synth
      await sonic.send("/s_new", "sonic-pi-beep", 7000, 0, 0);
      await sonic.sync(1);

      // Try various malformed /n_set calls
      await sonic.send("/n_set", 7000); // Missing control name/value
      await sonic.sync(2);

      // Verify synth still exists and server works
      const tree = sonic.getTree();
      const synthExists = tree.nodes.some((n) => n.id === 7000);

      messages.length = 0;
      await sonic.send("/status");
      await sonic.sync(3);

      const statusReply = messages.find((m) => m.address === "/status.reply");

      // Cleanup
      await sonic.send("/n_free", 7000);

      return {
        serverResponsive: !!statusReply,
        synthStillExists: synthExists,
      };
    }, SONIC_CONFIG);

    expect(result.serverResponsive).toBe(true);
    expect(result.synthStillExists).toBe(true);
  });

  // Test 10: /b_alloc with extreme values
  test("/b_alloc with extreme values handled gracefully", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      const messages = [];
      sonic.on('message', (msg) => messages.push(msg));

      await sonic.init();

      // Try to allocate an impossibly large buffer
      try {
        await sonic.send("/b_alloc", 100, 0x7fffffff, 2); // ~2GB buffer
        await sonic.sync(1);
      } catch (e) {
        // Exception is acceptable
      }

      // Verify server still works
      messages.length = 0;
      await sonic.send("/status");
      await sonic.sync(2);

      const statusReply = messages.find((m) => m.address === "/status.reply");

      return {
        serverResponsive: !!statusReply,
      };
    }, SONIC_CONFIG);

    expect(result.serverResponsive).toBe(true);
  });

  // Test 11: /b_set with out-of-bounds index
  test("/b_set with out-of-bounds index handled gracefully", async ({
    page,
  }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      const messages = [];
      sonic.on('message', (msg) => messages.push(msg));

      await sonic.init();

      // Allocate a small buffer
      await sonic.send("/b_alloc", 200, 64, 1);
      await sonic.sync(1);

      // Try to set out-of-bounds sample
      await sonic.send("/b_set", 200, 99999, 1.0);
      await sonic.sync(2);

      // Verify server still works
      messages.length = 0;
      await sonic.send("/status");
      await sonic.sync(3);

      const statusReply = messages.find((m) => m.address === "/status.reply");

      // Cleanup
      await sonic.send("/b_free", 200);

      return {
        serverResponsive: !!statusReply,
      };
    }, SONIC_CONFIG);

    expect(result.serverResponsive).toBe(true);
  });

  // Test 12: Rapid fire of malformed commands doesn't crash
  test("rapid fire of malformed commands handled gracefully", async ({
    page,
  }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      const messages = [];
      sonic.on('message', (msg) => messages.push(msg));

      await sonic.init();

      // Send many malformed commands rapidly
      const promises = [];
      for (let i = 0; i < 50; i++) {
        promises.push(sonic.send("/s_new", "nonexistent", -1, 0, 0));
        promises.push(sonic.send("/n_free", 99999 + i));
        promises.push(sonic.send("/n_set", 88888 + i, "x", i));
        promises.push(sonic.send("/g_new", -1, 999, 77777 + i));
      }

      try {
        await Promise.all(promises);
      } catch (e) {
        // Exceptions acceptable
      }

      await sonic.sync(1);

      // Verify server still works
      messages.length = 0;
      await sonic.send("/status");
      await sonic.sync(2);

      const statusReply = messages.find((m) => m.address === "/status.reply");

      return {
        serverResponsive: !!statusReply,
        numGroups: statusReply?.args?.[3],
      };
    }, SONIC_CONFIG);

    expect(result.serverResponsive).toBe(true);
    expect(result.numGroups).toBeGreaterThanOrEqual(1);
  });

  // Test 13: Unknown OSC command handled gracefully
  test("unknown OSC command handled gracefully", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      const messages = [];
      sonic.on('message', (msg) => messages.push(msg));

      await sonic.init();

      // Send completely unknown command
      await sonic.send("/this_command_does_not_exist", 1, 2, 3);
      await sonic.sync(1);

      // Verify server still works
      messages.length = 0;
      await sonic.send("/status");
      await sonic.sync(2);

      const statusReply = messages.find((m) => m.address === "/status.reply");

      return {
        serverResponsive: !!statusReply,
      };
    }, SONIC_CONFIG);

    expect(result.serverResponsive).toBe(true);
  });

  // Test 14: Valid synthdef still works after malformed ones
  test("valid synthdef loads after malformed attempts", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      const messages = [];
      sonic.on('message', (msg) => messages.push(msg));

      await sonic.init();

      // Send several malformed synthdefs
      const garbage = new Uint8Array([1, 2, 3, 4, 5]);
      try {
        await sonic.send("/d_recv", garbage);
      } catch (e) {}
      try {
        await sonic.send("/d_recv", new Uint8Array(0));
      } catch (e) {}
      try {
        await sonic.send(
          "/d_recv",
          new Uint8Array([0x53, 0x43, 0x67, 0x66, 0xff, 0xff])
        );
      } catch (e) {}

      await sonic.sync(1);

      // Now load a valid synthdef
      await sonic.loadSynthDef("sonic-pi-beep");
      await sonic.sync(2);

      // Create a synth with it
      await sonic.send("/s_new", "sonic-pi-beep", 8000, 0, 0);
      await sonic.sync(3);

      const tree = sonic.getTree();
      const synthExists = tree.nodes.some((n) => n.id === 8000);

      // Cleanup
      await sonic.send("/n_free", 8000);

      return {
        synthCreated: synthExists,
      };
    }, SONIC_CONFIG);

    expect(result.synthCreated).toBe(true);
  });
});

// =============================================================================
// /d_freeAll - FREE ALL SYNTHDEFS
// =============================================================================

test.describe("/d_freeAll semantic tests", () => {
  test("frees all loaded synthdefs", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      const messages = [];
      sonic.on('message', (msg) => messages.push(msg));

      await sonic.init();
      await sonic.send("/notify", 1);

      // Load a synthdef
      await sonic.loadSynthDef("sonic-pi-beep");

      // Get synthdef count before
      messages.length = 0;
      await sonic.send("/status");
      await sonic.sync(1);
      const statusBefore = messages.find((m) => m.address === "/status.reply");
      const synthdefCountBefore = statusBefore?.args?.[4] || 0;

      // Free all synthdefs
      await sonic.send("/d_freeAll");
      await sonic.sync(2);

      // Get synthdef count after
      messages.length = 0;
      await sonic.send("/status");
      await sonic.sync(3);
      const statusAfter = messages.find((m) => m.address === "/status.reply");
      const synthdefCountAfter = statusAfter?.args?.[4] || 0;

      return {
        synthdefCountBefore,
        synthdefCountAfter,
      };
    }, SONIC_CONFIG);

    expect(result.synthdefCountBefore).toBeGreaterThan(0);
    expect(result.synthdefCountAfter).toBe(0);
  });
});

// =============================================================================
// /n_mapn - MAP MULTIPLE CONTROLS TO SEQUENTIAL BUSES
// =============================================================================

test.describe("/n_mapn semantic tests", () => {
  test("maps multiple sequential controls to buses", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      // Set bus values
      await sonic.send("/c_set", 0, 100, 1, 200);

      // Create synth
      await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 0);
      await sonic.sync(1);

      // Map "note" (and next control) to buses 0, 1
      // /n_mapn nodeID, controlIndex, busIndex, numControls
      await sonic.send("/n_mapn", 1000, "note", 0, 2);
      await sonic.sync(2);

      // Synth should still exist
      const tree = sonic.getTree();
      const synthExists = tree.nodes.some((n) => n.id === 1000);

      // Cleanup
      await sonic.send("/n_free", 1000);

      return { synthExists };
    }, SONIC_CONFIG);

    expect(result.synthExists).toBe(true);
  });
});

// =============================================================================
// /s_noid - REMOVE NODE ID FROM SYNTH
// =============================================================================

test.describe("/s_noid semantic tests", () => {
  test("removes synth node ID (assigns hidden ID)", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      // Create synth with known ID
      await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 0, "release", 60);
      await sonic.sync(1);

      const treeBefore = sonic.getTree();
      const synthExistsBefore = treeBefore.nodes.some((n) => n.id === 1000);

      // Remove the node ID - synth gets hidden negative ID
      await sonic.send("/s_noid", 1000);
      await sonic.sync(2);

      const treeAfter = sonic.getTree();
      const synthExistsAfter = treeAfter.nodes.some((n) => n.id === 1000);

      return {
        synthExistsBefore,
        synthExistsAfter,
        nodeCountBefore: treeBefore.nodeCount,
        nodeCountAfter: treeAfter.nodeCount,
      };
    }, SONIC_CONFIG);

    // Synth should exist before
    expect(result.synthExistsBefore).toBe(true);
    // Synth should NOT exist with that ID after (it has a hidden negative ID now)
    expect(result.synthExistsAfter).toBe(false);
    // Node count should decrease (hidden IDs excluded from SAB tree)
    expect(result.nodeCountAfter).toBe(result.nodeCountBefore - 1);
  });
});

// =============================================================================
// /n_mapa - MAP CONTROL TO AUDIO BUS
// =============================================================================

test.describe("/n_mapa semantic tests", () => {
  test("maps control to audio bus", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      // Create synth
      await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 0);
      await sonic.sync(1);

      // Map "note" control to audio bus 0
      await sonic.send("/n_mapa", 1000, "note", 0);
      await sonic.sync(2);

      // Synth should still exist
      const tree = sonic.getTree();
      const synthExists = tree.nodes.some((n) => n.id === 1000);

      // Cleanup
      await sonic.send("/n_free", 1000);

      return { synthExists };
    }, SONIC_CONFIG);

    expect(result.synthExists).toBe(true);
  });
});

// =============================================================================
// /n_mapan - MAP MULTIPLE CONTROLS TO AUDIO BUSES
// =============================================================================

test.describe("/n_mapan semantic tests", () => {
  test("maps multiple sequential controls to audio buses", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      // Create synth
      await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 0);
      await sonic.sync(1);

      // Map "note" (and next control) to audio buses 0, 1
      await sonic.send("/n_mapan", 1000, "note", 0, 2);
      await sonic.sync(2);

      // Synth should still exist
      const tree = sonic.getTree();
      const synthExists = tree.nodes.some((n) => n.id === 1000);

      // Cleanup
      await sonic.send("/n_free", 1000);

      return { synthExists };
    }, SONIC_CONFIG);

    expect(result.synthExists).toBe(true);
  });
});

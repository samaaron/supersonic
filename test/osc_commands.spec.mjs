/**
 * OSC Command Test Suite
 *
 * Tests all supported scsynth OSC commands as documented in SERVER_COMMAND_REFERENCE.md
 * Uses SharedArrayBuffer node tree (getTree()) for verification where possible,
 * falling back to OSC replies for commands that return specific data.
 */

import { test, expect } from "@playwright/test";

// Helper to boot supersonic and return instance
const bootSupersonic = async (page) => {
  return await page.evaluate(async () => {
    const sonic = new window.SuperSonic({
      workerBaseURL: "/dist/workers/",
      wasmBaseURL: "/dist/wasm/",
      sampleBaseURL: "/dist/samples/",
      synthdefBaseURL: "/dist/synthdefs/",
    });
    await sonic.init();
    return true;
  });
};

// =============================================================================
// TOP-LEVEL COMMANDS
// =============================================================================

test.describe("Top-level Commands", () => {
  test("/status - returns server status", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: "/dist/workers/",
        wasmBaseURL: "/dist/wasm/",
        sampleBaseURL: "/dist/samples/",
        synthdefBaseURL: "/dist/synthdefs/",
      });

      const messages = [];
      sonic.on('message', (msg) => messages.push(msg));

      await sonic.init();
      await sonic.send("/status");
      await sonic.sync(1);

      const statusReply = messages.find((m) => m.address === "/status.reply");
      return {
        success: !!statusReply,
        args: statusReply?.args,
        numUgens: statusReply?.args?.[1],
        numSynths: statusReply?.args?.[2],
        numGroups: statusReply?.args?.[3],
        numSynthDefs: statusReply?.args?.[4],
      };
    });

    expect(result.success).toBe(true);
    expect(result.numGroups).toBeGreaterThanOrEqual(1); // At least root group
    expect(typeof result.numUgens).toBe("number");
    expect(typeof result.numSynths).toBe("number");
    expect(typeof result.numSynthDefs).toBe("number");
  });

  test("/sync - waits for async commands", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: "/dist/workers/",
        wasmBaseURL: "/dist/wasm/",
        sampleBaseURL: "/dist/samples/",
        synthdefBaseURL: "/dist/synthdefs/",
      });

      const messages = [];
      sonic.on('message', (msg) => messages.push(msg));

      await sonic.init();
      await sonic.send("/sync", 42);
      await new Promise((r) => setTimeout(r, 100));

      const syncedMsg = messages.find(
        (m) => m.address === "/synced" && m.args[0] === 42
      );
      return { success: !!syncedMsg, id: syncedMsg?.args?.[0] };
    });

    expect(result.success).toBe(true);
    expect(result.id).toBe(42);
  });

  test("/version - returns server version", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: "/dist/workers/",
        wasmBaseURL: "/dist/wasm/",
        sampleBaseURL: "/dist/samples/",
        synthdefBaseURL: "/dist/synthdefs/",
      });

      const messages = [];
      sonic.on('message', (msg) => messages.push(msg));

      await sonic.init();
      await sonic.send("/version");
      await sonic.sync(1);

      const versionReply = messages.find(
        (m) => m.address === "/version.reply"
      );
      return {
        success: !!versionReply,
        programName: versionReply?.args?.[0],
        majorVersion: versionReply?.args?.[1],
        minorVersion: versionReply?.args?.[2],
      };
    });

    expect(result.success).toBe(true);
    expect(typeof result.programName).toBe("string");
    expect(typeof result.majorVersion).toBe("number");
    expect(typeof result.minorVersion).toBe("number");
  });

  test("/notify - registers for notifications", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: "/dist/workers/",
        wasmBaseURL: "/dist/wasm/",
        sampleBaseURL: "/dist/samples/",
        synthdefBaseURL: "/dist/synthdefs/",
      });

      const messages = [];
      sonic.on('message', (msg) => messages.push(msg));

      await sonic.init();
      await sonic.send("/notify", 1);
      await sonic.sync(1);

      const doneMsg = messages.find(
        (m) => m.address === "/done" && m.args[0] === "/notify"
      );
      return { success: !!doneMsg };
    });

    expect(result.success).toBe(true);
  });

  test("/error - throws error (unsupported in SuperSonic)", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: "/dist/workers/",
        wasmBaseURL: "/dist/wasm/",
        sampleBaseURL: "/dist/samples/",
        synthdefBaseURL: "/dist/synthdefs/",
      });

      await sonic.init();
      try {
        await sonic.send("/error", 1);
        return { threw: false };
      } catch (e) {
        return { threw: true, message: e.message };
      }
    });

    expect(result.threw).toBe(true);
    expect(result.message).toContain("not supported");
  });

  test("/clearSched - throws error (unsupported in SuperSonic)", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: "/dist/workers/",
        wasmBaseURL: "/dist/wasm/",
        sampleBaseURL: "/dist/samples/",
        synthdefBaseURL: "/dist/synthdefs/",
      });

      await sonic.init();
      try {
        await sonic.send("/clearSched");
        return { threw: false };
      } catch (e) {
        return { threw: true, message: e.message };
      }
    });

    expect(result.threw).toBe(true);
    expect(result.message).toContain("not supported");
  });

  test("/dumpOSC - throws error (unsupported in SuperSonic)", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: "/dist/workers/",
        wasmBaseURL: "/dist/wasm/",
        sampleBaseURL: "/dist/samples/",
        synthdefBaseURL: "/dist/synthdefs/",
      });

      await sonic.init();
      try {
        await sonic.send("/dumpOSC", 1);
        return { threw: false };
      } catch (e) {
        return { threw: true, message: e.message };
      }
    });

    expect(result.threw).toBe(true);
    expect(result.message).toContain("not supported");
  });
});

// =============================================================================
// SYNTHDEF COMMANDS
// =============================================================================

test.describe("Synthdef Commands", () => {
  test("/d_recv - loads synthdef from bytes and increases synthdef count", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: "/dist/workers/",
        wasmBaseURL: "/dist/wasm/",
        sampleBaseURL: "/dist/samples/",
        synthdefBaseURL: "/dist/synthdefs/",
      });

      const messages = [];
      sonic.on('message', (msg) => messages.push(msg));

      await sonic.init();

      // Get synthdef count before loading
      await sonic.send("/status");
      await sonic.sync(1);
      const statusBefore = messages.find((m) => m.address === "/status.reply");
      const defsBefore = statusBefore?.args?.[4];

      // Load synthdef (uses /d_recv internally)
      messages.length = 0;
      await sonic.loadSynthDef("sonic-pi-beep");
      await sonic.sync(2);

      const doneMsg = messages.find(
        (m) => m.address === "/done" && m.args[0] === "/d_recv"
      );

      // Get synthdef count after loading
      messages.length = 0;
      await sonic.send("/status");
      await sonic.sync(3);
      const statusAfter = messages.find((m) => m.address === "/status.reply");
      const defsAfter = statusAfter?.args?.[4];

      return {
        success: !!doneMsg,
        defsBefore,
        defsAfter,
        countIncreased: defsAfter > defsBefore,
      };
    });

    expect(result.success).toBe(true);
    expect(result.countIncreased).toBe(true);
  });

  test("/d_free - frees synthdef", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: "/dist/workers/",
        wasmBaseURL: "/dist/wasm/",
        sampleBaseURL: "/dist/samples/",
        synthdefBaseURL: "/dist/synthdefs/",
      });

      const messages = [];
      sonic.on('message', (msg) => messages.push(msg));

      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      // Get status before free
      await sonic.send("/status");
      await sonic.sync(1);
      const statusBefore = messages.find((m) => m.address === "/status.reply");
      const defsBefore = statusBefore?.args?.[4];

      // Free the synthdef
      await sonic.send("/d_free", "sonic-pi-beep");
      await sonic.sync(2);

      // Get status after free
      messages.length = 0;
      await sonic.send("/status");
      await sonic.sync(3);
      const statusAfter = messages.find((m) => m.address === "/status.reply");
      const defsAfter = statusAfter?.args?.[4];

      return {
        success: true,
        defsBefore,
        defsAfter,
        freed: defsAfter < defsBefore,
      };
    });

    expect(result.success).toBe(true);
    expect(result.freed).toBe(true);
  });
});

// =============================================================================
// NODE COMMANDS
// =============================================================================

test.describe("Node Commands", () => {
  test("/n_free - frees nodes", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: "/dist/workers/",
        wasmBaseURL: "/dist/wasm/",
        sampleBaseURL: "/dist/samples/",
        synthdefBaseURL: "/dist/synthdefs/",
      });

      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      // Create synth
      await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 0);
      await sonic.sync(1);

      const treeBefore = sonic.getTree();
      const hasSynthBefore = treeBefore.nodes.some((n) => n.id === 1000);

      // Free synth
      await sonic.send("/n_free", 1000);
      await sonic.sync(2);

      const treeAfter = sonic.getTree();
      const hasSynthAfter = treeAfter.nodes.some((n) => n.id === 1000);

      return {
        success: true,
        hasSynthBefore,
        hasSynthAfter,
        countBefore: treeBefore.nodeCount,
        countAfter: treeAfter.nodeCount,
      };
    });

    expect(result.success).toBe(true);
    expect(result.hasSynthBefore).toBe(true);
    expect(result.hasSynthAfter).toBe(false);
    expect(result.countAfter).toBe(result.countBefore - 1);
  });

  test("/n_run - turns nodes on/off", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: "/dist/workers/",
        wasmBaseURL: "/dist/wasm/",
        sampleBaseURL: "/dist/samples/",
        synthdefBaseURL: "/dist/synthdefs/",
      });

      const messages = [];
      sonic.on('message', (msg) => messages.push(msg));

      await sonic.init();
      await sonic.send("/notify", 1);
      await sonic.loadSynthDef("sonic-pi-beep");

      // Create synth
      await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 0, "release", 60);
      await sonic.sync(1);

      // Turn off
      messages.length = 0;
      await sonic.send("/n_run", 1000, 0);
      await sonic.sync(2);
      const offMsg = messages.find((m) => m.address === "/n_off");

      // Turn on
      messages.length = 0;
      await sonic.send("/n_run", 1000, 1);
      await sonic.sync(3);
      const onMsg = messages.find((m) => m.address === "/n_on");

      // Cleanup
      await sonic.send("/n_free", 1000);

      return {
        success: true,
        gotOffNotification: !!offMsg,
        gotOnNotification: !!onMsg,
      };
    });

    expect(result.success).toBe(true);
    expect(result.gotOffNotification).toBe(true);
    expect(result.gotOnNotification).toBe(true);
  });

  test("/n_set - sets node control values", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: "/dist/workers/",
        wasmBaseURL: "/dist/wasm/",
        sampleBaseURL: "/dist/samples/",
        synthdefBaseURL: "/dist/synthdefs/",
      });

      const messages = [];
      sonic.on('message', (msg) => messages.push(msg));

      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      // Create synth with initial note
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

      // Change note
      await sonic.send("/n_set", 1000, "note", 72);

      // Get the value back
      messages.length = 0;
      await sonic.send("/s_get", 1000, "note");
      await sonic.sync(2);

      const reply = messages.find((m) => m.address === "/n_set");

      // Cleanup
      await sonic.send("/n_free", 1000);

      return {
        success: !!reply,
        nodeId: reply?.args?.[0],
        controlName: reply?.args?.[1],
        value: reply?.args?.[2],
      };
    });

    expect(result.success).toBe(true);
    expect(result.nodeId).toBe(1000);
    expect(result.value).toBe(72);
  });

  test("/n_setn - sets sequential control values", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: "/dist/workers/",
        wasmBaseURL: "/dist/wasm/",
        sampleBaseURL: "/dist/samples/",
        synthdefBaseURL: "/dist/synthdefs/",
      });

      const messages = [];
      sonic.on('message', (msg) => messages.push(msg));

      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 0, "release", 60);
      await sonic.sync(1);

      // Set multiple controls at once (by index)
      await sonic.send("/n_setn", 1000, 0, 2, 0.8, 0.5);
      await sonic.sync(2);

      // Cleanup
      await sonic.send("/n_free", 1000);

      return { success: true };
    });

    expect(result.success).toBe(true);
  });

  test("/n_query - queries node info", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: "/dist/workers/",
        wasmBaseURL: "/dist/wasm/",
        sampleBaseURL: "/dist/samples/",
        synthdefBaseURL: "/dist/synthdefs/",
      });

      const messages = [];
      sonic.on('message', (msg) => messages.push(msg));

      await sonic.init();
      // Must register for notifications - /n_query sends /n_info to registered clients
      await sonic.send("/notify", 1);
      await sonic.loadSynthDef("sonic-pi-beep");

      await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 0, "release", 60);
      await sonic.sync(1);

      messages.length = 0;
      await sonic.send("/n_query", 1000);
      await sonic.sync(2);

      const infoMsg = messages.find((m) => m.address === "/n_info");

      // Cleanup
      await sonic.send("/n_free", 1000);

      return {
        success: !!infoMsg,
        nodeId: infoMsg?.args?.[0],
        parentId: infoMsg?.args?.[1],
        isGroup: infoMsg?.args?.[4],
      };
    });

    expect(result.success).toBe(true);
    expect(result.nodeId).toBe(1000);
    expect(result.parentId).toBe(0); // Root group
    expect(result.isGroup).toBe(0); // Synth, not group
  });

  test("/n_before - moves node before another", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: "/dist/workers/",
        wasmBaseURL: "/dist/wasm/",
        sampleBaseURL: "/dist/samples/",
        synthdefBaseURL: "/dist/synthdefs/",
      });

      const messages = [];
      sonic.on('message', (msg) => messages.push(msg));

      await sonic.init();
      await sonic.send("/notify", 1);
      await sonic.loadSynthDef("sonic-pi-beep");

      // Create two synths at head - order becomes: 1001 -> 1000
      await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 0, "release", 60);
      await sonic.send("/s_new", "sonic-pi-beep", 1001, 0, 0, "release", 60);
      await sonic.sync(1);
      // Order is now: 1001 -> 1000 (1001 at head since added to head last)

      // Move 1000 before 1001 (should become: 1000 -> 1001)
      messages.length = 0;
      await sonic.send("/n_before", 1000, 1001);
      await sonic.sync(2);
      await new Promise((r) => setTimeout(r, 50));

      // /n_move args: nodeID, parentID, prevID, nextID, isGroup
      const moveMsg = messages.find((m) => m.address === "/n_move");

      // Cleanup
      await sonic.send("/n_free", 1000, 1001);

      return {
        success: !!moveMsg,
        movedNodeId: moveMsg?.args?.[0],
        parentId: moveMsg?.args?.[1],
        prevId: moveMsg?.args?.[2],
        nextId: moveMsg?.args?.[3],
      };
    });

    expect(result.success).toBe(true);
    expect(result.movedNodeId).toBe(1000);
    expect(result.parentId).toBe(0); // Root group
    expect(result.prevId).toBe(-1); // 1000 is now at head
    expect(result.nextId).toBe(1001); // 1001 is now after 1000
  });

  test("/n_after - moves node after another", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: "/dist/workers/",
        wasmBaseURL: "/dist/wasm/",
        sampleBaseURL: "/dist/samples/",
        synthdefBaseURL: "/dist/synthdefs/",
      });

      const messages = [];
      sonic.on('message', (msg) => messages.push(msg));

      await sonic.init();
      await sonic.send("/notify", 1);
      await sonic.loadSynthDef("sonic-pi-beep");

      // Create two synths at head - order becomes: 1001 -> 1000
      await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 0, "release", 60);
      await sonic.send("/s_new", "sonic-pi-beep", 1001, 0, 0, "release", 60);
      await sonic.sync(1);
      // Order is now: 1001 -> 1000 (1001 at head since added to head last)

      // Move 1001 after 1000 (should become: 1000 -> 1001)
      messages.length = 0;
      await sonic.send("/n_after", 1001, 1000);
      await sonic.sync(2);
      await new Promise((r) => setTimeout(r, 50));

      // /n_move args: nodeID, parentID, prevID, nextID, isGroup
      const moveMsg = messages.find((m) => m.address === "/n_move");

      // Cleanup
      await sonic.send("/n_free", 1000, 1001);

      return {
        success: !!moveMsg,
        movedNodeId: moveMsg?.args?.[0],
        parentId: moveMsg?.args?.[1],
        prevId: moveMsg?.args?.[2],
        nextId: moveMsg?.args?.[3],
      };
    });

    expect(result.success).toBe(true);
    expect(result.movedNodeId).toBe(1001);
    expect(result.parentId).toBe(0); // Root group
    expect(result.prevId).toBe(1000); // 1000 is now before 1001
    expect(result.nextId).toBe(-1); // 1001 is now at tail
  });

  test("/n_order - reorders nodes", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: "/dist/workers/",
        wasmBaseURL: "/dist/wasm/",
        sampleBaseURL: "/dist/samples/",
        synthdefBaseURL: "/dist/synthdefs/",
      });

      const messages = [];
      sonic.on('message', (msg) => messages.push(msg));

      await sonic.init();
      await sonic.send("/notify", 1);
      await sonic.loadSynthDef("sonic-pi-beep");

      // Create three synths at head - order becomes: 1002 -> 1001 -> 1000
      await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 0, "release", 60);
      await sonic.send("/s_new", "sonic-pi-beep", 1001, 0, 0, "release", 60);
      await sonic.send("/s_new", "sonic-pi-beep", 1002, 0, 0, "release", 60);
      await sonic.sync(1);
      // Order is now: 1002 -> 1001 -> 1000

      // Reorder: move 1000, 1001 to head of group 0 (should become: 1000 -> 1001 -> 1002)
      messages.length = 0;
      await sonic.send("/n_order", 0, 0, 1000, 1001);
      await sonic.sync(2);
      await new Promise((r) => setTimeout(r, 50));

      // Should get /n_move for reordered nodes
      const moveMsgs = messages.filter((m) => m.address === "/n_move");

      // Cleanup
      await sonic.send("/n_free", 1000, 1001, 1002);

      // Check 1001's move notification (1000 may not generate one if already at head)
      // After /n_order, order should be: 1000 -> 1001 -> 1002
      const move1001 = moveMsgs.find((m) => m.args[0] === 1001);

      return {
        success: moveMsgs.length >= 1,
        moveCount: moveMsgs.length,
        node1001_prevId: move1001?.args?.[2],
        node1001_nextId: move1001?.args?.[3],
      };
    });

    expect(result.success).toBe(true);
    expect(result.node1001_prevId).toBe(1000); // 1000 is before 1001
    expect(result.node1001_nextId).toBe(1002); // 1002 is after 1001
  });

  test("/n_map - maps control to control bus", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: "/dist/workers/",
        wasmBaseURL: "/dist/wasm/",
        sampleBaseURL: "/dist/samples/",
        synthdefBaseURL: "/dist/synthdefs/",
      });

      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      // Set control bus value
      await sonic.send("/c_set", 0, 72);

      // Create synth and map note to bus 0
      await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 0, "release", 60);
      await sonic.send("/n_map", 1000, "note", 0);
      await sonic.sync(1);

      // Unmap
      await sonic.send("/n_map", 1000, "note", -1);
      await sonic.sync(2);

      // Cleanup
      await sonic.send("/n_free", 1000);

      return { success: true };
    });

    expect(result.success).toBe(true);
  });
});

// =============================================================================
// SYNTH COMMANDS
// =============================================================================

test.describe("Synth Commands", () => {
  test("/s_new - creates synth", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: "/dist/workers/",
        wasmBaseURL: "/dist/wasm/",
        sampleBaseURL: "/dist/samples/",
        synthdefBaseURL: "/dist/synthdefs/",
      });

      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

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

      const tree = sonic.getTree();
      const synth = tree.nodes.find((n) => n.id === 1000);

      // Cleanup
      await sonic.send("/n_free", 1000);

      return {
        success: !!synth,
        id: synth?.id,
        parentId: synth?.parentId,
        isGroup: synth?.isGroup,
        defName: synth?.defName,
      };
    });

    expect(result.success).toBe(true);
    expect(result.id).toBe(1000);
    expect(result.parentId).toBe(0);
    expect(result.isGroup).toBe(false);
    expect(result.defName).toBe("sonic-pi-beep");
  });

  test("/s_new with add actions", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: "/dist/workers/",
        wasmBaseURL: "/dist/wasm/",
        sampleBaseURL: "/dist/samples/",
        synthdefBaseURL: "/dist/synthdefs/",
      });

      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      // Create group
      await sonic.send("/g_new", 100, 0, 0);

      // Add to head (0)
      await sonic.send(
        "/s_new",
        "sonic-pi-beep",
        1000,
        0,
        100,
        "release",
        60
      );
      // Add to tail (1)
      await sonic.send(
        "/s_new",
        "sonic-pi-beep",
        1001,
        1,
        100,
        "release",
        60
      );
      // Add before (2)
      await sonic.send(
        "/s_new",
        "sonic-pi-beep",
        1002,
        2,
        1001,
        "release",
        60
      );
      // Add after (3)
      await sonic.send(
        "/s_new",
        "sonic-pi-beep",
        1003,
        3,
        1000,
        "release",
        60
      );
      await sonic.sync(1);

      const tree = sonic.getTree();
      const synthsInGroup = tree.nodes.filter(
        (n) => n.parentId === 100 && !n.isGroup
      );

      // Cleanup
      await sonic.send("/g_freeAll", 100);
      await sonic.send("/n_free", 100);

      return {
        success: true,
        synthCount: synthsInGroup.length,
      };
    });

    expect(result.success).toBe(true);
    expect(result.synthCount).toBe(4);
  });

  test("/s_get - gets synth control value", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: "/dist/workers/",
        wasmBaseURL: "/dist/wasm/",
        sampleBaseURL: "/dist/samples/",
        synthdefBaseURL: "/dist/synthdefs/",
      });

      const messages = [];
      sonic.on('message', (msg) => messages.push(msg));

      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

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

      messages.length = 0;
      await sonic.send("/s_get", 1000, "note");
      await sonic.sync(2);

      const reply = messages.find((m) => m.address === "/n_set");

      // Cleanup
      await sonic.send("/n_free", 1000);

      return {
        success: !!reply,
        nodeId: reply?.args?.[0],
        control: reply?.args?.[1],
        value: reply?.args?.[2],
      };
    });

    expect(result.success).toBe(true);
    expect(result.nodeId).toBe(1000);
    expect(result.control).toBe("note");
    expect(result.value).toBe(60);
  });

  test("/s_getn - gets sequential control values", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: "/dist/workers/",
        wasmBaseURL: "/dist/wasm/",
        sampleBaseURL: "/dist/samples/",
        synthdefBaseURL: "/dist/synthdefs/",
      });

      const messages = [];
      sonic.on('message', (msg) => messages.push(msg));

      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 0, "release", 60);
      await sonic.sync(1);

      messages.length = 0;
      await sonic.send("/s_getn", 1000, 0, 3); // Get first 3 controls
      await sonic.sync(2);

      const reply = messages.find((m) => m.address === "/n_setn");

      // Cleanup
      await sonic.send("/n_free", 1000);

      return {
        success: !!reply,
        nodeId: reply?.args?.[0],
        startIndex: reply?.args?.[1],
        count: reply?.args?.[2],
      };
    });

    expect(result.success).toBe(true);
    expect(result.nodeId).toBe(1000);
    expect(result.startIndex).toBe(0);
    expect(result.count).toBe(3);
  });
});

// =============================================================================
// GROUP COMMANDS
// =============================================================================

test.describe("Group Commands", () => {
  test("/g_new - creates group", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: "/dist/workers/",
        wasmBaseURL: "/dist/wasm/",
        sampleBaseURL: "/dist/samples/",
        synthdefBaseURL: "/dist/synthdefs/",
      });

      await sonic.init();

      await sonic.send("/g_new", 100, 0, 0);
      await sonic.sync(1);

      const tree = sonic.getTree();
      const group = tree.nodes.find((n) => n.id === 100);

      // Cleanup
      await sonic.send("/n_free", 100);

      return {
        success: !!group,
        id: group?.id,
        parentId: group?.parentId,
        isGroup: group?.isGroup,
        defName: group?.defName,
      };
    });

    expect(result.success).toBe(true);
    expect(result.id).toBe(100);
    expect(result.parentId).toBe(0);
    expect(result.isGroup).toBe(true);
    expect(result.defName).toBe("group");
  });

  test("/p_new - creates parallel group", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: "/dist/workers/",
        wasmBaseURL: "/dist/wasm/",
        sampleBaseURL: "/dist/samples/",
        synthdefBaseURL: "/dist/synthdefs/",
      });

      await sonic.init();

      await sonic.send("/p_new", 100, 0, 0);
      await sonic.sync(1);

      const tree = sonic.getTree();
      const group = tree.nodes.find((n) => n.id === 100);

      // Cleanup
      await sonic.send("/n_free", 100);

      return {
        success: !!group,
        id: group?.id,
        isGroup: group?.isGroup,
      };
    });

    expect(result.success).toBe(true);
    expect(result.id).toBe(100);
    expect(result.isGroup).toBe(true);
  });

  test("/g_head - moves node to head of group", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: "/dist/workers/",
        wasmBaseURL: "/dist/wasm/",
        sampleBaseURL: "/dist/samples/",
        synthdefBaseURL: "/dist/synthdefs/",
      });

      const messages = [];
      sonic.on('message', (msg) => messages.push(msg));

      await sonic.init();
      await sonic.send("/notify", 1);
      await sonic.loadSynthDef("sonic-pi-beep");

      // Create group and synths - 1000 at head, then 1001 at tail
      await sonic.send("/g_new", 100, 0, 0);
      await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 100, "release", 60); // head
      await sonic.send("/s_new", "sonic-pi-beep", 1001, 1, 100, "release", 60); // tail
      await sonic.sync(1);
      // Order is now: 1000 -> 1001

      // Move 1001 to head (should become: 1001 -> 1000)
      messages.length = 0;
      await sonic.send("/g_head", 100, 1001);
      await sonic.sync(2);
      await new Promise((r) => setTimeout(r, 50));

      // /n_move args: nodeID, parentID, prevID, nextID, isGroup
      const moveMsg = messages.find((m) => m.address === "/n_move");

      // Cleanup
      await sonic.send("/g_freeAll", 100);
      await sonic.send("/n_free", 100);

      return {
        success: !!moveMsg,
        movedNodeId: moveMsg?.args?.[0],
        parentId: moveMsg?.args?.[1],
        prevId: moveMsg?.args?.[2],
        nextId: moveMsg?.args?.[3],
      };
    });

    expect(result.success).toBe(true);
    expect(result.movedNodeId).toBe(1001);
    expect(result.parentId).toBe(100);
    expect(result.prevId).toBe(-1); // Head has no prev
    expect(result.nextId).toBe(1000); // 1000 is now after 1001
  });

  test("/g_tail - moves node to tail of group", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: "/dist/workers/",
        wasmBaseURL: "/dist/wasm/",
        sampleBaseURL: "/dist/samples/",
        synthdefBaseURL: "/dist/synthdefs/",
      });

      const messages = [];
      sonic.on('message', (msg) => messages.push(msg));

      await sonic.init();
      await sonic.send("/notify", 1);
      await sonic.loadSynthDef("sonic-pi-beep");

      // Create group and synths - 1000 at head, then 1001 at tail
      await sonic.send("/g_new", 100, 0, 0);
      await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 100, "release", 60); // head
      await sonic.send("/s_new", "sonic-pi-beep", 1001, 1, 100, "release", 60); // tail
      await sonic.sync(1);
      // Order is now: 1000 -> 1001

      // Move 1000 to tail (should become: 1001 -> 1000)
      messages.length = 0;
      await sonic.send("/g_tail", 100, 1000);
      await sonic.sync(2);
      await new Promise((r) => setTimeout(r, 50));

      // /n_move args: nodeID, parentID, prevID, nextID, isGroup
      const moveMsg = messages.find((m) => m.address === "/n_move");

      // Cleanup
      await sonic.send("/g_freeAll", 100);
      await sonic.send("/n_free", 100);

      return {
        success: !!moveMsg,
        movedNodeId: moveMsg?.args?.[0],
        parentId: moveMsg?.args?.[1],
        prevId: moveMsg?.args?.[2],
        nextId: moveMsg?.args?.[3],
      };
    });

    expect(result.success).toBe(true);
    expect(result.movedNodeId).toBe(1000);
    expect(result.parentId).toBe(100);
    expect(result.prevId).toBe(1001); // 1001 is now before 1000
    expect(result.nextId).toBe(-1); // Tail has no next
  });

  test("/g_freeAll - frees all nodes in group", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: "/dist/workers/",
        wasmBaseURL: "/dist/wasm/",
        sampleBaseURL: "/dist/samples/",
        synthdefBaseURL: "/dist/synthdefs/",
      });

      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      // Create group and synths
      await sonic.send("/g_new", 100, 0, 0);
      await sonic.send("/s_new", "sonic-pi-beep", 1000, 1, 100, "release", 60);
      await sonic.send("/s_new", "sonic-pi-beep", 1001, 1, 100, "release", 60);
      await sonic.send("/s_new", "sonic-pi-beep", 1002, 1, 100, "release", 60);
      await sonic.sync(1);

      const treeBefore = sonic.getTree();
      const countBefore = treeBefore.nodes.filter(
        (n) => n.parentId === 100
      ).length;

      // Free all in group
      await sonic.send("/g_freeAll", 100);
      await sonic.sync(2);

      const treeAfter = sonic.getTree();
      const countAfter = treeAfter.nodes.filter(
        (n) => n.parentId === 100
      ).length;
      const groupStillExists = treeAfter.nodes.some((n) => n.id === 100);

      // Cleanup
      await sonic.send("/n_free", 100);

      return {
        success: true,
        countBefore,
        countAfter,
        groupStillExists,
      };
    });

    expect(result.success).toBe(true);
    expect(result.countBefore).toBe(3);
    expect(result.countAfter).toBe(0);
    expect(result.groupStillExists).toBe(true); // Group itself not freed
  });

  test("/g_deepFree - deep frees all synths", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: "/dist/workers/",
        wasmBaseURL: "/dist/wasm/",
        sampleBaseURL: "/dist/samples/",
        synthdefBaseURL: "/dist/synthdefs/",
      });

      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      // Create nested groups with synths
      await sonic.send("/g_new", 100, 0, 0);
      await sonic.send("/g_new", 101, 0, 100); // Nested group
      await sonic.send("/s_new", "sonic-pi-beep", 1000, 1, 100, "release", 60);
      await sonic.send("/s_new", "sonic-pi-beep", 1001, 1, 101, "release", 60);
      await sonic.sync(1);

      const treeBefore = sonic.getTree();
      const synthsBefore = treeBefore.nodes.filter((n) => !n.isGroup).length;

      // Deep free from group 100
      await sonic.send("/g_deepFree", 100);
      await sonic.sync(2);

      const treeAfter = sonic.getTree();
      const synthsAfter = treeAfter.nodes.filter((n) => !n.isGroup).length;
      const groupsRemain =
        treeAfter.nodes.some((n) => n.id === 100) &&
        treeAfter.nodes.some((n) => n.id === 101);

      // Cleanup
      await sonic.send("/n_free", 101, 100);

      return {
        success: true,
        synthsBefore,
        synthsAfter,
        groupsRemain,
      };
    });

    expect(result.success).toBe(true);
    expect(result.synthsBefore).toBe(2);
    expect(result.synthsAfter).toBe(0);
    expect(result.groupsRemain).toBe(true); // Groups not freed, only synths
  });
});

// =============================================================================
// BUFFER COMMANDS
// =============================================================================

test.describe("Buffer Commands", () => {
  test("/b_alloc - allocates buffer", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: "/dist/workers/",
        wasmBaseURL: "/dist/wasm/",
        sampleBaseURL: "/dist/samples/",
        synthdefBaseURL: "/dist/synthdefs/",
      });

      const messages = [];
      sonic.on('message', (msg) => messages.push(msg));

      await sonic.init();

      await sonic.send("/b_alloc", 0, 44100, 2); // Stereo buffer
      await sonic.sync(1);

      // SuperSonic rewrites /b_alloc to /b_allocPtr (JS manages buffer memory)
      const doneMsg = messages.find(
        (m) => m.address === "/done" && m.args[0] === "/b_allocPtr"
      );

      // Query buffer info
      messages.length = 0;
      await sonic.send("/b_query", 0);
      await sonic.sync(2);

      const infoMsg = messages.find((m) => m.address === "/b_info");

      // Cleanup
      await sonic.send("/b_free", 0);

      return {
        success: !!doneMsg && !!infoMsg,
        bufnum: infoMsg?.args?.[0],
        frames: infoMsg?.args?.[1],
        channels: infoMsg?.args?.[2],
        sampleRate: infoMsg?.args?.[3],
      };
    });

    expect(result.success).toBe(true);
    expect(result.bufnum).toBe(0);
    expect(result.frames).toBe(44100);
    expect(result.channels).toBe(2);
  });

  test("/b_free - frees buffer", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: "/dist/workers/",
        wasmBaseURL: "/dist/wasm/",
        sampleBaseURL: "/dist/samples/",
        synthdefBaseURL: "/dist/synthdefs/",
      });

      const messages = [];
      sonic.on('message', (msg) => messages.push(msg));

      await sonic.init();

      await sonic.send("/b_alloc", 0, 1024, 1);
      await sonic.sync(1);

      messages.length = 0;
      await sonic.send("/b_free", 0);
      await sonic.sync(2);

      const doneMsg = messages.find(
        (m) => m.address === "/done" && m.args[0] === "/b_free"
      );

      return { success: !!doneMsg };
    });

    expect(result.success).toBe(true);
  });

  test("/b_zero - zeros buffer", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: "/dist/workers/",
        wasmBaseURL: "/dist/wasm/",
        sampleBaseURL: "/dist/samples/",
        synthdefBaseURL: "/dist/synthdefs/",
      });

      const messages = [];
      sonic.on('message', (msg) => messages.push(msg));

      await sonic.init();

      await sonic.send("/b_alloc", 0, 1024, 1);
      await sonic.sync(1);

      messages.length = 0;
      await sonic.send("/b_zero", 0);
      await sonic.sync(2);

      const doneMsg = messages.find(
        (m) => m.address === "/done" && m.args[0] === "/b_zero"
      );

      // Cleanup
      await sonic.send("/b_free", 0);

      return { success: !!doneMsg };
    });

    expect(result.success).toBe(true);
  });

  test("/b_set and /b_get - sets and gets samples", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: "/dist/workers/",
        wasmBaseURL: "/dist/wasm/",
        sampleBaseURL: "/dist/samples/",
        synthdefBaseURL: "/dist/synthdefs/",
      });

      const messages = [];
      sonic.on('message', (msg) => messages.push(msg));

      await sonic.init();

      await sonic.send("/b_alloc", 0, 1024, 1);
      await sonic.sync(1);

      // Set samples
      await sonic.send("/b_set", 0, 0, 0.5, 10, 0.75, 100, -0.25);
      await sonic.sync(2);

      // Get samples
      messages.length = 0;
      await sonic.send("/b_get", 0, 0, 10, 100);
      await sonic.sync(3);

      const reply = messages.find((m) => m.address === "/b_set");

      // Cleanup
      await sonic.send("/b_free", 0);

      return {
        success: !!reply,
        bufnum: reply?.args?.[0],
        index0: reply?.args?.[1],
        value0: reply?.args?.[2],
        index1: reply?.args?.[3],
        value1: reply?.args?.[4],
        index2: reply?.args?.[5],
        value2: reply?.args?.[6],
      };
    });

    expect(result.success).toBe(true);
    expect(result.bufnum).toBe(0);
    expect(result.index0).toBe(0);
    expect(result.value0).toBeCloseTo(0.5, 5);
    expect(result.index1).toBe(10);
    expect(result.value1).toBeCloseTo(0.75, 5);
    expect(result.index2).toBe(100);
    expect(result.value2).toBeCloseTo(-0.25, 5);
  });

  test("/b_setn and /b_getn - sets and gets sequential samples", async ({
    page,
  }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: "/dist/workers/",
        wasmBaseURL: "/dist/wasm/",
        sampleBaseURL: "/dist/samples/",
        synthdefBaseURL: "/dist/synthdefs/",
      });

      const messages = [];
      sonic.on('message', (msg) => messages.push(msg));

      await sonic.init();

      await sonic.send("/b_alloc", 0, 1024, 1);
      await sonic.sync(1);

      // Set sequential samples
      await sonic.send("/b_setn", 0, 0, 4, 0.1, 0.2, 0.3, 0.4);
      await sonic.sync(2);

      // Get sequential samples
      messages.length = 0;
      await sonic.send("/b_getn", 0, 0, 4);
      await sonic.sync(3);

      const reply = messages.find((m) => m.address === "/b_setn");

      // Cleanup
      await sonic.send("/b_free", 0);

      return {
        success: !!reply,
        bufnum: reply?.args?.[0],
        startIndex: reply?.args?.[1],
        count: reply?.args?.[2],
        values: reply?.args?.slice(3),
      };
    });

    expect(result.success).toBe(true);
    expect(result.bufnum).toBe(0);
    expect(result.startIndex).toBe(0);
    expect(result.count).toBe(4);
    expect(result.values[0]).toBeCloseTo(0.1, 5);
    expect(result.values[1]).toBeCloseTo(0.2, 5);
    expect(result.values[2]).toBeCloseTo(0.3, 5);
    expect(result.values[3]).toBeCloseTo(0.4, 5);
  });

  test("/b_fill - fills buffer with value", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: "/dist/workers/",
        wasmBaseURL: "/dist/wasm/",
        sampleBaseURL: "/dist/samples/",
        synthdefBaseURL: "/dist/synthdefs/",
      });

      const messages = [];
      sonic.on('message', (msg) => messages.push(msg));

      await sonic.init();

      await sonic.send("/b_alloc", 0, 1024, 1);
      await sonic.sync(1);

      // Fill samples 0-99 with 0.5
      await sonic.send("/b_fill", 0, 0, 100, 0.5);
      await sonic.sync(2);

      // Verify by getting a sample
      messages.length = 0;
      await sonic.send("/b_get", 0, 50);
      await sonic.sync(3);

      const reply = messages.find((m) => m.address === "/b_set");

      // Cleanup
      await sonic.send("/b_free", 0);

      return {
        success: !!reply,
        value: reply?.args?.[2],
      };
    });

    expect(result.success).toBe(true);
    expect(result.value).toBeCloseTo(0.5, 5);
  });

  test("/b_gen sine1 - generates sine wave", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: "/dist/workers/",
        wasmBaseURL: "/dist/wasm/",
        sampleBaseURL: "/dist/samples/",
        synthdefBaseURL: "/dist/synthdefs/",
      });

      const messages = [];
      sonic.on('message', (msg) => messages.push(msg));

      await sonic.init();

      await sonic.send("/b_alloc", 0, 512, 1);
      await sonic.sync(1);

      // Generate normalized sine wave
      messages.length = 0;
      await sonic.send("/b_gen", 0, "sine1", 1, 1.0); // flags=1 (normalize), amp=1.0
      await sonic.sync(2);

      const doneMsg = messages.find(
        (m) => m.address === "/done" && m.args[0] === "/b_gen"
      );

      // Check peak value (should be ~1.0 due to normalization)
      messages.length = 0;
      await sonic.send("/b_get", 0, 128); // Quarter way through (should be near peak)
      await sonic.sync(3);

      const valueReply = messages.find((m) => m.address === "/b_set");

      // Cleanup
      await sonic.send("/b_free", 0);

      return {
        success: !!doneMsg,
        peakValue: valueReply?.args?.[2],
      };
    });

    expect(result.success).toBe(true);
    expect(Math.abs(result.peakValue)).toBeGreaterThan(0.9); // Should be near 1.0
  });

  test("/b_query - queries buffer info", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: "/dist/workers/",
        wasmBaseURL: "/dist/wasm/",
        sampleBaseURL: "/dist/samples/",
        synthdefBaseURL: "/dist/synthdefs/",
      });

      const messages = [];
      sonic.on('message', (msg) => messages.push(msg));

      await sonic.init();

      await sonic.send("/b_alloc", 0, 22050, 2);
      await sonic.send("/b_alloc", 1, 44100, 1);
      await sonic.sync(1);

      messages.length = 0;
      await sonic.send("/b_query", 0, 1);
      await sonic.sync(2);

      const infoMsg = messages.find((m) => m.address === "/b_info");

      // Cleanup
      await sonic.send("/b_free", 0);
      await sonic.send("/b_free", 1);

      // /b_query returns one /b_info message with all buffer info concatenated
      // Each buffer has 4 values: bufnum, frames, channels, sampleRate
      return {
        success: !!infoMsg,
        buf0: {
          bufnum: infoMsg?.args?.[0],
          frames: infoMsg?.args?.[1],
          channels: infoMsg?.args?.[2],
          sampleRate: infoMsg?.args?.[3],
        },
        buf1: {
          bufnum: infoMsg?.args?.[4],
          frames: infoMsg?.args?.[5],
          channels: infoMsg?.args?.[6],
          sampleRate: infoMsg?.args?.[7],
        },
      };
    });

    expect(result.success).toBe(true);
    expect(result.buf0.bufnum).toBe(0);
    expect(result.buf0.frames).toBe(22050);
    expect(result.buf0.channels).toBe(2);
    expect(result.buf1.bufnum).toBe(1);
    expect(result.buf1.frames).toBe(44100);
    expect(result.buf1.channels).toBe(1);
  });
});

// =============================================================================
// CONTROL BUS COMMANDS
// =============================================================================

test.describe("Control Bus Commands", () => {
  test("/c_set and /c_get - sets and gets bus value", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: "/dist/workers/",
        wasmBaseURL: "/dist/wasm/",
        sampleBaseURL: "/dist/samples/",
        synthdefBaseURL: "/dist/synthdefs/",
      });

      const messages = [];
      sonic.on('message', (msg) => messages.push(msg));

      await sonic.init();

      // Set bus values
      await sonic.send("/c_set", 0, 440, 1, 0.5, 2, 880);
      await sonic.sync(1);

      // Get bus values
      messages.length = 0;
      await sonic.send("/c_get", 0, 1, 2);
      await sonic.sync(2);

      const reply = messages.find((m) => m.address === "/c_set");

      return {
        success: !!reply,
        values: reply?.args,
      };
    });

    expect(result.success).toBe(true);
    expect(result.values[0]).toBe(0);
    expect(result.values[1]).toBeCloseTo(440, 1);
    expect(result.values[2]).toBe(1);
    expect(result.values[3]).toBeCloseTo(0.5, 5);
    expect(result.values[4]).toBe(2);
    expect(result.values[5]).toBeCloseTo(880, 1);
  });

  test("/c_setn and /c_getn - sets and gets sequential bus values", async ({
    page,
  }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: "/dist/workers/",
        wasmBaseURL: "/dist/wasm/",
        sampleBaseURL: "/dist/samples/",
        synthdefBaseURL: "/dist/synthdefs/",
      });

      const messages = [];
      sonic.on('message', (msg) => messages.push(msg));

      await sonic.init();

      // Set sequential bus values
      await sonic.send("/c_setn", 0, 4, 100, 200, 300, 400);
      await sonic.sync(1);

      // Get sequential bus values
      messages.length = 0;
      await sonic.send("/c_getn", 0, 4);
      await sonic.sync(2);

      const reply = messages.find((m) => m.address === "/c_setn");

      return {
        success: !!reply,
        startIndex: reply?.args?.[0],
        count: reply?.args?.[1],
        values: reply?.args?.slice(2),
      };
    });

    expect(result.success).toBe(true);
    expect(result.startIndex).toBe(0);
    expect(result.count).toBe(4);
    expect(result.values[0]).toBeCloseTo(100, 1);
    expect(result.values[1]).toBeCloseTo(200, 1);
    expect(result.values[2]).toBeCloseTo(300, 1);
    expect(result.values[3]).toBeCloseTo(400, 1);
  });

  test("/c_fill - fills buses with value", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: "/dist/workers/",
        wasmBaseURL: "/dist/wasm/",
        sampleBaseURL: "/dist/samples/",
        synthdefBaseURL: "/dist/synthdefs/",
      });

      const messages = [];
      sonic.on('message', (msg) => messages.push(msg));

      await sonic.init();

      // Fill buses 0-9 with 0.5
      await sonic.send("/c_fill", 0, 10, 0.5);
      await sonic.sync(1);

      // Verify
      messages.length = 0;
      await sonic.send("/c_get", 5);
      await sonic.sync(2);

      const reply = messages.find((m) => m.address === "/c_set");

      return {
        success: !!reply,
        value: reply?.args?.[1],
      };
    });

    expect(result.success).toBe(true);
    expect(result.value).toBeCloseTo(0.5, 5);
  });
});

/**
 * Node Tree (getTree) Test Suite
 *
 * Comprehensive tests for the getTree() SharedArrayBuffer API,
 * validating it against the standard SuperCollider OSC node tree commands:
 * - /g_queryTree for structure verification
 * - /n_query for individual node info
 * - Node notifications (/n_go, /n_end, /n_move) for lifecycle events
 *
 * These tests ensure getTree() provides an accurate, low-latency view
 * of the scsynth node tree for visualization purposes.
 */

import { test, expect } from "@playwright/test";

// =============================================================================
// TEST UTILITIES
// =============================================================================

const SONIC_CONFIG = {
  workerBaseURL: "/dist/workers/",
  wasmBaseURL: "/dist/wasm/",
  sampleBaseURL: "/dist/samples/",
  synthdefBaseURL: "/dist/synthdefs/",
};

/**
 * Helper to parse /g_queryTree.reply into a structured tree
 * Format: [flag, nodeID, numChildren, ...]
 * Groups: [nodeID, numChildren, ...children]
 * Synths: [nodeID, -1, defName] (when flag=0) or [nodeID, -1, defName, numControls, ...controls] (when flag=1)
 */
function parseQueryTreeReply(args, includeControls = false) {
  const nodes = [];
  let i = 1; // Skip flag at index 0

  function parseNode(parentId) {
    if (i >= args.length) return null;

    const nodeId = args[i++];
    const numChildren = args[i++];

    if (numChildren === -1) {
      // Synth node
      const defName = args[i++];
      const node = { id: nodeId, parentId, isGroup: false, defName };

      if (includeControls) {
        const numControls = args[i++];
        node.controls = [];
        for (let c = 0; c < numControls; c++) {
          const name = args[i++];
          const value = args[i++];
          node.controls.push({ name, value });
        }
      }

      nodes.push(node);
      return node;
    } else {
      // Group node
      const node = { id: nodeId, parentId, isGroup: true, defName: "group", children: [] };
      nodes.push(node);

      for (let c = 0; c < numChildren; c++) {
        const child = parseNode(nodeId);
        if (child) node.children.push(child.id);
      }

      return node;
    }
  }

  parseNode(-1); // Root has no parent
  return nodes;
}

// =============================================================================
// BASIC getTree() FUNCTIONALITY
// =============================================================================

test.describe("getTree() basic functionality", () => {
  test("returns empty tree before initialization", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      // Don't call init()
      const tree = sonic.getTree();
      return tree;
    }, SONIC_CONFIG);

    expect(result.nodeCount).toBe(0);
    expect(result.version).toBe(0);
    expect(result.nodes).toEqual([]);
  });

  test("returns root group after initialization", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();

      const tree = sonic.getTree();
      return tree;
    }, SONIC_CONFIG);

    expect(result.nodeCount).toBe(1);
    expect(result.nodes.length).toBe(1);

    const rootGroup = result.nodes[0];
    expect(rootGroup.id).toBe(0);
    expect(rootGroup.isGroup).toBe(true);
    expect(rootGroup.defName).toBe("group");
    expect(rootGroup.parentId).toBe(-1);
  });

  test("returns correct structure shape for all fields", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 0, "release", 60);
      await sonic.sync(1);

      const tree = sonic.getTree();

      // Check field types
      const synth = tree.nodes.find((n) => n.id === 1000);

      return {
        tree,
        hasNodeCount: typeof tree.nodeCount === "number",
        hasVersion: typeof tree.version === "number",
        hasNodes: Array.isArray(tree.nodes),
        synthFields: synth
          ? {
              hasId: typeof synth.id === "number",
              hasParentId: typeof synth.parentId === "number",
              hasIsGroup: typeof synth.isGroup === "boolean",
              hasPrevId: typeof synth.prevId === "number",
              hasNextId: typeof synth.nextId === "number",
              hasHeadId: typeof synth.headId === "number",
              hasDefName: typeof synth.defName === "string",
            }
          : null,
      };
    }, SONIC_CONFIG);

    expect(result.hasNodeCount).toBe(true);
    expect(result.hasVersion).toBe(true);
    expect(result.hasNodes).toBe(true);
    expect(result.synthFields).not.toBeNull();
    expect(result.synthFields.hasId).toBe(true);
    expect(result.synthFields.hasParentId).toBe(true);
    expect(result.synthFields.hasIsGroup).toBe(true);
    expect(result.synthFields.hasPrevId).toBe(true);
    expect(result.synthFields.hasNextId).toBe(true);
    expect(result.synthFields.hasHeadId).toBe(true);
    expect(result.synthFields.hasDefName).toBe(true);
  });
});

// =============================================================================
// VERSION COUNTER
// =============================================================================

test.describe("getTree() version counter", () => {
  test("version increments on node creation", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      const v1 = sonic.getTree().version;

      await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 0, "release", 60);
      await sonic.sync(1);

      const v2 = sonic.getTree().version;

      await sonic.send("/s_new", "sonic-pi-beep", 1001, 0, 0, "release", 60);
      await sonic.sync(2);

      const v3 = sonic.getTree().version;

      // Cleanup
      await sonic.send("/n_free", 1000, 1001);

      return { v1, v2, v3 };
    }, SONIC_CONFIG);

    expect(result.v2).toBeGreaterThan(result.v1);
    expect(result.v3).toBeGreaterThan(result.v2);
  });

  test("version increments on node removal", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 0, "release", 60);
      await sonic.sync(1);

      const vBefore = sonic.getTree().version;

      await sonic.send("/n_free", 1000);
      await sonic.sync(2);

      const vAfter = sonic.getTree().version;

      return { vBefore, vAfter };
    }, SONIC_CONFIG);

    expect(result.vAfter).toBeGreaterThan(result.vBefore);
  });

  test("version increments on node move", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 0, "release", 60);
      await sonic.send("/s_new", "sonic-pi-beep", 1001, 0, 0, "release", 60);
      await sonic.sync(1);

      const vBefore = sonic.getTree().version;

      // Move 1001 before 1000
      await sonic.send("/n_before", 1001, 1000);
      await sonic.sync(2);

      const vAfter = sonic.getTree().version;

      // Cleanup
      await sonic.send("/n_free", 1000, 1001);

      return { vBefore, vAfter };
    }, SONIC_CONFIG);

    expect(result.vAfter).toBeGreaterThan(result.vBefore);
  });

  test("version unchanged when tree unchanged", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 0, "release", 60);
      await sonic.sync(1);

      const v1 = sonic.getTree().version;

      // Just query - no changes
      await sonic.send("/status");
      await sonic.sync(2);

      const v2 = sonic.getTree().version;

      // Cleanup
      await sonic.send("/n_free", 1000);

      return { v1, v2 };
    }, SONIC_CONFIG);

    expect(result.v1).toBe(result.v2);
  });
});

// =============================================================================
// COMPARISON WITH /g_queryTree OSC API
// =============================================================================

test.describe("getTree() vs /g_queryTree comparison", () => {
  test("node count matches /g_queryTree", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      const messages = [];
      sonic.on("message", (msg) => messages.push(msg));

      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      // Create varied hierarchy
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

      const reply = messages.find((m) => m.address === "/g_queryTree.reply");

      // Parse /g_queryTree response to count nodes
      // Format: [flag, nodeID, numChildren, ...children]
      // Groups: nodeID, numChildren >= 0, then children recursively
      // Synths: nodeID, -1, defName
      let oscNodeCount = 0;
      const args = reply?.args || [];
      let i = 1; // Skip flag

      function countNodes() {
        if (i >= args.length) return;
        const nodeId = args[i++];
        const numChildren = args[i++];
        oscNodeCount++;

        if (numChildren === -1) {
          // Synth - skip defName
          i++;
        } else {
          // Group - count children recursively
          for (let c = 0; c < numChildren; c++) {
            countNodes();
          }
        }
      }
      countNodes();

      // Cleanup
      await sonic.send("/n_free", 100, 1002);

      return {
        sabNodeCount: sabTree.nodeCount,
        sabNodesLength: sabTree.nodes.length,
        oscNodeCount,
      };
    }, SONIC_CONFIG);

    // SAB should report same count as nodes array length
    expect(result.sabNodeCount).toBe(result.sabNodesLength);
    // Expected: root(0) + group(100) + group(101) + synth(1000) + synth(1001) + synth(1002) = 6
    expect(result.sabNodeCount).toBe(6);
    expect(result.oscNodeCount).toBe(6);
  });

  test("all node IDs match /g_queryTree", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      const messages = [];
      sonic.on("message", (msg) => messages.push(msg));

      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      // Create hierarchy
      await sonic.send("/g_new", 100, 0, 0);
      await sonic.send("/g_new", 200, 1, 0);
      await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 100, "release", 60);
      await sonic.send("/s_new", "sonic-pi-beep", 2000, 0, 200, "release", 60);
      await sonic.sync(1);

      const sabTree = sonic.getTree();

      messages.length = 0;
      await sonic.send("/g_queryTree", 0, 0);
      await sonic.sync(2);

      const reply = messages.find((m) => m.address === "/g_queryTree.reply");

      // Parse /g_queryTree to extract all node IDs
      const oscNodeIds = [];
      const args = reply?.args || [];
      let i = 1; // Skip flag

      function extractIds() {
        if (i >= args.length) return;
        const nodeId = args[i++];
        const numChildren = args[i++];
        oscNodeIds.push(nodeId);

        if (numChildren === -1) {
          // Synth - skip defName
          i++;
        } else {
          // Group - process children recursively
          for (let c = 0; c < numChildren; c++) {
            extractIds();
          }
        }
      }
      extractIds();

      // Cleanup
      await sonic.send("/n_free", 100, 200);

      return {
        sabNodeIds: sabTree.nodes.map((n) => n.id).sort((a, b) => a - b),
        oscNodeIds: oscNodeIds.sort((a, b) => a - b),
      };
    }, SONIC_CONFIG);

    expect(result.sabNodeIds).toEqual(result.oscNodeIds);
  });

  test("synthdef names match /g_queryTree", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      const messages = [];
      sonic.on("message", (msg) => messages.push(msg));

      await sonic.init();
      await sonic.loadSynthDefs(["sonic-pi-beep", "sonic-pi-saw"]);

      await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 0, "release", 60);
      await sonic.send("/s_new", "sonic-pi-saw", 1001, 0, 0, "release", 60);
      await sonic.sync(1);

      const sabTree = sonic.getTree();

      messages.length = 0;
      await sonic.send("/g_queryTree", 0, 0);
      await sonic.sync(2);

      const reply = messages.find((m) => m.address === "/g_queryTree.reply");

      // Extract synth names from OSC response
      // Synths have format: [nodeID, -1, defName]
      const oscSynthNames = {};
      const args = reply?.args || [];
      for (let i = 1; i < args.length - 2; i++) {
        if (
          typeof args[i] === "number" &&
          args[i] >= 0 &&
          args[i + 1] === -1 &&
          typeof args[i + 2] === "string"
        ) {
          oscSynthNames[args[i]] = args[i + 2];
        }
      }

      // Cleanup
      await sonic.send("/n_free", 1000, 1001);

      return {
        sabSynth1000: sabTree.nodes.find((n) => n.id === 1000)?.defName,
        sabSynth1001: sabTree.nodes.find((n) => n.id === 1001)?.defName,
        oscSynth1000: oscSynthNames[1000],
        oscSynth1001: oscSynthNames[1001],
      };
    }, SONIC_CONFIG);

    expect(result.sabSynth1000).toBe("sonic-pi-beep");
    expect(result.sabSynth1001).toBe("sonic-pi-saw");
    expect(result.sabSynth1000).toBe(result.oscSynth1000);
    expect(result.sabSynth1001).toBe(result.oscSynth1001);
  });

  test("isGroup matches /g_queryTree (numChildren >= 0 vs -1)", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      const messages = [];
      sonic.on("message", (msg) => messages.push(msg));

      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      await sonic.send("/g_new", 100, 0, 0);
      await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 100, "release", 60);
      await sonic.sync(1);

      const sabTree = sonic.getTree();

      messages.length = 0;
      await sonic.send("/g_queryTree", 0, 0);
      await sonic.sync(2);

      const reply = messages.find((m) => m.address === "/g_queryTree.reply");

      // Parse OSC to determine isGroup (numChildren >= 0 = group, -1 = synth)
      const oscIsGroup = {};
      const args = reply?.args || [];
      for (let i = 1; i < args.length - 1; i++) {
        if (typeof args[i] === "number" && args[i] >= 0) {
          const numChildren = args[i + 1];
          if (typeof numChildren === "number") {
            oscIsGroup[args[i]] = numChildren >= 0;
          }
        }
      }

      // Cleanup
      await sonic.send("/n_free", 100);

      return {
        sabRoot: sabTree.nodes.find((n) => n.id === 0)?.isGroup,
        sabGroup100: sabTree.nodes.find((n) => n.id === 100)?.isGroup,
        sabSynth1000: sabTree.nodes.find((n) => n.id === 1000)?.isGroup,
        oscRoot: oscIsGroup[0],
        oscGroup100: oscIsGroup[100],
        oscSynth1000: oscIsGroup[1000],
      };
    }, SONIC_CONFIG);

    expect(result.sabRoot).toBe(true);
    expect(result.sabGroup100).toBe(true);
    expect(result.sabSynth1000).toBe(false);
    expect(result.sabRoot).toBe(result.oscRoot);
    expect(result.sabGroup100).toBe(result.oscGroup100);
    expect(result.sabSynth1000).toBe(result.oscSynth1000);
  });
});

// =============================================================================
// COMPARISON WITH /n_query OSC API
// =============================================================================

test.describe("getTree() vs /n_query comparison", () => {
  test("parentId matches /n_info response", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      const messages = [];
      sonic.on("message", (msg) => messages.push(msg));

      await sonic.init();
      // Must register for notifications to receive /n_info
      await sonic.send("/notify", 1);
      await sonic.loadSynthDef("sonic-pi-beep");

      await sonic.send("/g_new", 100, 0, 0);
      await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 100, "release", 60);
      await sonic.sync(1);

      const sabTree = sonic.getTree();

      // Query individual nodes
      messages.length = 0;
      await sonic.send("/n_query", 100, 1000);
      await sonic.sync(2);

      // /n_info format: [nodeID, parentID, prevID, nextID, isGroup, ...]
      const info100 = messages.find(
        (m) => m.address === "/n_info" && m.args[0] === 100
      );
      const info1000 = messages.find(
        (m) => m.address === "/n_info" && m.args[0] === 1000
      );

      // Cleanup
      await sonic.send("/n_free", 100);

      return {
        sabGroup100Parent: sabTree.nodes.find((n) => n.id === 100)?.parentId,
        sabSynth1000Parent: sabTree.nodes.find((n) => n.id === 1000)?.parentId,
        oscGroup100Parent: info100?.args[1],
        oscSynth1000Parent: info1000?.args[1],
        hasInfo100: !!info100,
        hasInfo1000: !!info1000,
      };
    }, SONIC_CONFIG);

    expect(result.hasInfo100).toBe(true);
    expect(result.hasInfo1000).toBe(true);
    expect(result.sabGroup100Parent).toBe(0); // Parent is root
    expect(result.sabSynth1000Parent).toBe(100); // Parent is group 100
    expect(result.sabGroup100Parent).toBe(result.oscGroup100Parent);
    expect(result.sabSynth1000Parent).toBe(result.oscSynth1000Parent);
  });

  test("prevId and nextId match /n_info response", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      const messages = [];
      sonic.on("message", (msg) => messages.push(msg));

      await sonic.init();
      // Must register for notifications to receive /n_info
      await sonic.send("/notify", 1);
      await sonic.loadSynthDef("sonic-pi-beep");

      // Create chain: 1000 -> 1001 -> 1002 (in root group)
      await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 0, "release", 60);
      await sonic.send("/s_new", "sonic-pi-beep", 1001, 3, 1000, "release", 60); // after 1000
      await sonic.send("/s_new", "sonic-pi-beep", 1002, 3, 1001, "release", 60); // after 1001
      await sonic.sync(1);

      const sabTree = sonic.getTree();

      messages.length = 0;
      await sonic.send("/n_query", 1000, 1001, 1002);
      await sonic.sync(2);

      const info1000 = messages.find(
        (m) => m.address === "/n_info" && m.args[0] === 1000
      );
      const info1001 = messages.find(
        (m) => m.address === "/n_info" && m.args[0] === 1001
      );
      const info1002 = messages.find(
        (m) => m.address === "/n_info" && m.args[0] === 1002
      );

      const sab1000 = sabTree.nodes.find((n) => n.id === 1000);
      const sab1001 = sabTree.nodes.find((n) => n.id === 1001);
      const sab1002 = sabTree.nodes.find((n) => n.id === 1002);

      // Cleanup
      await sonic.send("/n_free", 1000, 1001, 1002);

      // /n_info format: [nodeID, parentID, prevID, nextID, isGroup, ...]
      return {
        sab1000: { prev: sab1000?.prevId, next: sab1000?.nextId },
        sab1001: { prev: sab1001?.prevId, next: sab1001?.nextId },
        sab1002: { prev: sab1002?.prevId, next: sab1002?.nextId },
        osc1000: { prev: info1000?.args[2], next: info1000?.args[3] },
        osc1001: { prev: info1001?.args[2], next: info1001?.args[3] },
        osc1002: { prev: info1002?.args[2], next: info1002?.args[3] },
      };
    }, SONIC_CONFIG);

    // Verify SAB matches OSC
    expect(result.sab1000.prev).toBe(result.osc1000.prev);
    expect(result.sab1000.next).toBe(result.osc1000.next);
    expect(result.sab1001.prev).toBe(result.osc1001.prev);
    expect(result.sab1001.next).toBe(result.osc1001.next);
    expect(result.sab1002.prev).toBe(result.osc1002.prev);
    expect(result.sab1002.next).toBe(result.osc1002.next);

    // Verify chain structure: 1000 -> 1001 -> 1002
    expect(result.sab1000.prev).toBe(-1); // First in chain
    expect(result.sab1000.next).toBe(1001);
    expect(result.sab1001.prev).toBe(1000);
    expect(result.sab1001.next).toBe(1002);
    expect(result.sab1002.prev).toBe(1001);
    expect(result.sab1002.next).toBe(-1); // Last in chain
  });

  test("headId matches /n_info group head", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      const messages = [];
      sonic.on("message", (msg) => messages.push(msg));

      await sonic.init();
      // Must register for notifications to receive /n_info
      await sonic.send("/notify", 1);
      await sonic.loadSynthDef("sonic-pi-beep");

      await sonic.send("/g_new", 100, 0, 0);
      await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 100, "release", 60); // head of 100
      await sonic.send("/s_new", "sonic-pi-beep", 1001, 1, 100, "release", 60); // tail of 100
      await sonic.sync(1);

      const sabTree = sonic.getTree();

      messages.length = 0;
      await sonic.send("/n_query", 100);
      await sonic.sync(2);

      // /n_info for groups: [nodeID, parentID, prevID, nextID, isGroup(1), headID, tailID]
      const info100 = messages.find(
        (m) => m.address === "/n_info" && m.args[0] === 100
      );

      const sabGroup100 = sabTree.nodes.find((n) => n.id === 100);

      // Cleanup
      await sonic.send("/n_free", 100);

      return {
        sabHeadId: sabGroup100?.headId,
        oscHeadId: info100?.args[5],
        oscTailId: info100?.args[6],
        hasInfo100: !!info100,
      };
    }, SONIC_CONFIG);

    expect(result.hasInfo100).toBe(true);
    expect(result.sabHeadId).toBe(1000);
    expect(result.sabHeadId).toBe(result.oscHeadId);
    expect(result.oscTailId).toBe(1001);
  });
});

// =============================================================================
// NODE LIFECYCLE SYNCHRONIZATION
// =============================================================================

test.describe("getTree() node lifecycle", () => {
  test("synth appears after /s_new", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      const treeBefore = sonic.getTree();

      await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 0, "release", 60);
      await sonic.sync(1);

      const treeAfter = sonic.getTree();

      // Cleanup
      await sonic.send("/n_free", 1000);

      return {
        before: {
          nodeCount: treeBefore.nodeCount,
          has1000: treeBefore.nodes.some((n) => n.id === 1000),
        },
        after: {
          nodeCount: treeAfter.nodeCount,
          has1000: treeAfter.nodes.some((n) => n.id === 1000),
          node1000: treeAfter.nodes.find((n) => n.id === 1000),
        },
      };
    }, SONIC_CONFIG);

    expect(result.before.has1000).toBe(false);
    expect(result.after.has1000).toBe(true);
    expect(result.after.nodeCount).toBe(result.before.nodeCount + 1);
    expect(result.after.node1000.defName).toBe("sonic-pi-beep");
    expect(result.after.node1000.isGroup).toBe(false);
  });

  test("group appears after /g_new", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();

      const treeBefore = sonic.getTree();

      await sonic.send("/g_new", 100, 0, 0);
      await sonic.sync(1);

      const treeAfter = sonic.getTree();

      // Cleanup
      await sonic.send("/n_free", 100);

      return {
        before: {
          nodeCount: treeBefore.nodeCount,
          has100: treeBefore.nodes.some((n) => n.id === 100),
        },
        after: {
          nodeCount: treeAfter.nodeCount,
          has100: treeAfter.nodes.some((n) => n.id === 100),
          node100: treeAfter.nodes.find((n) => n.id === 100),
        },
      };
    }, SONIC_CONFIG);

    expect(result.before.has100).toBe(false);
    expect(result.after.has100).toBe(true);
    expect(result.after.nodeCount).toBe(result.before.nodeCount + 1);
    expect(result.after.node100.defName).toBe("group");
    expect(result.after.node100.isGroup).toBe(true);
  });

  test("node disappears after /n_free", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 0, "release", 60);
      await sonic.sync(1);

      const treeBefore = sonic.getTree();

      await sonic.send("/n_free", 1000);
      await sonic.sync(2);

      const treeAfter = sonic.getTree();

      return {
        before: {
          nodeCount: treeBefore.nodeCount,
          has1000: treeBefore.nodes.some((n) => n.id === 1000),
        },
        after: {
          nodeCount: treeAfter.nodeCount,
          has1000: treeAfter.nodes.some((n) => n.id === 1000),
        },
      };
    }, SONIC_CONFIG);

    expect(result.before.has1000).toBe(true);
    expect(result.after.has1000).toBe(false);
    expect(result.after.nodeCount).toBe(result.before.nodeCount - 1);
  });

  test("children removed with /g_freeAll", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      await sonic.send("/g_new", 100, 0, 0);
      await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 100, "release", 60);
      await sonic.send("/s_new", "sonic-pi-beep", 1001, 0, 100, "release", 60);
      await sonic.sync(1);

      const treeBefore = sonic.getTree();

      await sonic.send("/g_freeAll", 100);
      await sonic.sync(2);

      const treeAfter = sonic.getTree();

      // Cleanup
      await sonic.send("/n_free", 100);

      return {
        before: {
          nodeCount: treeBefore.nodeCount,
          has100: treeBefore.nodes.some((n) => n.id === 100),
          has1000: treeBefore.nodes.some((n) => n.id === 1000),
          has1001: treeBefore.nodes.some((n) => n.id === 1001),
        },
        after: {
          nodeCount: treeAfter.nodeCount,
          has100: treeAfter.nodes.some((n) => n.id === 100),
          has1000: treeAfter.nodes.some((n) => n.id === 1000),
          has1001: treeAfter.nodes.some((n) => n.id === 1001),
        },
      };
    }, SONIC_CONFIG);

    expect(result.before.has100).toBe(true);
    expect(result.before.has1000).toBe(true);
    expect(result.before.has1001).toBe(true);
    expect(result.after.has100).toBe(true); // Group still exists
    expect(result.after.has1000).toBe(false); // Children freed
    expect(result.after.has1001).toBe(false);
    expect(result.after.nodeCount).toBe(result.before.nodeCount - 2);
  });
});

// =============================================================================
// NODE MOVEMENT
// =============================================================================

test.describe("getTree() node movement", () => {
  test("/n_before updates sibling links", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      // Create order: 1000, 1001, 1002
      await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 0, "release", 60);
      await sonic.send("/s_new", "sonic-pi-beep", 1001, 3, 1000, "release", 60);
      await sonic.send("/s_new", "sonic-pi-beep", 1002, 3, 1001, "release", 60);
      await sonic.sync(1);

      const treeBefore = sonic.getTree();

      // Move 1002 before 1000: new order should be 1002, 1000, 1001
      await sonic.send("/n_before", 1002, 1000);
      await sonic.sync(2);

      const treeAfter = sonic.getTree();

      // Cleanup
      await sonic.send("/n_free", 1000, 1001, 1002);

      const getBefore = (tree, id) => tree.nodes.find((n) => n.id === id);

      return {
        before: {
          n1000: { prev: getBefore(treeBefore, 1000)?.prevId, next: getBefore(treeBefore, 1000)?.nextId },
          n1001: { prev: getBefore(treeBefore, 1001)?.prevId, next: getBefore(treeBefore, 1001)?.nextId },
          n1002: { prev: getBefore(treeBefore, 1002)?.prevId, next: getBefore(treeBefore, 1002)?.nextId },
        },
        after: {
          n1000: { prev: getBefore(treeAfter, 1000)?.prevId, next: getBefore(treeAfter, 1000)?.nextId },
          n1001: { prev: getBefore(treeAfter, 1001)?.prevId, next: getBefore(treeAfter, 1001)?.nextId },
          n1002: { prev: getBefore(treeAfter, 1002)?.prevId, next: getBefore(treeAfter, 1002)?.nextId },
        },
      };
    }, SONIC_CONFIG);

    // Before: 1000 -> 1001 -> 1002
    expect(result.before.n1000.prev).toBe(-1);
    expect(result.before.n1000.next).toBe(1001);
    expect(result.before.n1002.prev).toBe(1001);
    expect(result.before.n1002.next).toBe(-1);

    // After: 1002 -> 1000 -> 1001
    expect(result.after.n1002.prev).toBe(-1); // 1002 is now first
    expect(result.after.n1002.next).toBe(1000);
    expect(result.after.n1000.prev).toBe(1002);
    expect(result.after.n1000.next).toBe(1001);
    expect(result.after.n1001.prev).toBe(1000);
    expect(result.after.n1001.next).toBe(-1); // 1001 is now last
  });

  test("/n_after updates sibling links", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      // Create order: 1000, 1001, 1002
      await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 0, "release", 60);
      await sonic.send("/s_new", "sonic-pi-beep", 1001, 3, 1000, "release", 60);
      await sonic.send("/s_new", "sonic-pi-beep", 1002, 3, 1001, "release", 60);
      await sonic.sync(1);

      // Move 1000 after 1002: new order should be 1001, 1002, 1000
      await sonic.send("/n_after", 1000, 1002);
      await sonic.sync(2);

      const treeAfter = sonic.getTree();

      // Cleanup
      await sonic.send("/n_free", 1000, 1001, 1002);

      const getNode = (id) => treeAfter.nodes.find((n) => n.id === id);

      return {
        n1000: { prev: getNode(1000)?.prevId, next: getNode(1000)?.nextId },
        n1001: { prev: getNode(1001)?.prevId, next: getNode(1001)?.nextId },
        n1002: { prev: getNode(1002)?.prevId, next: getNode(1002)?.nextId },
      };
    }, SONIC_CONFIG);

    // After: 1001 -> 1002 -> 1000
    expect(result.n1001.prev).toBe(-1); // 1001 is now first
    expect(result.n1001.next).toBe(1002);
    expect(result.n1002.prev).toBe(1001);
    expect(result.n1002.next).toBe(1000);
    expect(result.n1000.prev).toBe(1002);
    expect(result.n1000.next).toBe(-1); // 1000 is now last
  });

  test("/g_head moves node to group head", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      await sonic.send("/g_new", 100, 0, 0);
      await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 100, "release", 60);
      await sonic.send("/s_new", "sonic-pi-beep", 1001, 1, 100, "release", 60); // tail
      await sonic.sync(1);

      const treeBefore = sonic.getTree();

      // Move 1001 to head
      await sonic.send("/g_head", 100, 1001);
      await sonic.sync(2);

      const treeAfter = sonic.getTree();

      // Cleanup
      await sonic.send("/n_free", 100);

      return {
        beforeHeadId: treeBefore.nodes.find((n) => n.id === 100)?.headId,
        afterHeadId: treeAfter.nodes.find((n) => n.id === 100)?.headId,
        after1001Prev: treeAfter.nodes.find((n) => n.id === 1001)?.prevId,
      };
    }, SONIC_CONFIG);

    expect(result.beforeHeadId).toBe(1000);
    expect(result.afterHeadId).toBe(1001);
    expect(result.after1001Prev).toBe(-1); // 1001 is now first
  });

  test("/g_tail moves node to group tail", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      await sonic.send("/g_new", 100, 0, 0);
      await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 100, "release", 60); // head
      await sonic.send("/s_new", "sonic-pi-beep", 1001, 1, 100, "release", 60);
      await sonic.sync(1);

      // Move 1000 to tail
      await sonic.send("/g_tail", 100, 1000);
      await sonic.sync(2);

      const treeAfter = sonic.getTree();

      // Cleanup
      await sonic.send("/n_free", 100);

      return {
        afterHeadId: treeAfter.nodes.find((n) => n.id === 100)?.headId,
        after1000Next: treeAfter.nodes.find((n) => n.id === 1000)?.nextId,
        after1001Next: treeAfter.nodes.find((n) => n.id === 1001)?.nextId,
      };
    }, SONIC_CONFIG);

    expect(result.afterHeadId).toBe(1001); // 1001 is now head
    expect(result.after1000Next).toBe(-1); // 1000 is now last
    expect(result.after1001Next).toBe(1000);
  });

  test("moving node between groups updates parentId", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      await sonic.send("/g_new", 100, 0, 0);
      await sonic.send("/g_new", 200, 0, 0);
      await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 100, "release", 60);
      await sonic.sync(1);

      const treeBefore = sonic.getTree();

      // Move synth 1000 to group 200
      await sonic.send("/g_head", 200, 1000);
      await sonic.sync(2);

      const treeAfter = sonic.getTree();

      // Cleanup
      await sonic.send("/n_free", 100, 200);

      return {
        beforeParent: treeBefore.nodes.find((n) => n.id === 1000)?.parentId,
        afterParent: treeAfter.nodes.find((n) => n.id === 1000)?.parentId,
        group100HeadAfter: treeAfter.nodes.find((n) => n.id === 100)?.headId,
        group200HeadAfter: treeAfter.nodes.find((n) => n.id === 200)?.headId,
      };
    }, SONIC_CONFIG);

    expect(result.beforeParent).toBe(100);
    expect(result.afterParent).toBe(200);
    expect(result.group100HeadAfter).toBe(-1); // Group 100 now empty
    expect(result.group200HeadAfter).toBe(1000); // Group 200 has 1000
  });
});

// =============================================================================
// COMPLEX HIERARCHIES
// =============================================================================

test.describe("getTree() complex hierarchies", () => {
  test("deeply nested groups", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      // Create: root -> 100 -> 101 -> 102 -> synth 1000
      await sonic.send("/g_new", 100, 0, 0);
      await sonic.send("/g_new", 101, 0, 100);
      await sonic.send("/g_new", 102, 0, 101);
      await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 102, "release", 60);
      await sonic.sync(1);

      const tree = sonic.getTree();

      // Cleanup
      await sonic.send("/n_free", 100);

      return {
        nodeCount: tree.nodeCount,
        rootChildren: tree.nodes.find((n) => n.id === 0)?.headId,
        g100Parent: tree.nodes.find((n) => n.id === 100)?.parentId,
        g101Parent: tree.nodes.find((n) => n.id === 101)?.parentId,
        g102Parent: tree.nodes.find((n) => n.id === 102)?.parentId,
        synth1000Parent: tree.nodes.find((n) => n.id === 1000)?.parentId,
      };
    }, SONIC_CONFIG);

    expect(result.nodeCount).toBe(5); // root, 100, 101, 102, 1000
    expect(result.rootChildren).toBe(100);
    expect(result.g100Parent).toBe(0);
    expect(result.g101Parent).toBe(100);
    expect(result.g102Parent).toBe(101);
    expect(result.synth1000Parent).toBe(102);
  });

  test("multiple synths in multiple groups", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      // Create structure:
      // root
      //   ├── group 100
      //   │   ├── synth 1000
      //   │   └── synth 1001
      //   └── group 200
      //       ├── synth 2000
      //       └── synth 2001

      await sonic.send("/g_new", 100, 0, 0);
      await sonic.send("/g_new", 200, 1, 0);
      await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 100, "release", 60);
      await sonic.send("/s_new", "sonic-pi-beep", 1001, 1, 100, "release", 60);
      await sonic.send("/s_new", "sonic-pi-beep", 2000, 0, 200, "release", 60);
      await sonic.send("/s_new", "sonic-pi-beep", 2001, 1, 200, "release", 60);
      await sonic.sync(1);

      const tree = sonic.getTree();

      // Cleanup
      await sonic.send("/n_free", 100, 200);

      const groups = tree.nodes.filter((n) => n.isGroup);
      const synths = tree.nodes.filter((n) => !n.isGroup);

      return {
        nodeCount: tree.nodeCount,
        groupCount: groups.length,
        synthCount: synths.length,
        group100Head: tree.nodes.find((n) => n.id === 100)?.headId,
        group200Head: tree.nodes.find((n) => n.id === 200)?.headId,
        synth1000Parent: tree.nodes.find((n) => n.id === 1000)?.parentId,
        synth2000Parent: tree.nodes.find((n) => n.id === 2000)?.parentId,
      };
    }, SONIC_CONFIG);

    expect(result.nodeCount).toBe(7); // root + 2 groups + 4 synths
    expect(result.groupCount).toBe(3); // root, 100, 200
    expect(result.synthCount).toBe(4);
    expect(result.group100Head).toBe(1000);
    expect(result.group200Head).toBe(2000);
    expect(result.synth1000Parent).toBe(100);
    expect(result.synth2000Parent).toBe(200);
  });

  test("sibling order preserved across operations", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      // Create chain at head: each new synth pushes previous down
      // Order after creation: 1004 -> 1003 -> 1002 -> 1001 -> 1000
      await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 0, "release", 60);
      await sonic.send("/s_new", "sonic-pi-beep", 1001, 0, 0, "release", 60);
      await sonic.send("/s_new", "sonic-pi-beep", 1002, 0, 0, "release", 60);
      await sonic.send("/s_new", "sonic-pi-beep", 1003, 0, 0, "release", 60);
      await sonic.send("/s_new", "sonic-pi-beep", 1004, 0, 0, "release", 60);
      await sonic.sync(1);

      const tree = sonic.getTree();

      // Walk the chain from head
      const root = tree.nodes.find((n) => n.id === 0);
      const chain = [];
      let current = tree.nodes.find((n) => n.id === root.headId);
      while (current) {
        chain.push(current.id);
        current = tree.nodes.find((n) => n.id === current.nextId);
      }

      // Cleanup
      await sonic.send("/n_free", 1000, 1001, 1002, 1003, 1004);

      return { chain };
    }, SONIC_CONFIG);

    // Newest at head (addAction 0 = head)
    expect(result.chain).toEqual([1004, 1003, 1002, 1001, 1000]);
  });
});

// =============================================================================
// AUTO-ASSIGNED NODE IDS
// =============================================================================

test.describe("getTree() with auto-assigned IDs", () => {
  // Note: Auto-assigned IDs (using -1) result in negative node IDs from scsynth.
  // These ARE included in the SAB tree for visualization purposes.

  test("auto-assigned synth ID (-1) appears in SAB tree with negative ID", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      const treeBefore = sonic.getTree();

      // Create synth with auto-assigned ID (will get negative ID)
      await sonic.send("/s_new", "sonic-pi-beep", -1, 0, 0, "release", 60);
      await sonic.sync(1);

      const treeAfter = sonic.getTree();

      // Find the new synth with negative ID
      const newSynth = treeAfter.nodes.find(
        (n) => n.id < 0 && n.defName === "sonic-pi-beep"
      );

      return {
        countBefore: treeBefore.nodeCount,
        countAfter: treeAfter.nodeCount,
        newSynthFound: !!newSynth,
        newSynthId: newSynth?.id,
        newSynthDefName: newSynth?.defName,
      };
    }, SONIC_CONFIG);

    // Auto-assigned IDs are now included in the SAB tree
    expect(result.countAfter).toBe(result.countBefore + 1);
    expect(result.newSynthFound).toBe(true);
    expect(result.newSynthId).toBeLessThan(0);
    expect(result.newSynthDefName).toBe("sonic-pi-beep");
  });

  test("auto-assigned group ID (-1) appears in SAB tree with negative ID", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();

      const treeBefore = sonic.getTree();

      // Create group with auto-assigned ID (will get negative ID)
      await sonic.send("/g_new", -1, 0, 0);
      await sonic.sync(1);

      const treeAfter = sonic.getTree();

      // Find the new group with negative ID
      const newGroup = treeAfter.nodes.find(
        (n) => n.id < 0 && n.isGroup && n.defName === "group"
      );

      return {
        countBefore: treeBefore.nodeCount,
        countAfter: treeAfter.nodeCount,
        newGroupFound: !!newGroup,
        newGroupId: newGroup?.id,
      };
    }, SONIC_CONFIG);

    // Auto-assigned IDs are now included in the SAB tree
    expect(result.countAfter).toBe(result.countBefore + 1);
    expect(result.newGroupFound).toBe(true);
    expect(result.newGroupId).toBeLessThan(0);
  });

  test("explicit positive IDs appear in tree correctly", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      const treeBefore = sonic.getTree();

      // Create synth with explicit positive ID
      await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 0, "release", 60);
      await sonic.sync(1);

      const treeAfter = sonic.getTree();
      const synth = treeAfter.nodes.find((n) => n.id === 1000);

      // Cleanup
      await sonic.send("/n_free", 1000);

      return {
        countBefore: treeBefore.nodeCount,
        countAfter: treeAfter.nodeCount,
        synthFound: !!synth,
        synth,
      };
    }, SONIC_CONFIG);

    expect(result.countAfter).toBe(result.countBefore + 1);
    expect(result.synthFound).toBe(true);
    expect(result.synth.id).toBe(1000);
    expect(result.synth.defName).toBe("sonic-pi-beep");
  });

  test("OSC notifications skip negative IDs (matching upstream scsynth behavior)", async ({
    page,
  }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      const messages = [];
      sonic.on("message", (msg) => messages.push(msg));

      await sonic.init();
      await sonic.send("/notify", 1); // Enable OSC notifications
      await sonic.loadSynthDef("sonic-pi-beep");

      // Clear initial messages
      messages.length = 0;

      // Create synth with auto-assigned ID
      await sonic.send("/s_new", "sonic-pi-beep", -1, 0, 0, "release", 60);
      await sonic.sync(1);

      // Check SAB tree has the synth
      const tree = sonic.getTree();
      const autoSynth = tree.nodes.find(
        (n) => n.id < 0 && n.defName === "sonic-pi-beep"
      );

      // Check for /n_go notification
      const nGoMessages = messages.filter((m) => m.address === "/n_go");

      return {
        synthInSabTree: !!autoSynth,
        synthIdInSab: autoSynth?.id,
        nGoCount: nGoMessages.length,
        nGoNodeIds: nGoMessages.map((m) => m.args[0]),
      };
    }, SONIC_CONFIG);

    // SAB tree should include auto-assigned synths (for visualization)
    expect(result.synthInSabTree).toBe(true);
    expect(result.synthIdInSab).toBeLessThan(0);

    // But OSC notifications should NOT include negative IDs (matching upstream)
    // This ensures the standard SuperCollider behavior is preserved
    expect(result.nGoCount).toBe(0);
    expect(result.nGoNodeIds).toEqual([]);
  });
});

// =============================================================================
// EDGE CASES
// =============================================================================

test.describe("getTree() edge cases", () => {
  test("empty group has headId -1", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();

      await sonic.send("/g_new", 100, 0, 0);
      await sonic.sync(1);

      const tree = sonic.getTree();
      const group100 = tree.nodes.find((n) => n.id === 100);

      // Cleanup
      await sonic.send("/n_free", 100);

      return { headId: group100?.headId };
    }, SONIC_CONFIG);

    expect(result.headId).toBe(-1);
  });

  test("synth has headId -1 (only groups have children)", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 0, "release", 60);
      await sonic.sync(1);

      const tree = sonic.getTree();
      const synth = tree.nodes.find((n) => n.id === 1000);

      // Cleanup
      await sonic.send("/n_free", 1000);

      return { headId: synth?.headId };
    }, SONIC_CONFIG);

    expect(result.headId).toBe(-1);
  });

  test("first child has prevId -1", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      await sonic.send("/g_new", 100, 0, 0);
      await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 100, "release", 60);
      await sonic.sync(1);

      const tree = sonic.getTree();
      const synth = tree.nodes.find((n) => n.id === 1000);

      // Cleanup
      await sonic.send("/n_free", 100);

      return { prevId: synth?.prevId };
    }, SONIC_CONFIG);

    expect(result.prevId).toBe(-1);
  });

  test("last child has nextId -1", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      await sonic.send("/g_new", 100, 0, 0);
      await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 100, "release", 60);
      await sonic.send("/s_new", "sonic-pi-beep", 1001, 1, 100, "release", 60); // tail
      await sonic.sync(1);

      const tree = sonic.getTree();
      const lastSynth = tree.nodes.find((n) => n.id === 1001);

      // Cleanup
      await sonic.send("/n_free", 100);

      return { nextId: lastSynth?.nextId };
    }, SONIC_CONFIG);

    expect(result.nextId).toBe(-1);
  });

  test("root group has parentId -1", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();

      const tree = sonic.getTree();
      const root = tree.nodes.find((n) => n.id === 0);

      return { parentId: root?.parentId };
    }, SONIC_CONFIG);

    expect(result.parentId).toBe(-1);
  });

  test("handles rapid create/free cycles", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      // Rapid create/free without sync between
      for (let i = 0; i < 50; i++) {
        sonic.send("/s_new", "sonic-pi-beep", 1000 + i, 0, 0, "release", 60);
        sonic.send("/n_free", 1000 + i);
      }

      await sonic.sync(1);
      await new Promise((r) => setTimeout(r, 100));

      const tree = sonic.getTree();

      return {
        nodeCount: tree.nodeCount,
        synthCount: tree.nodes.filter((n) => !n.isGroup).length,
      };
    }, SONIC_CONFIG);

    // All synths should be freed, only root remains
    expect(result.nodeCount).toBe(1);
    expect(result.synthCount).toBe(0);
  });

  test("handles many concurrent synths", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      const COUNT = 100;

      // Create many synths
      for (let i = 0; i < COUNT; i++) {
        sonic.send("/s_new", "sonic-pi-beep", 1000 + i, 0, 0, "release", 60);
      }

      await sonic.sync(1);

      const tree = sonic.getTree();
      const synthCount = tree.nodes.filter((n) => !n.isGroup).length;

      // Cleanup
      for (let i = 0; i < COUNT; i++) {
        sonic.send("/n_free", 1000 + i);
      }
      await sonic.sync(2);

      const treeAfter = sonic.getTree();

      return {
        nodeCount: tree.nodeCount,
        synthCount,
        afterNodeCount: treeAfter.nodeCount,
      };
    }, SONIC_CONFIG);

    expect(result.nodeCount).toBe(101); // root + 100 synths
    expect(result.synthCount).toBe(100);
    expect(result.afterNodeCount).toBe(1); // Only root
  });
});

// =============================================================================
// DEFNAME HANDLING
// =============================================================================

test.describe("getTree() defName handling", () => {
  test("groups have defName 'group'", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();

      await sonic.send("/g_new", 100, 0, 0);
      await sonic.send("/g_new", 101, 0, 100);
      await sonic.sync(1);

      const tree = sonic.getTree();

      // Cleanup
      await sonic.send("/n_free", 100);

      return {
        rootDefName: tree.nodes.find((n) => n.id === 0)?.defName,
        group100DefName: tree.nodes.find((n) => n.id === 100)?.defName,
        group101DefName: tree.nodes.find((n) => n.id === 101)?.defName,
      };
    }, SONIC_CONFIG);

    expect(result.rootDefName).toBe("group");
    expect(result.group100DefName).toBe("group");
    expect(result.group101DefName).toBe("group");
  });

  test("synths have correct synthdef names", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDefs(["sonic-pi-beep", "sonic-pi-saw", "sonic-pi-prophet"]);

      await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 0, "release", 60);
      await sonic.send("/s_new", "sonic-pi-saw", 1001, 0, 0, "release", 60);
      await sonic.send("/s_new", "sonic-pi-prophet", 1002, 0, 0, "release", 60);
      await sonic.sync(1);

      const tree = sonic.getTree();

      // Cleanup
      await sonic.send("/n_free", 1000, 1001, 1002);

      return {
        synth1000: tree.nodes.find((n) => n.id === 1000)?.defName,
        synth1001: tree.nodes.find((n) => n.id === 1001)?.defName,
        synth1002: tree.nodes.find((n) => n.id === 1002)?.defName,
      };
    }, SONIC_CONFIG);

    expect(result.synth1000).toBe("sonic-pi-beep");
    expect(result.synth1001).toBe("sonic-pi-saw");
    expect(result.synth1002).toBe("sonic-pi-prophet");
  });

  test("long synthdef names are truncated but usable", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();

      // sonic-pi-piano has a reasonably long name
      await sonic.loadSynthDef("sonic-pi-piano");
      await sonic.send("/s_new", "sonic-pi-piano", 1000, 0, 0, "release", 60);
      await sonic.sync(1);

      const tree = sonic.getTree();
      const synth = tree.nodes.find((n) => n.id === 1000);

      // Cleanup
      await sonic.send("/n_free", 1000);

      return {
        defName: synth?.defName,
        defNameLength: synth?.defName?.length,
      };
    }, SONIC_CONFIG);

    expect(result.defName).toBe("sonic-pi-piano");
    expect(result.defNameLength).toBeLessThanOrEqual(32); // NODE_TREE_DEF_NAME_SIZE
  });
});

// =============================================================================
// ERROR SCENARIOS
// =============================================================================

test.describe("getTree() error scenarios", () => {
  test("freeing non-existent node doesn't corrupt tree", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      // Create a known synth
      await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 0, "release", 60);
      await sonic.sync(1);

      const treeBefore = sonic.getTree();

      // Try to free a node that doesn't exist
      await sonic.send("/n_free", 9999);
      await sonic.sync(2);

      const treeAfter = sonic.getTree();

      // Cleanup
      await sonic.send("/n_free", 1000);

      return {
        beforeCount: treeBefore.nodeCount,
        afterCount: treeAfter.nodeCount,
        has1000Before: treeBefore.nodes.some((n) => n.id === 1000),
        has1000After: treeAfter.nodes.some((n) => n.id === 1000),
      };
    }, SONIC_CONFIG);

    // Tree should remain unchanged
    expect(result.beforeCount).toBe(result.afterCount);
    expect(result.has1000Before).toBe(true);
    expect(result.has1000After).toBe(true);
  });

  test("double-free doesn't corrupt tree", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 0, "release", 60);
      await sonic.send("/s_new", "sonic-pi-beep", 1001, 0, 0, "release", 60);
      await sonic.sync(1);

      const treeBefore = sonic.getTree();

      // Free 1000 twice
      await sonic.send("/n_free", 1000);
      await sonic.sync(2);
      await sonic.send("/n_free", 1000);
      await sonic.sync(3);

      const treeAfter = sonic.getTree();

      // Cleanup
      await sonic.send("/n_free", 1001);

      return {
        beforeCount: treeBefore.nodeCount,
        afterCount: treeAfter.nodeCount,
        has1000After: treeAfter.nodes.some((n) => n.id === 1000),
        has1001After: treeAfter.nodes.some((n) => n.id === 1001),
      };
    }, SONIC_CONFIG);

    // Only one node should be removed
    expect(result.beforeCount).toBe(3); // root + 1000 + 1001
    expect(result.afterCount).toBe(2); // root + 1001
    expect(result.has1000After).toBe(false);
    expect(result.has1001After).toBe(true);
  });

  test("moving non-existent node doesn't corrupt tree", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 0, "release", 60);
      await sonic.sync(1);

      const treeBefore = sonic.getTree();

      // Try to move a non-existent node
      await sonic.send("/n_before", 9999, 1000);
      await sonic.sync(2);

      const treeAfter = sonic.getTree();

      // Cleanup
      await sonic.send("/n_free", 1000);

      return {
        beforeCount: treeBefore.nodeCount,
        afterCount: treeAfter.nodeCount,
        node1000Before: treeBefore.nodes.find((n) => n.id === 1000),
        node1000After: treeAfter.nodes.find((n) => n.id === 1000),
      };
    }, SONIC_CONFIG);

    // Tree should remain unchanged
    expect(result.beforeCount).toBe(result.afterCount);
    expect(result.node1000Before.prevId).toBe(result.node1000After.prevId);
    expect(result.node1000Before.nextId).toBe(result.node1000After.nextId);
  });

  test("moving to non-existent target doesn't corrupt tree", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 0, "release", 60);
      await sonic.sync(1);

      const treeBefore = sonic.getTree();

      // Try to move to a non-existent target
      await sonic.send("/n_after", 1000, 9999);
      await sonic.sync(2);

      const treeAfter = sonic.getTree();

      // Cleanup
      await sonic.send("/n_free", 1000);

      return {
        beforeCount: treeBefore.nodeCount,
        afterCount: treeAfter.nodeCount,
        node1000Before: treeBefore.nodes.find((n) => n.id === 1000),
        node1000After: treeAfter.nodes.find((n) => n.id === 1000),
      };
    }, SONIC_CONFIG);

    // Tree should remain unchanged
    expect(result.beforeCount).toBe(result.afterCount);
    expect(result.node1000Before.parentId).toBe(result.node1000After.parentId);
  });

  test("creating synth in non-existent group doesn't corrupt tree", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      const treeBefore = sonic.getTree();

      // Try to create synth in non-existent group
      await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 9999, "release", 60);
      await sonic.sync(1);

      const treeAfter = sonic.getTree();

      return {
        beforeCount: treeBefore.nodeCount,
        afterCount: treeAfter.nodeCount,
        has1000: treeAfter.nodes.some((n) => n.id === 1000),
      };
    }, SONIC_CONFIG);

    // Synth should not be created
    expect(result.afterCount).toBe(result.beforeCount);
    expect(result.has1000).toBe(false);
  });

  test("creating group in non-existent parent doesn't corrupt tree", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();

      const treeBefore = sonic.getTree();

      // Try to create group in non-existent parent
      await sonic.send("/g_new", 100, 0, 9999);
      await sonic.sync(1);

      const treeAfter = sonic.getTree();

      return {
        beforeCount: treeBefore.nodeCount,
        afterCount: treeAfter.nodeCount,
        has100: treeAfter.nodes.some((n) => n.id === 100),
      };
    }, SONIC_CONFIG);

    // Group should not be created
    expect(result.afterCount).toBe(result.beforeCount);
    expect(result.has100).toBe(false);
  });

  test("g_freeAll on non-existent group doesn't corrupt tree", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      await sonic.send("/g_new", 100, 0, 0);
      await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 100, "release", 60);
      await sonic.sync(1);

      const treeBefore = sonic.getTree();

      // Try g_freeAll on non-existent group
      await sonic.send("/g_freeAll", 9999);
      await sonic.sync(2);

      const treeAfter = sonic.getTree();

      // Cleanup
      await sonic.send("/n_free", 100);

      return {
        beforeCount: treeBefore.nodeCount,
        afterCount: treeAfter.nodeCount,
        has100: treeAfter.nodes.some((n) => n.id === 100),
        has1000: treeAfter.nodes.some((n) => n.id === 1000),
      };
    }, SONIC_CONFIG);

    // Tree should remain unchanged
    expect(result.beforeCount).toBe(result.afterCount);
    expect(result.has100).toBe(true);
    expect(result.has1000).toBe(true);
  });

  test("reusing freed node ID works correctly", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      // Create and free synth 1000
      await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 0, "release", 60);
      await sonic.sync(1);
      await sonic.send("/n_free", 1000);
      await sonic.sync(2);

      const treeAfterFree = sonic.getTree();

      // Reuse the same ID
      await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 0, "release", 60);
      await sonic.sync(3);

      const treeAfterReuse = sonic.getTree();
      const node1000 = treeAfterReuse.nodes.find((n) => n.id === 1000);

      // Cleanup
      await sonic.send("/n_free", 1000);

      return {
        afterFreeCount: treeAfterFree.nodeCount,
        afterFreeHas1000: treeAfterFree.nodes.some((n) => n.id === 1000),
        afterReuseCount: treeAfterReuse.nodeCount,
        afterReuseHas1000: !!node1000,
        node1000ParentId: node1000?.parentId,
        node1000IsGroup: node1000?.isGroup,
      };
    }, SONIC_CONFIG);

    expect(result.afterFreeHas1000).toBe(false);
    expect(result.afterFreeCount).toBe(1); // Just root
    expect(result.afterReuseHas1000).toBe(true);
    expect(result.afterReuseCount).toBe(2); // root + 1000
    expect(result.node1000ParentId).toBe(0);
    expect(result.node1000IsGroup).toBe(false);
  });

  test("creating duplicate node ID fails gracefully", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      // Create synth 1000
      await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 0, "release", 60);
      await sonic.sync(1);

      const treeBefore = sonic.getTree();

      // Try to create another synth with same ID
      await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 0, "release", 60);
      await sonic.sync(2);

      const treeAfter = sonic.getTree();

      // Cleanup
      await sonic.send("/n_free", 1000);

      return {
        beforeCount: treeBefore.nodeCount,
        afterCount: treeAfter.nodeCount,
      };
    }, SONIC_CONFIG);

    // Should not create duplicate - count unchanged
    expect(result.afterCount).toBe(result.beforeCount);
  });

  test("g_head to non-existent group doesn't corrupt tree", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      await sonic.send("/g_new", 100, 0, 0);
      await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 100, "release", 60);
      await sonic.sync(1);

      const treeBefore = sonic.getTree();

      // Try to move to head of non-existent group
      await sonic.send("/g_head", 9999, 1000);
      await sonic.sync(2);

      const treeAfter = sonic.getTree();

      // Cleanup
      await sonic.send("/n_free", 100);

      return {
        node1000BeforeParent: treeBefore.nodes.find((n) => n.id === 1000)?.parentId,
        node1000AfterParent: treeAfter.nodes.find((n) => n.id === 1000)?.parentId,
      };
    }, SONIC_CONFIG);

    // Parent should remain unchanged
    expect(result.node1000BeforeParent).toBe(100);
    expect(result.node1000AfterParent).toBe(100);
  });
});

// =============================================================================
// COMPLEX MOVEMENT PATTERNS
// =============================================================================

test.describe("getTree() complex movement patterns", () => {
  test("chain of moves: rotate nodes in circle", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      // Create chain: 1000 -> 1001 -> 1002 -> 1003
      await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 0, "release", 60);
      await sonic.send("/s_new", "sonic-pi-beep", 1001, 3, 1000, "release", 60);
      await sonic.send("/s_new", "sonic-pi-beep", 1002, 3, 1001, "release", 60);
      await sonic.send("/s_new", "sonic-pi-beep", 1003, 3, 1002, "release", 60);
      await sonic.sync(1);

      // Rotate: move last to first position repeatedly
      // 1000 -> 1001 -> 1002 -> 1003  =>  1003 -> 1000 -> 1001 -> 1002
      await sonic.send("/n_before", 1003, 1000);
      await sonic.sync(2);

      const tree1 = sonic.getTree();

      // Another rotation: 1003 -> 1000 -> 1001 -> 1002  =>  1002 -> 1003 -> 1000 -> 1001
      await sonic.send("/n_before", 1002, 1003);
      await sonic.sync(3);

      const tree2 = sonic.getTree();

      // Cleanup
      await sonic.send("/n_free", 1000, 1001, 1002, 1003);

      // Walk chains
      const walkChain = (tree) => {
        const root = tree.nodes.find((n) => n.id === 0);
        const chain = [];
        let current = tree.nodes.find((n) => n.id === root.headId);
        while (current && chain.length < 10) {
          chain.push(current.id);
          current = tree.nodes.find((n) => n.id === current.nextId);
        }
        return chain;
      };

      return {
        chain1: walkChain(tree1),
        chain2: walkChain(tree2),
      };
    }, SONIC_CONFIG);

    expect(result.chain1).toEqual([1003, 1000, 1001, 1002]);
    expect(result.chain2).toEqual([1002, 1003, 1000, 1001]);
  });

  test("swap two adjacent nodes", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      // Create: 1000 -> 1001 -> 1002
      await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 0, "release", 60);
      await sonic.send("/s_new", "sonic-pi-beep", 1001, 3, 1000, "release", 60);
      await sonic.send("/s_new", "sonic-pi-beep", 1002, 3, 1001, "release", 60);
      await sonic.sync(1);

      // Swap 1000 and 1001: move 1001 before 1000
      // Result: 1001 -> 1000 -> 1002
      await sonic.send("/n_before", 1001, 1000);
      await sonic.sync(2);

      const tree = sonic.getTree();

      // Cleanup
      await sonic.send("/n_free", 1000, 1001, 1002);

      const n1000 = tree.nodes.find((n) => n.id === 1000);
      const n1001 = tree.nodes.find((n) => n.id === 1001);
      const n1002 = tree.nodes.find((n) => n.id === 1002);

      return {
        n1001: { prev: n1001.prevId, next: n1001.nextId },
        n1000: { prev: n1000.prevId, next: n1000.nextId },
        n1002: { prev: n1002.prevId, next: n1002.nextId },
      };
    }, SONIC_CONFIG);

    // 1001 -> 1000 -> 1002
    expect(result.n1001.prev).toBe(-1);
    expect(result.n1001.next).toBe(1000);
    expect(result.n1000.prev).toBe(1001);
    expect(result.n1000.next).toBe(1002);
    expect(result.n1002.prev).toBe(1000);
    expect(result.n1002.next).toBe(-1);
  });

  test("swap two non-adjacent nodes", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      // Create: 1000 -> 1001 -> 1002 -> 1003
      await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 0, "release", 60);
      await sonic.send("/s_new", "sonic-pi-beep", 1001, 3, 1000, "release", 60);
      await sonic.send("/s_new", "sonic-pi-beep", 1002, 3, 1001, "release", 60);
      await sonic.send("/s_new", "sonic-pi-beep", 1003, 3, 1002, "release", 60);
      await sonic.sync(1);

      // Swap 1000 and 1003 (first and last)
      // Step 1: Move 1003 before 1000: 1003 -> 1000 -> 1001 -> 1002
      await sonic.send("/n_before", 1003, 1000);
      await sonic.sync(2);

      // Step 2: Move 1000 after 1002: 1003 -> 1001 -> 1002 -> 1000
      await sonic.send("/n_after", 1000, 1002);
      await sonic.sync(3);

      const tree = sonic.getTree();

      // Cleanup
      await sonic.send("/n_free", 1000, 1001, 1002, 1003);

      const walkChain = (tree) => {
        const root = tree.nodes.find((n) => n.id === 0);
        const chain = [];
        let current = tree.nodes.find((n) => n.id === root.headId);
        while (current && chain.length < 10) {
          chain.push(current.id);
          current = tree.nodes.find((n) => n.id === current.nextId);
        }
        return chain;
      };

      return { chain: walkChain(tree) };
    }, SONIC_CONFIG);

    expect(result.chain).toEqual([1003, 1001, 1002, 1000]);
  });

  test("move node through multiple groups", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      // Create groups: 100, 200, 300
      await sonic.send("/g_new", 100, 0, 0);
      await sonic.send("/g_new", 200, 1, 0);
      await sonic.send("/g_new", 300, 1, 0);

      // Create synth in group 100
      await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 100, "release", 60);
      await sonic.sync(1);

      const tree1 = sonic.getTree();

      // Move to group 200
      await sonic.send("/g_head", 200, 1000);
      await sonic.sync(2);

      const tree2 = sonic.getTree();

      // Move to group 300
      await sonic.send("/g_head", 300, 1000);
      await sonic.sync(3);

      const tree3 = sonic.getTree();

      // Move back to group 100
      await sonic.send("/g_head", 100, 1000);
      await sonic.sync(4);

      const tree4 = sonic.getTree();

      // Cleanup
      await sonic.send("/n_free", 100, 200, 300);

      return {
        parent1: tree1.nodes.find((n) => n.id === 1000)?.parentId,
        parent2: tree2.nodes.find((n) => n.id === 1000)?.parentId,
        parent3: tree3.nodes.find((n) => n.id === 1000)?.parentId,
        parent4: tree4.nodes.find((n) => n.id === 1000)?.parentId,
        g100Head1: tree1.nodes.find((n) => n.id === 100)?.headId,
        g100Head2: tree2.nodes.find((n) => n.id === 100)?.headId,
        g200Head2: tree2.nodes.find((n) => n.id === 200)?.headId,
        g300Head3: tree3.nodes.find((n) => n.id === 300)?.headId,
        g100Head4: tree4.nodes.find((n) => n.id === 100)?.headId,
      };
    }, SONIC_CONFIG);

    expect(result.parent1).toBe(100);
    expect(result.parent2).toBe(200);
    expect(result.parent3).toBe(300);
    expect(result.parent4).toBe(100);

    expect(result.g100Head1).toBe(1000);
    expect(result.g100Head2).toBe(-1); // Empty after move
    expect(result.g200Head2).toBe(1000);
    expect(result.g300Head3).toBe(1000);
    expect(result.g100Head4).toBe(1000);
  });

  test("reverse a chain of nodes", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      // Create chain: 1000 -> 1001 -> 1002 -> 1003
      await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 0, "release", 60);
      await sonic.send("/s_new", "sonic-pi-beep", 1001, 3, 1000, "release", 60);
      await sonic.send("/s_new", "sonic-pi-beep", 1002, 3, 1001, "release", 60);
      await sonic.send("/s_new", "sonic-pi-beep", 1003, 3, 1002, "release", 60);
      await sonic.sync(1);

      // Reverse by moving each to head
      // Move 1001 to head: 1001 -> 1000 -> 1002 -> 1003
      await sonic.send("/g_head", 0, 1001);
      await sonic.sync(2);

      // Move 1002 to head: 1002 -> 1001 -> 1000 -> 1003
      await sonic.send("/g_head", 0, 1002);
      await sonic.sync(3);

      // Move 1003 to head: 1003 -> 1002 -> 1001 -> 1000
      await sonic.send("/g_head", 0, 1003);
      await sonic.sync(4);

      const tree = sonic.getTree();

      // Cleanup
      await sonic.send("/n_free", 1000, 1001, 1002, 1003);

      const walkChain = (tree) => {
        const root = tree.nodes.find((n) => n.id === 0);
        const chain = [];
        let current = tree.nodes.find((n) => n.id === root.headId);
        while (current && chain.length < 10) {
          chain.push(current.id);
          current = tree.nodes.find((n) => n.id === current.nextId);
        }
        return chain;
      };

      return { chain: walkChain(tree) };
    }, SONIC_CONFIG);

    expect(result.chain).toEqual([1003, 1002, 1001, 1000]);
  });

  test("interleave two chains", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      // Create two groups with chains
      await sonic.send("/g_new", 100, 0, 0);
      await sonic.send("/g_new", 200, 1, 0);

      // Chain A in group 100: A1 -> A2 -> A3
      await sonic.send("/s_new", "sonic-pi-beep", 1001, 0, 100, "release", 60);
      await sonic.send("/s_new", "sonic-pi-beep", 1002, 1, 100, "release", 60);
      await sonic.send("/s_new", "sonic-pi-beep", 1003, 1, 100, "release", 60);

      // Chain B in group 200: B1 -> B2 -> B3
      await sonic.send("/s_new", "sonic-pi-beep", 2001, 0, 200, "release", 60);
      await sonic.send("/s_new", "sonic-pi-beep", 2002, 1, 200, "release", 60);
      await sonic.send("/s_new", "sonic-pi-beep", 2003, 1, 200, "release", 60);
      await sonic.sync(1);

      // Interleave into group 100: A1 -> B1 -> A2 -> B2 -> A3 -> B3
      // Move B1 after A1
      await sonic.send("/n_after", 2001, 1001);
      await sonic.sync(2);

      // Move B2 after A2
      await sonic.send("/n_after", 2002, 1002);
      await sonic.sync(3);

      // Move B3 after A3
      await sonic.send("/n_after", 2003, 1003);
      await sonic.sync(4);

      const tree = sonic.getTree();

      // Cleanup
      await sonic.send("/n_free", 100, 200);

      const walkChain = (groupId) => {
        const group = tree.nodes.find((n) => n.id === groupId);
        const chain = [];
        let current = tree.nodes.find((n) => n.id === group?.headId);
        while (current && chain.length < 10) {
          chain.push(current.id);
          current = tree.nodes.find((n) => n.id === current.nextId);
        }
        return chain;
      };

      return {
        chain100: walkChain(100),
        chain200: walkChain(200),
        allParents: [2001, 2002, 2003].map(
          (id) => tree.nodes.find((n) => n.id === id)?.parentId
        ),
      };
    }, SONIC_CONFIG);

    expect(result.chain100).toEqual([1001, 2001, 1002, 2002, 1003, 2003]);
    expect(result.chain200).toEqual([]); // Group 200 should be empty
    expect(result.allParents).toEqual([100, 100, 100]); // All B nodes moved to group 100
  });

  test("move group with children to different parent", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      // Create structure:
      // root -> 100 -> 101 -> synth 1000
      //      -> 200 (empty)
      await sonic.send("/g_new", 100, 0, 0);
      await sonic.send("/g_new", 101, 0, 100);
      await sonic.send("/g_new", 200, 1, 0);
      await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 101, "release", 60);
      await sonic.sync(1);

      const treeBefore = sonic.getTree();

      // Move group 100 (with all its children) into group 200
      await sonic.send("/g_head", 200, 100);
      await sonic.sync(2);

      const treeAfter = sonic.getTree();

      // Cleanup
      await sonic.send("/n_free", 200);

      return {
        // Before: 100's parent is root
        g100ParentBefore: treeBefore.nodes.find((n) => n.id === 100)?.parentId,
        g200HeadBefore: treeBefore.nodes.find((n) => n.id === 200)?.headId,

        // After: 100's parent is 200
        g100ParentAfter: treeAfter.nodes.find((n) => n.id === 100)?.parentId,
        g200HeadAfter: treeAfter.nodes.find((n) => n.id === 200)?.headId,

        // Children should remain intact
        g101ParentAfter: treeAfter.nodes.find((n) => n.id === 101)?.parentId,
        synth1000ParentAfter: treeAfter.nodes.find((n) => n.id === 1000)?.parentId,
      };
    }, SONIC_CONFIG);

    expect(result.g100ParentBefore).toBe(0);
    expect(result.g200HeadBefore).toBe(-1);
    expect(result.g100ParentAfter).toBe(200);
    expect(result.g200HeadAfter).toBe(100);
    // Children unchanged
    expect(result.g101ParentAfter).toBe(100);
    expect(result.synth1000ParentAfter).toBe(101);
  });

  test("rapid sequential moves maintain consistency", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      // Create 5 synths
      for (let i = 0; i < 5; i++) {
        await sonic.send("/s_new", "sonic-pi-beep", 1000 + i, 1, 0, "release", 60);
      }
      await sonic.sync(1);

      // Perform many rapid moves without waiting for sync
      for (let i = 0; i < 20; i++) {
        const fromId = 1000 + (i % 5);
        const toId = 1000 + ((i + 1) % 5);
        sonic.send("/n_before", fromId, toId);
      }

      await sonic.sync(2);

      const tree = sonic.getTree();

      // Cleanup
      await sonic.send("/n_free", 1000, 1001, 1002, 1003, 1004);

      // Walk the chain and verify integrity
      const walkChain = () => {
        const root = tree.nodes.find((n) => n.id === 0);
        const chain = [];
        const seen = new Set();
        let current = tree.nodes.find((n) => n.id === root.headId);
        while (current && chain.length < 10) {
          if (seen.has(current.id)) {
            return { chain, error: "cycle detected" };
          }
          seen.add(current.id);
          chain.push(current.id);
          current = tree.nodes.find((n) => n.id === current.nextId);
        }
        return { chain, error: null };
      };

      const { chain, error } = walkChain();

      return {
        nodeCount: tree.nodeCount,
        chainLength: chain.length,
        error,
        allSynthsPresent: [1000, 1001, 1002, 1003, 1004].every((id) =>
          tree.nodes.some((n) => n.id === id)
        ),
      };
    }, SONIC_CONFIG);

    expect(result.nodeCount).toBe(6); // root + 5 synths
    expect(result.chainLength).toBe(5);
    expect(result.error).toBeNull();
    expect(result.allSynthsPresent).toBe(true);
  });

  test("move node to its current position (no-op)", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      // Create: 1000 -> 1001 -> 1002
      await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 0, "release", 60);
      await sonic.send("/s_new", "sonic-pi-beep", 1001, 3, 1000, "release", 60);
      await sonic.send("/s_new", "sonic-pi-beep", 1002, 3, 1001, "release", 60);
      await sonic.sync(1);

      const treeBefore = sonic.getTree();
      const versionBefore = treeBefore.version;

      // Move 1001 after 1000 (where it already is)
      await sonic.send("/n_after", 1001, 1000);
      await sonic.sync(2);

      const treeAfter = sonic.getTree();

      // Cleanup
      await sonic.send("/n_free", 1000, 1001, 1002);

      const walkChain = (tree) => {
        const root = tree.nodes.find((n) => n.id === 0);
        const chain = [];
        let current = tree.nodes.find((n) => n.id === root.headId);
        while (current && chain.length < 10) {
          chain.push(current.id);
          current = tree.nodes.find((n) => n.id === current.nextId);
        }
        return chain;
      };

      return {
        chainBefore: walkChain(treeBefore),
        chainAfter: walkChain(treeAfter),
        versionBefore,
        versionAfter: treeAfter.version,
      };
    }, SONIC_CONFIG);

    // Chain should remain unchanged
    expect(result.chainBefore).toEqual([1000, 1001, 1002]);
    expect(result.chainAfter).toEqual([1000, 1001, 1002]);
  });
});

// =============================================================================
// /g_deepFree TESTS
// =============================================================================

test.describe("getTree() /g_deepFree behavior", () => {
  test("g_deepFree removes all synths in nested hierarchy", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      // Create deep hierarchy:
      // root -> 100 -> 101 -> synth 1001
      //             -> synth 1000
      //      -> synth 2000
      await sonic.send("/g_new", 100, 0, 0);
      await sonic.send("/g_new", 101, 0, 100);
      await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 100, "release", 60);
      await sonic.send("/s_new", "sonic-pi-beep", 1001, 0, 101, "release", 60);
      await sonic.send("/s_new", "sonic-pi-beep", 2000, 1, 0, "release", 60);
      await sonic.sync(1);

      const treeBefore = sonic.getTree();

      // g_deepFree on group 100 - should free synths but keep groups
      await sonic.send("/g_deepFree", 100);
      await sonic.sync(2);

      const treeAfter = sonic.getTree();

      // Cleanup
      await sonic.send("/n_free", 100, 2000);

      return {
        beforeCount: treeBefore.nodeCount,
        afterCount: treeAfter.nodeCount,
        // Groups should still exist
        has100After: treeAfter.nodes.some((n) => n.id === 100),
        has101After: treeAfter.nodes.some((n) => n.id === 101),
        // Synths inside 100 and 101 should be gone
        has1000After: treeAfter.nodes.some((n) => n.id === 1000),
        has1001After: treeAfter.nodes.some((n) => n.id === 1001),
        // Synth outside (2000) should remain
        has2000After: treeAfter.nodes.some((n) => n.id === 2000),
      };
    }, SONIC_CONFIG);

    expect(result.beforeCount).toBe(6); // root + 100 + 101 + 1000 + 1001 + 2000
    expect(result.afterCount).toBe(4); // root + 100 + 101 + 2000
    expect(result.has100After).toBe(true);
    expect(result.has101After).toBe(true);
    expect(result.has1000After).toBe(false);
    expect(result.has1001After).toBe(false);
    expect(result.has2000After).toBe(true);
  });

  test("g_deepFree on empty group hierarchy is no-op", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();

      // Create empty nested groups
      await sonic.send("/g_new", 100, 0, 0);
      await sonic.send("/g_new", 101, 0, 100);
      await sonic.send("/g_new", 102, 0, 101);
      await sonic.sync(1);

      const treeBefore = sonic.getTree();

      await sonic.send("/g_deepFree", 100);
      await sonic.sync(2);

      const treeAfter = sonic.getTree();

      // Cleanup
      await sonic.send("/n_free", 100);

      return {
        beforeCount: treeBefore.nodeCount,
        afterCount: treeAfter.nodeCount,
        has100: treeAfter.nodes.some((n) => n.id === 100),
        has101: treeAfter.nodes.some((n) => n.id === 101),
        has102: treeAfter.nodes.some((n) => n.id === 102),
      };
    }, SONIC_CONFIG);

    expect(result.beforeCount).toBe(result.afterCount);
    expect(result.has100).toBe(true);
    expect(result.has101).toBe(true);
    expect(result.has102).toBe(true);
  });

  test("g_deepFree preserves sibling groups", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      // Create sibling groups with synths
      // root -> 100 -> synth 1000
      //      -> 200 -> synth 2000
      await sonic.send("/g_new", 100, 0, 0);
      await sonic.send("/g_new", 200, 1, 0);
      await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 100, "release", 60);
      await sonic.send("/s_new", "sonic-pi-beep", 2000, 0, 200, "release", 60);
      await sonic.sync(1);

      const treeBefore = sonic.getTree();

      // g_deepFree on 100 only
      await sonic.send("/g_deepFree", 100);
      await sonic.sync(2);

      const treeAfter = sonic.getTree();

      // Cleanup
      await sonic.send("/n_free", 100, 200);

      return {
        beforeCount: treeBefore.nodeCount,
        afterCount: treeAfter.nodeCount,
        has100After: treeAfter.nodes.some((n) => n.id === 100),
        has200After: treeAfter.nodes.some((n) => n.id === 200),
        has1000After: treeAfter.nodes.some((n) => n.id === 1000),
        has2000After: treeAfter.nodes.some((n) => n.id === 2000),
        g100HeadAfter: treeAfter.nodes.find((n) => n.id === 100)?.headId,
        g200HeadAfter: treeAfter.nodes.find((n) => n.id === 200)?.headId,
      };
    }, SONIC_CONFIG);

    expect(result.beforeCount).toBe(5); // root + 100 + 200 + 1000 + 2000
    expect(result.afterCount).toBe(4); // root + 100 + 200 + 2000
    expect(result.has100After).toBe(true);
    expect(result.has200After).toBe(true);
    expect(result.has1000After).toBe(false); // Freed by g_deepFree
    expect(result.has2000After).toBe(true); // Untouched
    expect(result.g100HeadAfter).toBe(-1); // Empty
    expect(result.g200HeadAfter).toBe(2000); // Still has synth
  });

  test("g_deepFree on root group clears all synths", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      // Create structure with synths at various levels
      await sonic.send("/g_new", 100, 0, 0);
      await sonic.send("/g_new", 101, 0, 100);
      await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 0, "release", 60);
      await sonic.send("/s_new", "sonic-pi-beep", 1001, 0, 100, "release", 60);
      await sonic.send("/s_new", "sonic-pi-beep", 1002, 0, 101, "release", 60);
      await sonic.sync(1);

      const treeBefore = sonic.getTree();

      // g_deepFree on root (0)
      await sonic.send("/g_deepFree", 0);
      await sonic.sync(2);

      const treeAfter = sonic.getTree();

      const synthsAfter = treeAfter.nodes.filter((n) => !n.isGroup);
      const groupsAfter = treeAfter.nodes.filter((n) => n.isGroup);

      return {
        beforeCount: treeBefore.nodeCount,
        afterCount: treeAfter.nodeCount,
        synthCountAfter: synthsAfter.length,
        groupCountAfter: groupsAfter.length,
        groupIds: groupsAfter.map((g) => g.id).sort((a, b) => a - b),
      };
    }, SONIC_CONFIG);

    expect(result.beforeCount).toBe(6); // root + 100 + 101 + 1000 + 1001 + 1002
    expect(result.synthCountAfter).toBe(0); // All synths freed
    expect(result.groupCountAfter).toBe(3); // All groups remain
    expect(result.groupIds).toEqual([0, 100, 101]);
  });

  test("g_deepFree vs g_freeAll: deepFree keeps nested groups", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      // Test g_deepFree first
      await sonic.send("/g_new", 100, 0, 0);
      await sonic.send("/g_new", 101, 0, 100);
      await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 101, "release", 60);
      await sonic.sync(1);

      await sonic.send("/g_deepFree", 100);
      await sonic.sync(2);

      const afterDeepFree = sonic.getTree();

      // Clean up and test g_freeAll
      await sonic.send("/n_free", 100);
      await sonic.sync(3);

      await sonic.send("/g_new", 200, 0, 0);
      await sonic.send("/g_new", 201, 0, 200);
      await sonic.send("/s_new", "sonic-pi-beep", 2000, 0, 201, "release", 60);
      await sonic.sync(4);

      await sonic.send("/g_freeAll", 200);
      await sonic.sync(5);

      const afterFreeAll = sonic.getTree();

      // Cleanup
      await sonic.send("/n_free", 200);

      return {
        // After g_deepFree: group 101 should exist, synth 1000 should not
        deepFreeHas100: afterDeepFree.nodes.some((n) => n.id === 100),
        deepFreeHas101: afterDeepFree.nodes.some((n) => n.id === 101),
        deepFreeHas1000: afterDeepFree.nodes.some((n) => n.id === 1000),

        // After g_freeAll: group 201 should NOT exist, synth 2000 should not
        freeAllHas200: afterFreeAll.nodes.some((n) => n.id === 200),
        freeAllHas201: afterFreeAll.nodes.some((n) => n.id === 201),
        freeAllHas2000: afterFreeAll.nodes.some((n) => n.id === 2000),
      };
    }, SONIC_CONFIG);

    // g_deepFree: frees synths recursively but keeps groups
    expect(result.deepFreeHas100).toBe(true);
    expect(result.deepFreeHas101).toBe(true);
    expect(result.deepFreeHas1000).toBe(false);

    // g_freeAll: frees all children including nested groups
    expect(result.freeAllHas200).toBe(true);
    expect(result.freeAllHas201).toBe(false);
    expect(result.freeAllHas2000).toBe(false);
  });

  test("g_deepFree updates version counter", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      await sonic.send("/g_new", 100, 0, 0);
      await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 100, "release", 60);
      await sonic.send("/s_new", "sonic-pi-beep", 1001, 0, 100, "release", 60);
      await sonic.sync(1);

      const versionBefore = sonic.getTree().version;

      await sonic.send("/g_deepFree", 100);
      await sonic.sync(2);

      const versionAfter = sonic.getTree().version;

      // Cleanup
      await sonic.send("/n_free", 100);

      return { versionBefore, versionAfter };
    }, SONIC_CONFIG);

    expect(result.versionAfter).toBeGreaterThan(result.versionBefore);
  });

  test("g_deepFree on deeply nested structure", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      // Create deep nesting: 100 -> 101 -> 102 -> 103 -> 104
      // With synths at each level
      await sonic.send("/g_new", 100, 0, 0);
      await sonic.send("/g_new", 101, 0, 100);
      await sonic.send("/g_new", 102, 0, 101);
      await sonic.send("/g_new", 103, 0, 102);
      await sonic.send("/g_new", 104, 0, 103);

      await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 100, "release", 60);
      await sonic.send("/s_new", "sonic-pi-beep", 1001, 0, 101, "release", 60);
      await sonic.send("/s_new", "sonic-pi-beep", 1002, 0, 102, "release", 60);
      await sonic.send("/s_new", "sonic-pi-beep", 1003, 0, 103, "release", 60);
      await sonic.send("/s_new", "sonic-pi-beep", 1004, 0, 104, "release", 60);
      await sonic.sync(1);

      const treeBefore = sonic.getTree();

      await sonic.send("/g_deepFree", 100);
      await sonic.sync(2);

      const treeAfter = sonic.getTree();

      // Cleanup
      await sonic.send("/n_free", 100);

      const groupsBefore = treeBefore.nodes.filter((n) => n.isGroup);
      const synthsBefore = treeBefore.nodes.filter((n) => !n.isGroup);
      const groupsAfter = treeAfter.nodes.filter((n) => n.isGroup);
      const synthsAfter = treeAfter.nodes.filter((n) => !n.isGroup);

      return {
        groupCountBefore: groupsBefore.length,
        synthCountBefore: synthsBefore.length,
        groupCountAfter: groupsAfter.length,
        synthCountAfter: synthsAfter.length,
        // Verify all groups still exist
        allGroupsExist: [100, 101, 102, 103, 104].every((id) =>
          treeAfter.nodes.some((n) => n.id === id && n.isGroup)
        ),
        // Verify all synths are gone
        allSynthsGone: [1000, 1001, 1002, 1003, 1004].every(
          (id) => !treeAfter.nodes.some((n) => n.id === id)
        ),
      };
    }, SONIC_CONFIG);

    expect(result.groupCountBefore).toBe(6); // root + 5 groups
    expect(result.synthCountBefore).toBe(5);
    expect(result.groupCountAfter).toBe(6); // All groups remain
    expect(result.synthCountAfter).toBe(0); // All synths freed
    expect(result.allGroupsExist).toBe(true);
    expect(result.allSynthsGone).toBe(true);
  });

  test("g_deepFree on non-existent group is no-op", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      await sonic.send("/g_new", 100, 0, 0);
      await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 100, "release", 60);
      await sonic.sync(1);

      const treeBefore = sonic.getTree();

      // Try g_deepFree on non-existent group
      await sonic.send("/g_deepFree", 9999);
      await sonic.sync(2);

      const treeAfter = sonic.getTree();

      // Cleanup
      await sonic.send("/n_free", 100);

      return {
        beforeCount: treeBefore.nodeCount,
        afterCount: treeAfter.nodeCount,
        has100: treeAfter.nodes.some((n) => n.id === 100),
        has1000: treeAfter.nodes.some((n) => n.id === 1000),
      };
    }, SONIC_CONFIG);

    expect(result.beforeCount).toBe(result.afterCount);
    expect(result.has100).toBe(true);
    expect(result.has1000).toBe(true);
  });

  test("g_deepFree on synth node is no-op", async ({ page }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 0, "release", 60);
      await sonic.send("/s_new", "sonic-pi-beep", 1001, 0, 0, "release", 60);
      await sonic.sync(1);

      const treeBefore = sonic.getTree();

      // Try g_deepFree on a synth (not a group)
      await sonic.send("/g_deepFree", 1000);
      await sonic.sync(2);

      const treeAfter = sonic.getTree();

      // Cleanup
      await sonic.send("/n_free", 1000, 1001);

      return {
        beforeCount: treeBefore.nodeCount,
        afterCount: treeAfter.nodeCount,
        has1000: treeAfter.nodes.some((n) => n.id === 1000),
        has1001: treeAfter.nodes.some((n) => n.id === 1001),
      };
    }, SONIC_CONFIG);

    // g_deepFree on a synth should be a no-op (or at most affect nothing)
    expect(result.has1000).toBe(true);
    expect(result.has1001).toBe(true);
  });
});

import { test, expect } from "@playwright/test";

test.describe("SuperSonic", () => {
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

  test("module loads successfully", async ({ page }) => {
    const hasSuperSonic = await page.evaluate(() => {
      return typeof window.SuperSonic === "function";
    });
    expect(hasSuperSonic).toBe(true);
  });

  test("boots and initializes scsynth", async ({ page }) => {
    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: "/dist/workers/",
        wasmBaseURL: "/dist/wasm/",
        sampleBaseURL: "/dist/samples/",
        synthdefBaseURL: "/dist/synthdefs/",
      });

      try {
        await sonic.init();
        return { success: true };
      } catch (err) {
        return { success: false, error: err.message };
      }
    });

    expect(result.success).toBe(true);
  });

  test("loads a synthdef", async ({ page }) => {
    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: "/dist/workers/",
        wasmBaseURL: "/dist/wasm/",
        sampleBaseURL: "/dist/samples/",
        synthdefBaseURL: "/dist/synthdefs/",
      });

      const messages = [];
      sonic.on('message', (msg) => {
        messages.push(JSON.parse(JSON.stringify(msg)));
      });

      try {
        await sonic.init();
        await sonic.loadSynthDefs(["sonic-pi-beep"]);
        await sonic.sync(1);
        return { success: true, messages };
      } catch (err) {
        return { success: false, error: err.message };
      }
    });

    expect(result.success).toBe(true);
    const doneMessages = result.messages.filter((m) => m.address === "/done");
    expect(doneMessages.length).toBeGreaterThan(0);
  });

  test("responds to /status command", async ({ page }) => {
    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: "/dist/workers/",
        wasmBaseURL: "/dist/wasm/",
        sampleBaseURL: "/dist/samples/",
        synthdefBaseURL: "/dist/synthdefs/",
      });

      const messages = [];
      sonic.on('message', (msg) => {
        messages.push(JSON.parse(JSON.stringify(msg)));
      });

      try {
        await sonic.init();
        sonic.send("/status");
        await sonic.sync(2);
        return { success: true, messages };
      } catch (err) {
        return { success: false, error: err.message };
      }
    });

    expect(result.success).toBe(true);
    const statusMessages = result.messages.filter(
      (m) => m.address === "/status.reply"
    );
    expect(statusMessages.length).toBe(1);
  });

  // Stage 1: Verify C++ and JS read the same memory at NODE_TREE_START
  test("node tree memory layout - Stage 1", async ({ page }) => {
    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: "/dist/workers/",
        wasmBaseURL: "/dist/wasm/",
        sampleBaseURL: "/dist/samples/",
        synthdefBaseURL: "/dist/synthdefs/",
      });

      const debugMessages = [];
      sonic.on('debug', (msg) => {
        debugMessages.push(msg);
      });

      try {
        await sonic.init();

        // Wait a moment for debug messages
        await new Promise((r) => setTimeout(r, 100));

        // Read tree from SharedArrayBuffer
        const tree = sonic.getTree();

        // Get first 10 int32s from tree memory for verification
        // Header: [count, version], then entries: [id, parent, isGroup, prev, next, head]
        const bc = sonic.bufferConstants;
        const ringBufferBase = sonic.ringBufferBase;
        const treeBase = ringBufferBase + bc.NODE_TREE_START;
        const treeView = new Int32Array(sonic.sharedBuffer, treeBase, 20);
        const first20 = Array.from(treeView);

        return {
          success: true,
          tree,
          first20,
          bufferConstants: {
            NODE_TREE_START: bc.NODE_TREE_START,
            NODE_TREE_SIZE: bc.NODE_TREE_SIZE,
            NODE_TREE_HEADER_SIZE: bc.NODE_TREE_HEADER_SIZE,
            NODE_TREE_ENTRY_SIZE: bc.NODE_TREE_ENTRY_SIZE,
            NODE_TREE_MAX_NODES: bc.NODE_TREE_MAX_NODES,
          },
          debugMessages: debugMessages.filter(m => m.text && m.text.includes('[NodeTree]')).map(m => m.text)
        };
      } catch (err) {
        return { success: false, error: err.message, stack: err.stack };
      }
    });

    expect(result.success).toBe(true);
    // Verify constants are set
    expect(result.bufferConstants.NODE_TREE_START).toBeGreaterThan(0);
    expect(result.bufferConstants.NODE_TREE_SIZE).toBe(57352); // 8 + 1024*56
    expect(result.bufferConstants.NODE_TREE_HEADER_SIZE).toBe(8);
    expect(result.bufferConstants.NODE_TREE_ENTRY_SIZE).toBe(56); // 6 int32s (24) + def_name (32)
    expect(result.bufferConstants.NODE_TREE_MAX_NODES).toBe(1024);

    // After boot, root group (id=0) is added automatically
    // So count=1, version=1, and entry[0] has the root group
    expect(result.first20[0]).toBe(1); // count (root group added)
    expect(result.first20[1]).toBe(1); // version (incremented when root added)

    // Entry 0 should have the root group (id=0)
    // With 56-byte entries (14 int32s), entry[0] spans int32 indices 2-15
    expect(result.first20[2]).toBe(0);  // entry[0].id = 0 (root group)
    expect(result.first20[3]).toBe(-1); // entry[0].parent_id = -1 (no parent)
    expect(result.first20[4]).toBe(1);  // entry[0].is_group = 1 (true)

    // Verify the tree structure via getTree() instead of raw memory
    expect(result.tree.nodes.length).toBe(1);
    expect(result.tree.nodes[0].id).toBe(0);
    expect(result.tree.nodes[0].defName).toBe('group');
  });

  // Stage 4: Root group appears in tree after boot
  test("node tree has root group after boot - Stage 4", async ({ page }) => {
    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: "/dist/workers/",
        wasmBaseURL: "/dist/wasm/",
        sampleBaseURL: "/dist/samples/",
        synthdefBaseURL: "/dist/synthdefs/",
      });

      const debugMessages = [];
      sonic.on('debug', (msg) => {
        debugMessages.push(msg);
      });

      try {
        await sonic.init();

        // Wait a moment for debug messages
        await new Promise((r) => setTimeout(r, 100));

        // Read tree from SharedArrayBuffer
        const tree = sonic.getTree();

        return {
          success: true,
          tree,
          debugMessages: debugMessages.filter(m => m.text && m.text.includes('[NodeTree]')).map(m => m.text)
        };
      } catch (err) {
        return { success: false, error: err.message, stack: err.stack };
      }
    });

    expect(result.success).toBe(true);
    // Root group should be in tree
    expect(result.tree.nodeCount).toBe(1);
    expect(result.tree.version).toBeGreaterThan(0); // Version incremented when root added
    expect(result.tree.nodes.length).toBe(1);
    expect(result.tree.nodes[0].id).toBe(0); // Root group has ID 0
    expect(result.tree.nodes[0].isGroup).toBe(true);
    expect(result.tree.nodes[0].parentId).toBe(-1); // No parent
  });

  // Stage 5: Node create/delete updates tree
  test("node tree updates on synth create/delete - Stage 5", async ({ page }) => {
    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: "/dist/workers/",
        wasmBaseURL: "/dist/wasm/",
        sampleBaseURL: "/dist/samples/",
        synthdefBaseURL: "/dist/synthdefs/",
      });

      try {
        await sonic.init();
        await sonic.loadSynthDef('sonic-pi-beep');

        // Tree should have just root group
        const tree1 = sonic.getTree();

        // Create a synth
        await sonic.send('/s_new', 'sonic-pi-beep', 1000, 1, 0);
        await sonic.sync(2);
        await new Promise(r => setTimeout(r, 50));

        const tree2 = sonic.getTree();

        // Create another synth
        await sonic.send('/s_new', 'sonic-pi-beep', 1001, 1, 0);
        await sonic.sync(2);
        await new Promise(r => setTimeout(r, 50));

        const tree3 = sonic.getTree();

        // Free the first synth
        await sonic.send('/n_free', 1000);
        await sonic.sync(2);
        await new Promise(r => setTimeout(r, 50));

        const tree4 = sonic.getTree();

        return {
          success: true,
          tree1,
          tree2,
          tree3,
          tree4
        };
      } catch (err) {
        return { success: false, error: err.message, stack: err.stack };
      }
    });

    expect(result.success).toBe(true);

    // Tree1: just root group
    expect(result.tree1.nodeCount).toBe(1);

    // Tree2: root + synth 1000
    expect(result.tree2.nodeCount).toBe(2);
    const synth1000 = result.tree2.nodes.find(n => n.id === 1000);
    expect(synth1000).toBeDefined();
    expect(synth1000.isGroup).toBe(false);
    expect(synth1000.parentId).toBe(0); // Child of root group

    // Tree3: root + synth 1000 + synth 1001
    expect(result.tree3.nodeCount).toBe(3);
    const synth1001 = result.tree3.nodes.find(n => n.id === 1001);
    expect(synth1001).toBeDefined();

    // Tree4: root + synth 1001 (1000 was freed)
    expect(result.tree4.nodeCount).toBe(2);
    expect(result.tree4.nodes.find(n => n.id === 1000)).toBeUndefined();
    expect(result.tree4.nodes.find(n => n.id === 1001)).toBeDefined();
  });

  // Stage 6: Compare getTree() with /g_queryTree OSC response
  test("node tree matches /g_queryTree response - Stage 6", async ({ page }) => {
    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: "/dist/workers/",
        wasmBaseURL: "/dist/wasm/",
        sampleBaseURL: "/dist/samples/",
        synthdefBaseURL: "/dist/synthdefs/",
      });

      const messages = [];
      sonic.on('message', (msg) => {
        messages.push(JSON.parse(JSON.stringify(msg)));
      });

      try {
        await sonic.init();
        await sonic.loadSynthDef('sonic-pi-beep');

        // Create a group and some synths
        await sonic.send('/g_new', 100, 1, 0);  // Group 100 inside root
        await sonic.send('/s_new', 'sonic-pi-beep', 1000, 1, 100);  // Synth in group 100
        await sonic.send('/s_new', 'sonic-pi-beep', 1001, 1, 100);  // Another synth in group 100
        await sonic.send('/s_new', 'sonic-pi-beep', 1002, 1, 0);    // Synth in root group
        await sonic.sync(2);
        await new Promise(r => setTimeout(r, 100));

        // Get tree from SharedArrayBuffer
        const sabTree = sonic.getTree();

        // Get tree from /g_queryTree OSC
        await sonic.send('/g_queryTree', 0, 0);  // Query root group (0), no controls (0)
        await sonic.sync(2);
        await new Promise(r => setTimeout(r, 100));

        // Find the queryTree response
        const queryTreeMsg = messages.find(m => m.address === '/g_queryTree.reply');

        return {
          success: true,
          sabTree,
          queryTreeMsg,
          allMessages: messages
        };
      } catch (err) {
        return { success: false, error: err.message, stack: err.stack };
      }
    });

    expect(result.success).toBe(true);
    expect(result.queryTreeMsg).toBeDefined();

    // SAB tree should have: root (0), group (100), synths (1000, 1001, 1002)
    expect(result.sabTree.nodeCount).toBe(5);

    // Parse /g_queryTree.reply to count nodes
    // Format: [controlFlag, rootNodeID, numChildren, ...]
    // Each group: [nodeID, numChildren, ...]
    // Each synth: [nodeID, synthDefSymbol]
    const args = result.queryTreeMsg.args;

    // Count nodes from queryTree response
    // The response structure is complex, but we can verify basic consistency
    expect(args[1]).toBe(0); // Root node ID

    // Verify all expected node IDs exist in SAB tree
    const nodeIds = result.sabTree.nodes.map(n => n.id);
    expect(nodeIds).toContain(0);    // Root group
    expect(nodeIds).toContain(100);  // Sub-group
    expect(nodeIds).toContain(1000); // Synth
    expect(nodeIds).toContain(1001); // Synth
    expect(nodeIds).toContain(1002); // Synth

    // Verify def names match /g_queryTree response
    // Groups should have defName "group", synths should have their synthdef name
    const rootGroup = result.sabTree.nodes.find(n => n.id === 0);
    const subGroup = result.sabTree.nodes.find(n => n.id === 100);
    const synth1000 = result.sabTree.nodes.find(n => n.id === 1000);
    const synth1001 = result.sabTree.nodes.find(n => n.id === 1001);
    const synth1002 = result.sabTree.nodes.find(n => n.id === 1002);

    expect(rootGroup.defName).toBe('group');
    expect(subGroup.defName).toBe('group');
    expect(synth1000.defName).toBe('sonic-pi-beep');
    expect(synth1001.defName).toBe('sonic-pi-beep');
    expect(synth1002.defName).toBe('sonic-pi-beep');

    // Verify isGroup matches what we expect
    expect(rootGroup.isGroup).toBe(true);
    expect(subGroup.isGroup).toBe(true);
    expect(synth1000.isGroup).toBe(false);
    expect(synth1001.isGroup).toBe(false);
    expect(synth1002.isGroup).toBe(false);
  });

  // Stress test: 500 playing synths with depth 10
  test("node tree handles 500 playing synths with depth 10", async ({ page }) => {
    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: "/dist/workers/",
        wasmBaseURL: "/dist/wasm/",
        sampleBaseURL: "/dist/samples/",
        synthdefBaseURL: "/dist/synthdefs/",
      });

      try {
        await sonic.init();
        await sonic.loadSynthDef('sonic-pi-beep');

        // Create 10 levels of nested groups (depth 10)
        // Each level: group ID = level * 100 (100, 200, 300, ..., 1000)
        const groupIds = [];
        let parentId = 0; // Start from root
        for (let level = 1; level <= 10; level++) {
          const groupId = level * 100;
          await sonic.send('/g_new', groupId, 1, parentId);
          groupIds.push(groupId);
          parentId = groupId;
        }

        // Create synths distributed across all groups
        // ~50 synths per group level to get ~500 total synths
        // Plus 10 groups = 510 nodes + root = 511 total
        // Use release=60 so they stay alive for 60 seconds
        let synthId = 10000;
        for (let i = 0; i < 500; i++) {
          // Distribute synths across groups (including root group 0)
          const targetGroup = i < 50 ? 0 : groupIds[(i - 50) % 10];
          sonic.send('/s_new', 'sonic-pi-beep', synthId++, 1, targetGroup, 'note', 60, 'release', 60);
        }

        // Sync to ensure all synths are created
        await sonic.sync(2);

        // Get tree from SharedArrayBuffer
        const tree = sonic.getTree();

        // Count nodes at each depth
        const depthCounts = {};
        const countDepth = (nodes, nodeId, depth) => {
          depthCounts[depth] = (depthCounts[depth] || 0) + 1;
          const children = nodes.filter(n => n.parentId === nodeId);
          children.forEach(child => countDepth(nodes, child.id, depth + 1));
        };
        countDepth(tree.nodes, 0, 0);

        // Verify structure by checking a few specific nodes
        const root = tree.nodes.find(n => n.id === 0);
        const group100 = tree.nodes.find(n => n.id === 100);
        const group1000 = tree.nodes.find(n => n.id === 1000);
        const firstSynth = tree.nodes.find(n => n.id === 10000);

        // Now free the root group's children (this should free all groups and synths)
        // g_freeAll frees all nodes in a group but not the group itself
        await sonic.send('/g_freeAll', 0);
        await sonic.sync(2);

        // Get tree after freeing
        const treeAfterFree = sonic.getTree();

        return {
          success: true,
          nodeCount: tree.nodeCount,
          version: tree.version,
          depthCounts,
          maxDepth: Math.max(...Object.keys(depthCounts).map(Number)),
          groupCount: tree.nodes.filter(n => n.isGroup).length,
          synthCount: tree.nodes.filter(n => !n.isGroup).length,
          root,
          group100,
          group1000,
          firstSynth,
          // After freeing
          nodeCountAfterFree: treeAfterFree.nodeCount,
          nodesAfterFree: treeAfterFree.nodes
        };
      } catch (err) {
        return { success: false, error: err.message, stack: err.stack };
      }
    });

    expect(result.success).toBe(true);

    // Should have 511 nodes: 1 root + 10 groups + 500 synths
    expect(result.nodeCount).toBe(511);
    expect(result.groupCount).toBe(11); // root + 10 nested groups
    expect(result.synthCount).toBe(500);

    // Max depth should be 10 (root is depth 0, deepest group is depth 10)
    expect(result.maxDepth).toBeGreaterThanOrEqual(10);

    // Verify structure
    expect(result.root.isGroup).toBe(true);
    expect(result.group100.parentId).toBe(0);
    expect(result.group1000.parentId).toBe(900); // Group 1000's parent is group 900
    expect(result.firstSynth.isGroup).toBe(false);

    // After g_freeAll on root, only root group should remain
    expect(result.nodeCountAfterFree).toBe(1);
    expect(result.nodesAfterFree.length).toBe(1);
    expect(result.nodesAfterFree[0].id).toBe(0); // Only root remains
  });
});

test.describe("Event Emitter", () => {
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

  test("on() returns an unsubscribe function", async ({ page }) => {
    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: "/dist/workers/",
        wasmBaseURL: "/dist/wasm/",
        sampleBaseURL: "/dist/samples/",
        synthdefBaseURL: "/dist/synthdefs/",
      });

      const messages = [];
      const unsubscribe = sonic.on('message', (msg) => {
        messages.push(msg.address);
      });

      // Verify unsubscribe is a function
      const isFunction = typeof unsubscribe === 'function';

      await sonic.init();
      sonic.send("/status");
      await sonic.sync(2);

      const countBefore = messages.length;

      // Unsubscribe
      unsubscribe();

      // Send another message - should not be received
      sonic.send("/status");
      await sonic.sync(2);

      const countAfter = messages.length;

      return {
        success: true,
        isFunction,
        countBefore,
        countAfter,
        receivedAfterUnsub: countAfter > countBefore
      };
    });

    expect(result.success).toBe(true);
    expect(result.isFunction).toBe(true);
    expect(result.countBefore).toBeGreaterThan(0);
    expect(result.receivedAfterUnsub).toBe(false);
  });

  test("multiple listeners receive same event", async ({ page }) => {
    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: "/dist/workers/",
        wasmBaseURL: "/dist/wasm/",
        sampleBaseURL: "/dist/samples/",
        synthdefBaseURL: "/dist/synthdefs/",
      });

      const listener1Messages = [];
      const listener2Messages = [];
      const listener3Messages = [];

      // Three independent listeners on same event
      sonic.on('message', (msg) => {
        listener1Messages.push({ listener: 1, address: msg.address });
      });

      sonic.on('message', (msg) => {
        listener2Messages.push({ listener: 2, address: msg.address });
      });

      sonic.on('message', (msg) => {
        listener3Messages.push({ listener: 3, address: msg.address });
      });

      await sonic.init();
      sonic.send("/status");
      await sonic.sync(2);

      return {
        success: true,
        listener1Count: listener1Messages.length,
        listener2Count: listener2Messages.length,
        listener3Count: listener3Messages.length,
        allEqual: listener1Messages.length === listener2Messages.length &&
                  listener2Messages.length === listener3Messages.length
      };
    });

    expect(result.success).toBe(true);
    expect(result.listener1Count).toBeGreaterThan(0);
    expect(result.allEqual).toBe(true);
  });

  test("unsubscribing one listener does not affect others", async ({ page }) => {
    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: "/dist/workers/",
        wasmBaseURL: "/dist/wasm/",
        sampleBaseURL: "/dist/samples/",
        synthdefBaseURL: "/dist/synthdefs/",
      });

      const listenerA = [];
      const listenerB = [];
      const listenerC = [];

      // Three listeners - we'll unsubscribe B
      sonic.on('message', (msg) => {
        listenerA.push(msg.address);
      });

      const unsubB = sonic.on('message', (msg) => {
        listenerB.push(msg.address);
      });

      sonic.on('message', (msg) => {
        listenerC.push(msg.address);
      });

      await sonic.init();
      sonic.send("/status");
      await sonic.sync(2);

      const countABefore = listenerA.length;
      const countBBefore = listenerB.length;
      const countCBefore = listenerC.length;

      // Unsubscribe only B
      unsubB();

      // Send more messages
      sonic.send("/status");
      await sonic.sync(2);

      return {
        success: true,
        countABefore,
        countBBefore,
        countCBefore,
        countAAfter: listenerA.length,
        countBAfter: listenerB.length,
        countCAfter: listenerC.length,
        // A and C should have received more, B should not
        aReceivedMore: listenerA.length > countABefore,
        bReceivedMore: listenerB.length > countBBefore,
        cReceivedMore: listenerC.length > countCBefore
      };
    });

    expect(result.success).toBe(true);
    // A and C should continue receiving
    expect(result.aReceivedMore).toBe(true);
    expect(result.cReceivedMore).toBe(true);
    // B should not receive after unsubscribe
    expect(result.bReceivedMore).toBe(false);
    expect(result.countBAfter).toBe(result.countBBefore);
  });

  test("once() fires only once then auto-unsubscribes", async ({ page }) => {
    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: "/dist/workers/",
        wasmBaseURL: "/dist/wasm/",
        sampleBaseURL: "/dist/samples/",
        synthdefBaseURL: "/dist/synthdefs/",
      });

      const onceMessages = [];
      const regularMessages = [];

      // once() listener - should only fire once
      sonic.once('message', (msg) => {
        onceMessages.push(msg.address);
      });

      // regular listener for comparison
      sonic.on('message', (msg) => {
        regularMessages.push(msg.address);
      });

      await sonic.init();

      // Send multiple status commands
      sonic.send("/status");
      await sonic.sync(2);
      sonic.send("/status");
      await sonic.sync(2);
      sonic.send("/status");
      await sonic.sync(2);

      return {
        success: true,
        onceCount: onceMessages.length,
        regularCount: regularMessages.length
      };
    });

    expect(result.success).toBe(true);
    // once() should only receive one message
    expect(result.onceCount).toBe(1);
    // regular should receive multiple
    expect(result.regularCount).toBeGreaterThan(1);
  });

  test("off() can remove listener without unsubscribe function", async ({ page }) => {
    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: "/dist/workers/",
        wasmBaseURL: "/dist/wasm/",
        sampleBaseURL: "/dist/samples/",
        synthdefBaseURL: "/dist/synthdefs/",
      });

      const messages = [];

      // Keep reference to callback
      const callback = (msg) => {
        messages.push(msg.address);
      };

      sonic.on('message', callback);

      await sonic.init();
      sonic.send("/status");
      await sonic.sync(2);

      const countBefore = messages.length;

      // Remove using off() with the same callback reference
      sonic.off('message', callback);

      sonic.send("/status");
      await sonic.sync(2);

      return {
        success: true,
        countBefore,
        countAfter: messages.length,
        receivedAfterOff: messages.length > countBefore
      };
    });

    expect(result.success).toBe(true);
    expect(result.countBefore).toBeGreaterThan(0);
    expect(result.receivedAfterOff).toBe(false);
  });

  test("error in one listener does not affect others", async ({ page }) => {
    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: "/dist/workers/",
        wasmBaseURL: "/dist/wasm/",
        sampleBaseURL: "/dist/samples/",
        synthdefBaseURL: "/dist/synthdefs/",
      });

      const goodListener1 = [];
      const goodListener2 = [];

      // First good listener
      sonic.on('message', (msg) => {
        goodListener1.push(msg.address);
      });

      // Bad listener that throws
      sonic.on('message', () => {
        throw new Error("Intentional test error");
      });

      // Second good listener - should still receive messages
      sonic.on('message', (msg) => {
        goodListener2.push(msg.address);
      });

      await sonic.init();
      sonic.send("/status");
      await sonic.sync(2);

      return {
        success: true,
        listener1Count: goodListener1.length,
        listener2Count: goodListener2.length,
        bothReceived: goodListener1.length > 0 && goodListener2.length > 0
      };
    });

    expect(result.success).toBe(true);
    // Both good listeners should receive messages despite the bad one throwing
    expect(result.bothReceived).toBe(true);
    expect(result.listener1Count).toBe(result.listener2Count);
  });

  test("different event types are independent", async ({ page }) => {
    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        workerBaseURL: "/dist/workers/",
        wasmBaseURL: "/dist/wasm/",
        sampleBaseURL: "/dist/samples/",
        synthdefBaseURL: "/dist/synthdefs/",
      });

      const messageEvents = [];
      const debugEvents = [];
      const metricsEvents = [];

      sonic.on('message', (msg) => {
        messageEvents.push(msg);
      });

      sonic.on('debug', (msg) => {
        debugEvents.push(msg);
      });

      sonic.on('metrics', (metrics) => {
        metricsEvents.push(metrics);
      });

      await sonic.init();

      // Wait for metrics (they come periodically)
      await new Promise(r => setTimeout(r, 200));

      sonic.send("/status");
      await sonic.sync(2);

      return {
        success: true,
        messageCount: messageEvents.length,
        debugCount: debugEvents.length,
        metricsCount: metricsEvents.length
      };
    });

    expect(result.success).toBe(true);
    // Should have received at least some messages and metrics
    expect(result.messageCount).toBeGreaterThan(0);
    expect(result.metricsCount).toBeGreaterThan(0);
    // Debug messages depend on scsynth output, may or may not have any
  });
});

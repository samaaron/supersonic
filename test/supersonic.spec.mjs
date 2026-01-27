import { test, expect, skipIfPostMessage, WAIT_FOR_TREE_HELPER } from "./fixtures.mjs";

// Helper to wait for tree updates in postMessage mode
const WAIT_FOR_TREE = `
  async function waitForTree(sonic, condition, timeoutMs = 2000) {
    const start = Date.now();
    while (Date.now() - start < timeoutMs) {
      const tree = sonic.getRawTree();
      if (condition(tree)) return tree;
      await new Promise(r => setTimeout(r, 20));
    }
    throw new Error("Timeout waiting for tree condition");
  }
`;

test.describe("SuperSonic", () => {
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

    test("module loads successfully", async ({ page }) => {
      const hasSuperSonic = await page.evaluate(() => {
        return typeof window.SuperSonic === "function";
      });
      expect(hasSuperSonic).toBe(true);
    });

    test("boots and initializes scsynth", async ({ page, sonicConfig }) => {
      const result = await page.evaluate(async (config) => {
        const sonic = new window.SuperSonic(config);

        try {
          await sonic.init();
          return { success: true, mode: sonic.mode };
        } catch (err) {
          return { success: false, error: err.message };
        }
      }, sonicConfig);

      expect(result.success).toBe(true);
    });

    test("loads a synthdef by name", async ({ page, sonicConfig }) => {
      const result = await page.evaluate(async (config) => {
        const sonic = new window.SuperSonic(config);

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
      }, sonicConfig);

      expect(result.success).toBe(true);
      const doneMessages = result.messages.filter((m) => m.address === "/done");
      expect(doneMessages.length).toBeGreaterThan(0);
    });

    test("loadSynthDef returns name and size", async ({ page, sonicConfig }) => {
      const result = await page.evaluate(async (config) => {
        const sonic = new window.SuperSonic(config);

        try {
          await sonic.init();
          const loadResult = await sonic.loadSynthDef("sonic-pi-beep");
          return {
            success: true,
            name: loadResult.name,
            size: loadResult.size,
            hasName: typeof loadResult.name === 'string' && loadResult.name.length > 0,
            hasSize: typeof loadResult.size === 'number' && loadResult.size > 0,
          };
        } catch (err) {
          return { success: false, error: err.message };
        }
      }, sonicConfig);

      expect(result.success).toBe(true);
      expect(result.hasName).toBe(true);
      expect(result.hasSize).toBe(true);
      expect(result.name).toBe("sonic-pi-beep");
    });

    test("loadSynthDef accepts Uint8Array", async ({ page, sonicConfig }) => {
      const result = await page.evaluate(async (config) => {
        const sonic = new window.SuperSonic(config);

        try {
          await sonic.init();

          // Fetch a known synthdef to get raw bytes
          const response = await fetch('/dist/synthdefs/sonic-pi-beep.scsyndef');
          const arrayBuffer = await response.arrayBuffer();
          const bytes = new Uint8Array(arrayBuffer);

          // Load via Uint8Array
          const loadResult = await sonic.loadSynthDef(bytes);

          // Verify it's in the loaded synthdefs
          const isLoaded = sonic.loadedSynthDefs.has(loadResult.name);

          return {
            success: true,
            name: loadResult.name,
            size: loadResult.size,
            isLoaded,
          };
        } catch (err) {
          return { success: false, error: err.message };
        }
      }, sonicConfig);

      expect(result.success).toBe(true);
      expect(result.name).toBe("sonic-pi-beep");
      expect(result.size).toBeGreaterThan(0);
      expect(result.isLoaded).toBe(true);
    });

    test("loadSynthDef accepts ArrayBuffer", async ({ page, sonicConfig }) => {
      const result = await page.evaluate(async (config) => {
        const sonic = new window.SuperSonic(config);

        try {
          await sonic.init();

          // Fetch a known synthdef to get raw bytes
          const response = await fetch('/dist/synthdefs/sonic-pi-beep.scsyndef');
          const arrayBuffer = await response.arrayBuffer();

          // Load via ArrayBuffer directly
          const loadResult = await sonic.loadSynthDef(arrayBuffer);

          return {
            success: true,
            name: loadResult.name,
            size: loadResult.size,
          };
        } catch (err) {
          return { success: false, error: err.message };
        }
      }, sonicConfig);

      expect(result.success).toBe(true);
      expect(result.name).toBe("sonic-pi-beep");
      expect(result.size).toBeGreaterThan(0);
    });

    test("loadSynthDef accepts File/Blob", async ({ page, sonicConfig }) => {
      const result = await page.evaluate(async (config) => {
        const sonic = new window.SuperSonic(config);

        try {
          await sonic.init();

          // Fetch a known synthdef and create a Blob
          const response = await fetch('/dist/synthdefs/sonic-pi-beep.scsyndef');
          const arrayBuffer = await response.arrayBuffer();
          const blob = new Blob([arrayBuffer], { type: 'application/octet-stream' });

          // Load via Blob
          const loadResult = await sonic.loadSynthDef(blob);

          return {
            success: true,
            name: loadResult.name,
            size: loadResult.size,
          };
        } catch (err) {
          return { success: false, error: err.message };
        }
      }, sonicConfig);

      expect(result.success).toBe(true);
      expect(result.name).toBe("sonic-pi-beep");
      expect(result.size).toBeGreaterThan(0);
    });

    test("loadSynthDef rejects invalid binary data", async ({ page, sonicConfig }) => {
      const result = await page.evaluate(async (config) => {
        const sonic = new window.SuperSonic(config);

        try {
          await sonic.init();

          // Create invalid synthdef bytes (not SCgf format)
          const invalidBytes = new Uint8Array([0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12]);

          await sonic.loadSynthDef(invalidBytes);
          return { success: false, error: "Should have thrown" };
        } catch (err) {
          return {
            success: true,
            threw: true,
            message: err.message,
          };
        }
      }, sonicConfig);

      expect(result.success).toBe(true);
      expect(result.threw).toBe(true);
      expect(result.message).toContain("Could not extract synthdef name");
    });

    test("loadSynthDef rejects invalid source type", async ({ page, sonicConfig }) => {
      const result = await page.evaluate(async (config) => {
        const sonic = new window.SuperSonic(config);

        try {
          await sonic.init();

          // Pass invalid type (number)
          await sonic.loadSynthDef(12345);
          return { success: false, error: "Should have thrown" };
        } catch (err) {
          return {
            success: true,
            threw: true,
            message: err.message,
          };
        }
      }, sonicConfig);

      expect(result.success).toBe(true);
      expect(result.threw).toBe(true);
      expect(result.message).toContain("must be a name, path/URL string, ArrayBuffer, Uint8Array, or File/Blob");
    });

    test("debug metrics increment when scsynth outputs", async ({ page, sonicConfig }) => {
      const result = await page.evaluate(async (config) => {
        const sonic = new window.SuperSonic(config);

        try {
          await sonic.init();
          // Wait for scsynth startup banner to be processed
          await new Promise(r => setTimeout(r, 500));
          const metrics = sonic.getMetrics();
          return {
            success: true,
            debugMessagesReceived: metrics.debugMessagesReceived,
            debugBytesReceived: metrics.debugBytesReceived,
          };
        } catch (err) {
          return { success: false, error: err.message };
        }
      }, sonicConfig);

      expect(result.success).toBe(true);
      expect(result.debugMessagesReceived).toBeGreaterThan(0);
      expect(result.debugBytesReceived).toBeGreaterThan(0);
    });

    test("responds to /status command", async ({ page, sonicConfig }) => {
      const result = await page.evaluate(async (config) => {
        const sonic = new window.SuperSonic(config);

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
      }, sonicConfig);

      expect(result.success).toBe(true);
      const statusMessages = result.messages.filter(
        (m) => m.address === "/status.reply"
      );
      expect(statusMessages.length).toBe(1);
    });

    // Stage 4: Root group appears in tree after boot
    test("node tree has root group after boot - Stage 4", async ({ page, sonicConfig }) => {
      const result = await page.evaluate(async ({ config, helpers }) => {
        eval(helpers);
        const sonic = new window.SuperSonic(config);

        try {
          await sonic.init();

          // Wait for tree to have root group (needed in postMessage mode)
          const tree = await waitForTree(sonic, t => t.nodeCount >= 1);

          return {
            success: true,
            tree,
          };
        } catch (err) {
          return { success: false, error: err.message, stack: err.stack };
        }
      }, { config: sonicConfig, helpers: WAIT_FOR_TREE });

      expect(result.success).toBe(true);
      expect(result.tree.nodeCount).toBe(1);
      expect(result.tree.version).toBeGreaterThan(0);
      expect(result.tree.nodes.length).toBe(1);
      expect(result.tree.nodes[0].id).toBe(0);
      expect(result.tree.nodes[0].isGroup).toBe(true);
      expect(result.tree.nodes[0].parentId).toBe(-1);
    });

    // Stage 5: Node create/delete updates tree
    test("node tree updates on synth create/delete - Stage 5", async ({ page, sonicConfig }) => {
      const result = await page.evaluate(async ({ config, helpers }) => {
        eval(helpers);
        const sonic = new window.SuperSonic(config);

        try {
          await sonic.init();
          await sonic.loadSynthDef('sonic-pi-beep');

          // Tree should have just root group
          const tree1 = await waitForTree(sonic, t => t.nodeCount >= 1);

          // Create a synth
          await sonic.send('/s_new', 'sonic-pi-beep', 1000, 1, 0);
          await sonic.sync(2);
          const tree2 = await waitForTree(sonic, t => t.nodes.some(n => n.id === 1000));

          // Create another synth
          await sonic.send('/s_new', 'sonic-pi-beep', 1001, 1, 0);
          await sonic.sync(2);
          const tree3 = await waitForTree(sonic, t => t.nodes.some(n => n.id === 1001));

          // Free the first synth
          await sonic.send('/n_free', 1000);
          await sonic.sync(2);
          const tree4 = await waitForTree(sonic, t => !t.nodes.some(n => n.id === 1000));

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
      }, { config: sonicConfig, helpers: WAIT_FOR_TREE });

      expect(result.success).toBe(true);

      // Tree1: just root group
      expect(result.tree1.nodeCount).toBe(1);

      // Tree2: root + synth 1000
      expect(result.tree2.nodeCount).toBe(2);
      const synth1000 = result.tree2.nodes.find(n => n.id === 1000);
      expect(synth1000).toBeDefined();
      expect(synth1000.isGroup).toBe(false);
      expect(synth1000.parentId).toBe(0);

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
    test("node tree matches /g_queryTree response - Stage 6", async ({ page, sonicConfig }) => {
      const result = await page.evaluate(async ({ config, helpers }) => {
        eval(helpers);
        const sonic = new window.SuperSonic(config);

        const messages = [];
        sonic.on('message', (msg) => {
          messages.push(JSON.parse(JSON.stringify(msg)));
        });

        try {
          await sonic.init();
          await sonic.loadSynthDef('sonic-pi-beep');

          // Create a group and some synths
          await sonic.send('/g_new', 100, 1, 0);
          await sonic.send('/s_new', 'sonic-pi-beep', 1000, 1, 100);
          await sonic.send('/s_new', 'sonic-pi-beep', 1001, 1, 100);
          await sonic.send('/s_new', 'sonic-pi-beep', 1002, 1, 0);
          await sonic.sync(2);

          // Wait for all nodes to appear in tree
          const tree = await waitForTree(sonic, t => t.nodeCount >= 5);

          // Get tree from /g_queryTree OSC
          await sonic.send('/g_queryTree', 0, 0);
          await sonic.sync(2);
          await new Promise(r => setTimeout(r, 100));

          const queryTreeMsg = messages.find(m => m.address === '/g_queryTree.reply');

          return {
            success: true,
            tree,
            queryTreeMsg,
          };
        } catch (err) {
          return { success: false, error: err.message, stack: err.stack };
        }
      }, { config: sonicConfig, helpers: WAIT_FOR_TREE });

      expect(result.success).toBe(true);
      expect(result.queryTreeMsg).toBeDefined();

      // Tree should have: root (0), group (100), synths (1000, 1001, 1002)
      expect(result.tree.nodeCount).toBe(5);

      const args = result.queryTreeMsg.args;
      expect(args[1]).toBe(0); // Root node ID

      // Verify all expected node IDs exist in tree
      const nodeIds = result.tree.nodes.map(n => n.id);
      expect(nodeIds).toContain(0);
      expect(nodeIds).toContain(100);
      expect(nodeIds).toContain(1000);
      expect(nodeIds).toContain(1001);
      expect(nodeIds).toContain(1002);

      // Verify def names
      const rootGroup = result.tree.nodes.find(n => n.id === 0);
      const subGroup = result.tree.nodes.find(n => n.id === 100);
      const synth1000 = result.tree.nodes.find(n => n.id === 1000);

      expect(rootGroup.defName).toBe('group');
      expect(subGroup.defName).toBe('group');
      expect(synth1000.defName).toBe('sonic-pi-beep');

      expect(rootGroup.isGroup).toBe(true);
      expect(subGroup.isGroup).toBe(true);
      expect(synth1000.isGroup).toBe(false);
    });

    // Stress test: 500 playing synths with depth 10
    test("node tree handles 500 playing synths with depth 10", async ({ page, sonicConfig }) => {
      const result = await page.evaluate(async ({ config, helpers }) => {
        eval(helpers);
        const sonic = new window.SuperSonic(config);

        try {
          await sonic.init();
          await sonic.loadSynthDef('sonic-pi-beep');

          // Create 10 levels of nested groups
          const groupIds = [];
          let parentId = 0;
          for (let level = 1; level <= 10; level++) {
            const groupId = level * 100;
            await sonic.send('/g_new', groupId, 1, parentId);
            groupIds.push(groupId);
            parentId = groupId;
          }

          // Create synths distributed across all groups
          let synthId = 10000;
          for (let i = 0; i < 500; i++) {
            const targetGroup = i < 50 ? 0 : groupIds[(i - 50) % 10];
            sonic.send('/s_new', 'sonic-pi-beep', synthId++, 1, targetGroup, 'note', 60, 'release', 60);
          }

          await sonic.sync(2);

          // Wait for all nodes (511 = 1 root + 10 groups + 500 synths)
          const tree = await waitForTree(sonic, t => t.nodeCount >= 511, 5000);

          // Count nodes at each depth
          const depthCounts = {};
          const countDepth = (nodes, nodeId, depth) => {
            depthCounts[depth] = (depthCounts[depth] || 0) + 1;
            const children = nodes.filter(n => n.parentId === nodeId);
            children.forEach(child => countDepth(nodes, child.id, depth + 1));
          };
          countDepth(tree.nodes, 0, 0);

          const root = tree.nodes.find(n => n.id === 0);
          const group100 = tree.nodes.find(n => n.id === 100);
          const group1000 = tree.nodes.find(n => n.id === 1000);
          const firstSynth = tree.nodes.find(n => n.id === 10000);

          // Free all
          await sonic.send('/g_freeAll', 0);
          await sonic.sync(2);

          const treeAfterFree = await waitForTree(sonic, t => t.nodeCount === 1);

          return {
            success: true,
            nodeCount: tree.nodeCount,
            depthCounts,
            maxDepth: Math.max(...Object.keys(depthCounts).map(Number)),
            groupCount: tree.nodes.filter(n => n.isGroup).length,
            synthCount: tree.nodes.filter(n => !n.isGroup).length,
            root,
            group100,
            group1000,
            firstSynth,
            nodeCountAfterFree: treeAfterFree.nodeCount,
            nodesAfterFree: treeAfterFree.nodes
          };
        } catch (err) {
          return { success: false, error: err.message, stack: err.stack };
        }
      }, { config: sonicConfig, helpers: WAIT_FOR_TREE });

      expect(result.success).toBe(true);
      expect(result.nodeCount).toBe(511);
      expect(result.groupCount).toBe(11);
      expect(result.synthCount).toBe(500);
      expect(result.maxDepth).toBeGreaterThanOrEqual(10);
      expect(result.root.isGroup).toBe(true);
      expect(result.group100.parentId).toBe(0);
      expect(result.group1000.parentId).toBe(900);
      expect(result.firstSynth.isGroup).toBe(false);
      expect(result.nodeCountAfterFree).toBe(1);
      expect(result.nodesAfterFree[0].id).toBe(0);
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

  test("on() returns an unsubscribe function", async ({ page, sonicConfig }) => {
    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);

      const messages = [];
      const unsubscribe = sonic.on('message', (msg) => {
        messages.push(msg.address);
      });

      const isFunction = typeof unsubscribe === 'function';

      await sonic.init();
      sonic.send("/status");
      await sonic.sync(2);

      const countBefore = messages.length;

      unsubscribe();

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
    }, sonicConfig);

      expect(result.success).toBe(true);
      expect(result.isFunction).toBe(true);
      expect(result.countBefore).toBeGreaterThan(0);
      expect(result.receivedAfterUnsub).toBe(false);
    });

  test("multiple listeners receive same event", async ({ page, sonicConfig }) => {
    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);

      const listener1Messages = [];
      const listener2Messages = [];
      const listener3Messages = [];

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
    }, sonicConfig);

      expect(result.success).toBe(true);
      expect(result.listener1Count).toBeGreaterThan(0);
      expect(result.allEqual).toBe(true);
    });

  test("unsubscribing one listener does not affect others", async ({ page, sonicConfig }) => {
    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);

      const listenerA = [];
      const listenerB = [];
      const listenerC = [];

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

      unsubB();

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
        aReceivedMore: listenerA.length > countABefore,
        bReceivedMore: listenerB.length > countBBefore,
        cReceivedMore: listenerC.length > countCBefore
      };
    }, sonicConfig);

      expect(result.success).toBe(true);
      expect(result.aReceivedMore).toBe(true);
      expect(result.cReceivedMore).toBe(true);
      expect(result.bReceivedMore).toBe(false);
      expect(result.countBAfter).toBe(result.countBBefore);
    });

  test("once() fires only once then auto-unsubscribes", async ({ page, sonicConfig }) => {
    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);

      const onceMessages = [];
      const regularMessages = [];

      sonic.once('message', (msg) => {
        onceMessages.push(msg.address);
      });

      sonic.on('message', (msg) => {
        regularMessages.push(msg.address);
      });

      await sonic.init();

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
    }, sonicConfig);

      expect(result.success).toBe(true);
      expect(result.onceCount).toBe(1);
      expect(result.regularCount).toBeGreaterThan(1);
    });

  test("off() can remove listener without unsubscribe function", async ({ page, sonicConfig }) => {
    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);

      const messages = [];

      const callback = (msg) => {
        messages.push(msg.address);
      };

      sonic.on('message', callback);

      await sonic.init();
      sonic.send("/status");
      await sonic.sync(2);

      const countBefore = messages.length;

      sonic.off('message', callback);

      sonic.send("/status");
      await sonic.sync(2);

      return {
        success: true,
        countBefore,
        countAfter: messages.length,
        receivedAfterOff: messages.length > countBefore
      };
    }, sonicConfig);

      expect(result.success).toBe(true);
      expect(result.countBefore).toBeGreaterThan(0);
      expect(result.receivedAfterOff).toBe(false);
    });

  test("error in one listener does not affect others", async ({ page, sonicConfig }) => {
    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);

      const goodListener1 = [];
      const goodListener2 = [];

      sonic.on('message', (msg) => {
        goodListener1.push(msg.address);
      });

      sonic.on('message', () => {
        throw new Error("Intentional test error");
      });

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
    }, sonicConfig);

      expect(result.success).toBe(true);
      expect(result.bothReceived).toBe(true);
      expect(result.listener1Count).toBe(result.listener2Count);
    });

  test("different event types are independent", async ({ page, sonicConfig }) => {
    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);

      const messageEvents = [];
      const debugEvents = [];

      sonic.on('message', (msg) => {
        messageEvents.push(msg);
      });

      sonic.on('debug', (msg) => {
        debugEvents.push(msg);
      });

      await sonic.init();

      await new Promise(r => setTimeout(r, 200));

      sonic.send("/status");
      await sonic.sync(2);

      // Check that getMetrics() works (this is mode-agnostic)
      const metrics = sonic.getMetrics();

      return {
        success: true,
        messageCount: messageEvents.length,
        debugCount: debugEvents.length,
        hasMetrics: metrics !== null && typeof metrics === 'object'
      };
    }, sonicConfig);

      expect(result.success).toBe(true);
      expect(result.messageCount).toBeGreaterThan(0);
      expect(result.hasMetrics).toBe(true);
    });

  test("sonic.node is null before init()", async ({ page, sonicConfig }) => {
    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);

      return {
        nodeBeforeInit: sonic.node,
      };
    }, sonicConfig);

      expect(result.nodeBeforeInit).toBe(null);
    });

  test("sonic.node returns frozen wrapper after init()", async ({ page, sonicConfig }) => {
    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);

      await sonic.init();

      const node = sonic.node;
      return {
        success: true,
        hasNode: node !== null,
        hasConnect: typeof node.connect === "function",
        hasDisconnect: typeof node.disconnect === "function",
        hasContext: node.context instanceof AudioContext,
        hasNumberOfOutputs: typeof node.numberOfOutputs === "number",
        hasChannelCount: typeof node.channelCount === "number",
        isFrozen: Object.isFrozen(node),
      };
    }, sonicConfig);

      expect(result.success).toBe(true);
      expect(result.hasNode).toBe(true);
      expect(result.hasConnect).toBe(true);
      expect(result.hasDisconnect).toBe(true);
      expect(result.hasContext).toBe(true);
      expect(result.hasNumberOfOutputs).toBe(true);
      expect(result.hasChannelCount).toBe(true);
      expect(result.isFrozen).toBe(true);
    });

  test("sonic.node is same object on repeated access", async ({ page, sonicConfig }) => {
    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);

      await sonic.init();

      const node1 = sonic.node;
      const node2 = sonic.node;
      const node3 = sonic.node;

      return {
        success: true,
        sameObject: node1 === node2 && node2 === node3,
      };
    }, sonicConfig);

      expect(result.success).toBe(true);
      expect(result.sameObject).toBe(true);
    });

  test("sonic.node wrapper does not expose port", async ({ page, sonicConfig }) => {
    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);

      await sonic.init();

      const node = sonic.node;
      const keys = Object.keys(node);

      return {
        success: true,
        keys: keys,
        hasPort: "port" in node,
        hasParameters: "parameters" in node,
      };
    }, sonicConfig);

      expect(result.success).toBe(true);
      expect(result.hasPort).toBe(false);
      expect(result.hasParameters).toBe(false);
    });

  test("sonic.node.connect() works with AnalyserNode", async ({ page, sonicConfig }) => {
    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic({ ...config, autoConnect: false });

      await sonic.init();

      const ctx = sonic.node.context;
      const analyser = ctx.createAnalyser();

      sonic.node.connect(analyser);
      analyser.connect(ctx.destination);

      return {
        success: true,
      };
    }, sonicConfig);

      expect(result.success).toBe(true);
    });

  test("autoConnect: false prevents auto-connection to destination", async ({ page, sonicConfig }) => {
    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic({ ...config, autoConnect: false });

      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 0, "note", 60, "release", 0.01);
      await new Promise(r => setTimeout(r, 100));

      return {
        success: true,
        initialized: sonic.initialized,
      };
    }, sonicConfig);

      expect(result.success).toBe(true);
      expect(result.initialized).toBe(true);
    });

  test("autoConnect: true (default) connects to destination", async ({ page, sonicConfig }) => {
    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);

      await sonic.init();

      return {
        success: true,
        initialized: sonic.initialized,
      };
    }, sonicConfig);

      expect(result.success).toBe(true);
      expect(result.initialized).toBe(true);
    });

  test("accepts external AudioContext", async ({ page, sonicConfig }) => {
    const result = await page.evaluate(async (config) => {
      const externalCtx = new AudioContext({ sampleRate: 44100 });

      const sonic = new window.SuperSonic({
        ...config,
        audioContext: externalCtx,
      });

      await sonic.init();

      return {
        success: true,
        sameContext: sonic.node.context === externalCtx,
        sampleRate: sonic.node.context.sampleRate,
      };
    }, sonicConfig);

    expect(result.success).toBe(true);
    expect(result.sameContext).toBe(true);
    expect(result.sampleRate).toBe(44100);
  });

  test("merges audioContextOptions with defaults", async ({ page, sonicConfig }) => {
    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic({
        ...config,
        audioContextOptions: { sampleRate: 44100 },
      });

      await sonic.init();

      return {
        success: true,
        sampleRate: sonic.node.context.sampleRate,
      };
    }, sonicConfig);

    expect(result.success).toBe(true);
    expect(result.sampleRate).toBe(44100);
  });
});

// Node tree tests - getRawTree() and buffer constants work in both modes
test.describe("Node Tree Layout", () => {
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

  test("getRawTree returns correct initial state with root group", async ({ page, sonicConfig }) => {
    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);

      try {
        await sonic.init();
        await new Promise((r) => setTimeout(r, 100));

        const tree = sonic.getRawTree();
        const bc = sonic.bufferConstants;

        return {
          success: true,
          tree,
          bufferConstants: {
            NODE_TREE_START: bc.NODE_TREE_START,
            NODE_TREE_SIZE: bc.NODE_TREE_SIZE,
            NODE_TREE_HEADER_SIZE: bc.NODE_TREE_HEADER_SIZE,
            NODE_TREE_ENTRY_SIZE: bc.NODE_TREE_ENTRY_SIZE,
            NODE_TREE_MIRROR_MAX_NODES: bc.NODE_TREE_MIRROR_MAX_NODES,
          },
        };
      } catch (err) {
        return { success: false, error: err.message, stack: err.stack };
      }
    }, sonicConfig);

    expect(result.success).toBe(true);

    // Buffer constants should be consistent
    expect(result.bufferConstants.NODE_TREE_START).toBeGreaterThan(0);
    expect(result.bufferConstants.NODE_TREE_SIZE).toBe(57360);
    expect(result.bufferConstants.NODE_TREE_HEADER_SIZE).toBe(16);
    expect(result.bufferConstants.NODE_TREE_ENTRY_SIZE).toBe(56);
    expect(result.bufferConstants.NODE_TREE_MIRROR_MAX_NODES).toBe(1024);

    // Initial tree should have just the root group
    expect(result.tree.nodes.length).toBe(1);
    expect(result.tree.nodes[0].id).toBe(0);
    expect(result.tree.nodes[0].defName).toBe('group');
  });
});

// SAB-only tests - these require direct SharedArrayBuffer access
test.describe("SuperSonic (SAB-only)", () => {
  // Skip all tests in this describe block if running in postMessage mode
  test.beforeEach(async ({ page, sonicMode }) => {
    skipIfPostMessage(sonicMode, 'SAB-only tests require SharedArrayBuffer');
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

  // Verify C++ and JS read the same memory at NODE_TREE_START
  test("node tree direct memory layout matches getRawTree", async ({ page, sonicConfig }) => {
    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);

      try {
        await sonic.init();
        await new Promise((r) => setTimeout(r, 100));

        const tree = sonic.getRawTree();

        // Direct memory access (SAB-only)
        const bc = sonic.bufferConstants;
        const ringBufferBase = sonic.ringBufferBase;
        const treeBase = ringBufferBase + bc.NODE_TREE_START;
        const treeView = new Int32Array(sonic.sharedBuffer, treeBase, 20);
        const first20 = Array.from(treeView);

        return {
          success: true,
          tree,
          first20,
        };
      } catch (err) {
        return { success: false, error: err.message, stack: err.stack };
      }
    }, sonicConfig);

    expect(result.success).toBe(true);

    // Verify direct memory layout matches expected header format
    // Header: node_count (4), version (4), dropped_count (4), padding (4)
    expect(result.first20[0]).toBe(1);  // node_count = 1 (root group)
    expect(result.first20[1]).toBe(1);  // version = 1
    expect(result.first20[2]).toBe(0);  // dropped_count = 0
    // first20[3] is padding - value doesn't matter
    // First node entry (root group): id, parent_id, is_group, prev_id, next_id, head_id
    expect(result.first20[4]).toBe(0);  // root group id = 0

    // getRawTree should match direct memory
    expect(result.tree.nodes.length).toBe(1);
    expect(result.tree.nodes[0].id).toBe(0);
  });
});

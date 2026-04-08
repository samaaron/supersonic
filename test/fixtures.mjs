/**
 * Playwright Test Fixtures for SuperSonic
 *
 * Provides custom fixtures that automatically handle mode-based configuration.
 * Tests import from this file instead of '@playwright/test'.
 *
 * Usage:
 *   import { test, expect } from './fixtures.mjs';
 *
 *   test("my test", async ({ page, sonicConfig }) => {
 *     const result = await page.evaluate(async (config) => {
 *       const sonic = new window.SuperSonic(config);
 *       await sonic.init();
 *       // ...
 *     }, sonicConfig);
 *   });
 */

import { test as base } from '@playwright/test';

// Re-export protocol constants for the reply_oscchannel_test_worklet so spec
// files don't have to know the asset path. Same source the worklet imports.
export { REPLY_WORKLET_MSG } from './assets/reply_worklet_protocol.js';

export const test = base.extend({
  /**
   * Fixture: SuperSonic configuration based on current project mode.
   * Automatically uses 'sab' or 'postMessage' based on which project is running.
   */
  sonicConfig: async ({}, use, testInfo) => {
    const mode = testInfo.project.use.supersonicMode || 'postMessage';
    await use({
      workerBaseURL: "/dist/workers/",
      wasmBaseURL: "/dist/wasm/",
      sampleBaseURL: "/dist/samples/",
      synthdefBaseURL: "/dist/synthdefs/",
      snapshotIntervalMs: 25,  // Fast metrics updates for test reliability
      mode,
    });
  },

  /**
   * Fixture: Current test mode string ('sab' or 'postMessage').
   * Useful for conditional logic or skip decisions.
   */
  sonicMode: async ({}, use, testInfo) => {
    const mode = testInfo.project.use.supersonicMode || 'postMessage';
    await use(mode);
  },

  /**
   * Fixture: Page pre-navigated to test harness and ready for SuperSonic.
   * Includes console error logging.
   */
  sonicPage: async ({ page, sonicConfig }, use) => {
    // Log browser errors to Node console
    page.on("console", (msg) => {
      if (msg.type() === "error") {
        console.error("Browser console error:", msg.text());
      }
    });

    page.on("pageerror", (err) => {
      console.error("Page error:", err.message);
    });

    // Navigate and wait for SuperSonic to be ready
    await page.goto("/test/harness.html");
    await page.waitForFunction(() => window.supersonicReady === true, {
      timeout: 10000,
    });

    await use(page);
  },
});

// Re-export expect for convenience
export { expect } from '@playwright/test';

/**
 * Skip test if running in postMessage mode.
 * Use for tests that require SAB features (audio capture, direct memory access).
 *
 * Usage:
 *   test("SAB-only test", async ({ sonicMode }) => {
 *     skipIfPostMessage(sonicMode);
 *     // ... test code
 *   });
 */
export function skipIfPostMessage(mode, reason = 'Requires SAB mode') {
  base.skip(mode === 'postMessage', reason);
}

/**
 * Skip test if running in SAB mode.
 * Use for tests specific to postMessage mode.
 *
 * Usage:
 *   test("postMessage-only test", async ({ sonicMode }) => {
 *     skipIfSAB(sonicMode);
 *     // ... test code
 *   });
 */
export function skipIfSAB(mode, reason = 'Requires postMessage mode') {
  base.skip(mode === 'sab', reason);
}

/**
 * Helper code to wait for tree updates (for use inside page.evaluate).
 * Useful in postMessage mode where tree updates are async.
 */
export const WAIT_FOR_TREE_HELPER = `
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

/**
 * Helper code to wait for a specific OSC message with timeout (for use inside page.evaluate).
 * Prevents hanging if expected message never arrives - fails fast instead.
 */
export const WAIT_FOR_MESSAGE_HELPER = `
  function waitForMessage(sonic, address, timeoutMs = 5000) {
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        sonic.off("in", handler);
        reject(new Error("Timeout waiting for " + address + " after " + timeoutMs + "ms"));
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
`;

/**
 * Helpers for raw node tree inspection (for use inside page.evaluate).
 * Provides:
 *   - makeUuid(suffix): build a 16-byte UUID v7 with a recognizable last byte
 *   - findBySuffix(rawTree, suffix): node id for the node whose UUID ends in suffix
 *   - childrenInOrder(rawTree, groupId): linked-list children ids in walk order
 *   - findInTree(treeNode, id): depth-first lookup in a getTree() result
 */
export const NODE_TREE_HELPERS = `
  function makeUuid(suffix) {
    const b = new Uint8Array(16);
    for (let i = 0; i < 16; i++) b[i] = (i * 13 + suffix * 31) & 0xff;
    b[6] = (b[6] & 0x0f) | 0x70; // version 7
    b[8] = (b[8] & 0x3f) | 0x80; // variant
    b[15] = suffix & 0xff;       // recognizable suffix
    return b;
  }
  function findBySuffix(rawTree, suffix) {
    const node = rawTree.nodes.find((n) => n.uuid && n.uuid[15] === suffix);
    return node ? node.id : null;
  }
  function childrenInOrder(rawTree, groupId) {
    const byId = new Map();
    for (const n of rawTree.nodes) byId.set(n.id, n);
    const group = byId.get(groupId);
    if (!group || !group.isGroup) return [];
    const out = [];
    let cur = group.headId;
    let safety = 0;
    const max = byId.size + 1;
    while (cur !== -1 && cur !== 0 && safety < max) {
      out.push(cur);
      const node = byId.get(cur);
      if (!node) break;
      cur = node.nextId;
      safety++;
    }
    return out;
  }
  function findInTree(treeNode, id) {
    if (treeNode.id === id) return treeNode;
    for (const c of treeNode.children || []) {
      const found = findInTree(c, id);
      if (found) return found;
    }
    return null;
  }
`;


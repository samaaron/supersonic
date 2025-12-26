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
      const tree = sonic.getTree();
      if (condition(tree)) return tree;
      await new Promise(r => setTimeout(r, 20));
    }
    throw new Error("Timeout waiting for tree condition");
  }
`;

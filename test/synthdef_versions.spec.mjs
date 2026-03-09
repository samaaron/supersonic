/**
 * Tests for SynthDef version compatibility (v1, v2, v3).
 *
 * Uses fixtures compiled by sclang 3.15.0-dev from:
 *   test/synthdefs/compile_version_synthdefs.scd
 *
 * Tests the full path: fetch binary → loadSynthDef (JS name extraction + /d_recv)
 * → C++ GraphDef_Recv → synth creation → audio output.
 */
import { test, expect } from "./fixtures.mjs";

const VERSIONS = [1, 2, 3];
const FIXTURES = {
  simple: (v) => `/test/synthdefs/versions/test_simple_v${v}.scsyndef`,
  multi:  (v) => `/test/synthdefs/versions/test_multi_v${v}.scsyndef`,
};

// ---------------------------------------------------------------------------
// Loading via loadSynthDef (JS API → name extraction → /d_recv → C++ parser)
// ---------------------------------------------------------------------------
test.describe("loadSynthDef — version handling", () => {
  for (const version of VERSIONS) {
    test(`loads v${version} simple synthdef and extracts correct name`, async ({ page, sonicConfig }) => {
      await page.goto("/test/harness.html");
      const result = await page.evaluate(async (args) => {
        const { config, url } = args;
        const sonic = new window.SuperSonic(config);
        await sonic.init();

        const response = await fetch(url);
        const bytes = new Uint8Array(await response.arrayBuffer());
        const loadResult = await sonic.loadSynthDef(bytes);

        return {
          name: loadResult.name,
          size: loadResult.size,
          isLoaded: sonic.loadedSynthDefs.has(loadResult.name),
        };
      }, { config: sonicConfig, url: FIXTURES.simple(version) });

      expect(result.name).toBe("test_simple");
      expect(result.size).toBeGreaterThan(0);
      expect(result.isLoaded).toBe(true);
    });

    test(`loads v${version} multi-control synthdef and extracts correct name`, async ({ page, sonicConfig }) => {
      await page.goto("/test/harness.html");
      const result = await page.evaluate(async (args) => {
        const { config, url } = args;
        const sonic = new window.SuperSonic(config);
        await sonic.init();

        const response = await fetch(url);
        const bytes = new Uint8Array(await response.arrayBuffer());
        const loadResult = await sonic.loadSynthDef(bytes);

        return {
          name: loadResult.name,
          size: loadResult.size,
          isLoaded: sonic.loadedSynthDefs.has(loadResult.name),
        };
      }, { config: sonicConfig, url: FIXTURES.multi(version) });

      expect(result.name).toBe("test_multi");
      expect(result.size).toBeGreaterThan(0);
      expect(result.isLoaded).toBe(true);
    });
  }
});

// ---------------------------------------------------------------------------
// Synth creation — load synthdef, create synth, verify it appears in node tree
// ---------------------------------------------------------------------------
test.describe("synth creation — version handling", () => {
  for (const version of VERSIONS) {
    test(`creates synth from v${version} simple synthdef`, async ({ page, sonicConfig }) => {
      await page.goto("/test/harness.html");
      const result = await page.evaluate(async (args) => {
        const { config, url } = args;
        const sonic = new window.SuperSonic(config);
        await sonic.init();

        const response = await fetch(url);
        const bytes = new Uint8Array(await response.arrayBuffer());
        await sonic.loadSynthDef(bytes);

        // Create synth with node ID 1001
        await sonic.send("/s_new", "test_simple", 1001, 0, 0, "freq", 880, "amp", 0.1);
        await sonic.sync(1);

        const tree = sonic.getRawTree();
        const hasSynth = tree.nodes.some((n) => n.id === 1001);

        // Clean up
        await sonic.send("/n_free", 1001);
        await sonic.sync(2);

        return { hasSynth };
      }, { config: sonicConfig, url: FIXTURES.simple(version) });

      expect(result.hasSynth).toBe(true);
    });

    test(`creates synth from v${version} multi-control synthdef with controls`, async ({ page, sonicConfig }) => {
      await page.goto("/test/harness.html");
      const result = await page.evaluate(async (args) => {
        const { config, url } = args;
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
        await sonic.send("/notify", 1);
        await sonic.sync(1);

        const response = await fetch(url);
        const bytes = new Uint8Array(await response.arrayBuffer());
        await sonic.loadSynthDef(bytes);

        // Create synth with specific control values
        await sonic.send("/s_new", "test_multi", 1002, 0, 0,
          "freq", 660, "amp", 0.2, "pan", -0.5, "gate", 1);
        await sonic.sync(2);

        const tree = sonic.getRawTree();
        const hasSynth = tree.nodes.some((n) => n.id === 1002);

        // Release via gate=0, wait for /n_end
        const endPromise = waitForMessage(sonic, "/n_end", 3000);
        await sonic.send("/n_set", 1002, "gate", 0);
        const endMsg = await endPromise;

        return {
          hasSynth,
          freedCleanly: endMsg[0] === "/n_end",
        };
      }, { config: sonicConfig, url: FIXTURES.multi(version) });

      expect(result.hasSynth).toBe(true);
      expect(result.freedCleanly).toBe(true);
    });
  }
});

// ---------------------------------------------------------------------------
// Synthdef count — verify /status reports increased synthdef count
// ---------------------------------------------------------------------------
test.describe("synthdef count — version handling", () => {
  for (const version of VERSIONS) {
    test(`/status reflects loaded v${version} synthdef`, async ({ page, sonicConfig }) => {
      await page.goto("/test/harness.html");
      const result = await page.evaluate(async (args) => {
        const { config, url } = args;
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

        // Count before
        await sonic.send("/status");
        const statusBefore = await waitForMessage(sonic, "/status.reply", 3000);
        const defsBefore = statusBefore[5]; // synthdef count is field 5

        // Load synthdef
        const response = await fetch(url);
        const bytes = new Uint8Array(await response.arrayBuffer());
        await sonic.loadSynthDef(bytes);

        // Count after
        await sonic.send("/status");
        const statusAfter = await waitForMessage(sonic, "/status.reply", 3000);
        const defsAfter = statusAfter[5];

        return { defsBefore, defsAfter, increased: defsAfter > defsBefore };
      }, { config: sonicConfig, url: FIXTURES.simple(version) });

      expect(result.increased).toBe(true);
    });
  }
});

// ---------------------------------------------------------------------------
// Server resilience — corrupted/truncated synthdefs don't crash
// ---------------------------------------------------------------------------
test.describe("corrupted synthdef resilience", () => {
  for (const version of VERSIONS) {
    test(`truncated v${version} synthdef does not crash server`, async ({ page, sonicConfig }) => {
      await page.goto("/test/harness.html");
      const result = await page.evaluate(async (args) => {
        const { config, url } = args;
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

        const response = await fetch(url);
        const fullBytes = new Uint8Array(await response.arrayBuffer());

        // Send progressively truncated synthdefs directly via /d_recv
        // (bypasses JS-level name extraction which would reject early)
        const lengths = [fullBytes.length - 4, fullBytes.length - 20, 20, 11];
        for (const len of lengths) {
          if (len < 1) continue;
          const truncated = fullBytes.slice(0, len);
          try {
            await sonic.send("/d_recv", truncated);
            await sonic.sync();
          } catch {
            // Expected — truncated data should fail
          }
        }

        // Verify server is still responsive
        await sonic.send("/status");
        const status = await waitForMessage(sonic, "/status.reply", 3000);
        return { serverAlive: status[0] === "/status.reply" };
      }, { config: sonicConfig, url: FIXTURES.multi(version) });

      expect(result.serverAlive).toBe(true);
    });
  }
});

// @ts-check
import { test, expect } from "./fixtures.mjs";

test.describe("Configuration Validation", () => {
  // Navigate to harness page before each test so window.SuperSonic is available
  test.beforeEach(async ({ page }) => {
    await page.goto("/test/harness.html");
    await page.waitForFunction(() => window.supersonicReady === true, { timeout: 10000 });
  });

  test.describe("Invalid configurations should throw", () => {
    test("negative numBuffers", async ({ page, sonicConfig }) => {
      const result = await page.evaluate(async (config) => {
        try {
          new window.SuperSonic({
            ...config,
            scsynthOptions: { numBuffers: -1 }
          });
          return { threw: false };
        } catch (error) {
          return { threw: true, message: error.message };
        }
      }, sonicConfig);

      expect(result.threw).toBe(true);
      expect(result.message).toContain("numBuffers");
    });

    test("non-numeric numBuffers", async ({ page, sonicConfig }) => {
      const result = await page.evaluate(async (config) => {
        try {
          new window.SuperSonic({
            ...config,
            scsynthOptions: { numBuffers: "lots" }
          });
          return { threw: false };
        } catch (error) {
          return { threw: true, message: error.message };
        }
      }, sonicConfig);

      expect(result.threw).toBe(true);
      expect(result.message).toContain("numBuffers");
    });

    test("numBuffers exceeds maximum", async ({ page, sonicConfig }) => {
      const result = await page.evaluate(async (config) => {
        try {
          new window.SuperSonic({
            ...config,
            scsynthOptions: { numBuffers: 100000 }
          });
          return { threw: false };
        } catch (error) {
          return { threw: true, message: error.message };
        }
      }, sonicConfig);

      expect(result.threw).toBe(true);
      expect(result.message).toContain("numBuffers");
    });

    test("zero numBuffers", async ({ page, sonicConfig }) => {
      const result = await page.evaluate(async (config) => {
        try {
          new window.SuperSonic({
            ...config,
            scsynthOptions: { numBuffers: 0 }
          });
          return { threw: false };
        } catch (error) {
          return { threw: true, message: error.message };
        }
      }, sonicConfig);

      expect(result.threw).toBe(true);
      expect(result.message).toContain("numBuffers");
    });

    test("Infinity numBuffers", async ({ page, sonicConfig }) => {
      const result = await page.evaluate(async (config) => {
        try {
          new window.SuperSonic({
            ...config,
            scsynthOptions: { numBuffers: Infinity }
          });
          return { threw: false };
        } catch (error) {
          return { threw: true, message: error.message };
        }
      }, sonicConfig);

      expect(result.threw).toBe(true);
      expect(result.message).toContain("numBuffers");
    });

    test("NaN numBuffers", async ({ page, sonicConfig }) => {
      const result = await page.evaluate(async (config) => {
        try {
          new window.SuperSonic({
            ...config,
            scsynthOptions: { numBuffers: NaN }
          });
          return { threw: false };
        } catch (error) {
          return { threw: true, message: error.message };
        }
      }, sonicConfig);

      expect(result.threw).toBe(true);
      expect(result.message).toContain("numBuffers");
    });

    test("bufLength not 128", async ({ page, sonicConfig }) => {
      const result = await page.evaluate(async (config) => {
        try {
          new window.SuperSonic({
            ...config,
            scsynthOptions: { bufLength: 256 }
          });
          return { threw: false };
        } catch (error) {
          return { threw: true, message: error.message };
        }
      }, sonicConfig);

      expect(result.threw).toBe(true);
      expect(result.message).toContain("bufLength");
    });

    test("preferredSampleRate out of range", async ({ page, sonicConfig }) => {
      const result = await page.evaluate(async (config) => {
        try {
          new window.SuperSonic({
            ...config,
            scsynthOptions: { preferredSampleRate: 500000 }
          });
          return { threw: false };
        } catch (error) {
          return { threw: true, message: error.message };
        }
      }, sonicConfig);

      expect(result.threw).toBe(true);
      expect(result.message).toContain("preferredSampleRate");
    });

    test("realTime not boolean", async ({ page, sonicConfig }) => {
      const result = await page.evaluate(async (config) => {
        try {
          new window.SuperSonic({
            ...config,
            scsynthOptions: { realTime: "yes" }
          });
          return { threw: false };
        } catch (error) {
          return { threw: true, message: error.message };
        }
      }, sonicConfig);

      expect(result.threw).toBe(true);
      expect(result.message).toContain("realTime");
    });

    test("negative maxNodes", async ({ page, sonicConfig }) => {
      const result = await page.evaluate(async (config) => {
        try {
          new window.SuperSonic({
            ...config,
            scsynthOptions: { maxNodes: -100 }
          });
          return { threw: false };
        } catch (error) {
          return { threw: true, message: error.message };
        }
      }, sonicConfig);

      expect(result.threw).toBe(true);
      expect(result.message).toContain("maxNodes");
    });

    test("negative loadGraphDefs", async ({ page, sonicConfig }) => {
      const result = await page.evaluate(async (config) => {
        try {
          new window.SuperSonic({
            ...config,
            scsynthOptions: { loadGraphDefs: -1 }
          });
          return { threw: false };
        } catch (error) {
          return { threw: true, message: error.message };
        }
      }, sonicConfig);

      expect(result.threw).toBe(true);
      expect(result.message).toContain("loadGraphDefs");
    });

    test("loadGraphDefs exceeds maximum", async ({ page, sonicConfig }) => {
      const result = await page.evaluate(async (config) => {
        try {
          new window.SuperSonic({
            ...config,
            scsynthOptions: { loadGraphDefs: 999 }
          });
          return { threw: false };
        } catch (error) {
          return { threw: true, message: error.message };
        }
      }, sonicConfig);

      expect(result.threw).toBe(true);
      expect(result.message).toContain("loadGraphDefs");
    });
  });

  test.describe("Valid configurations should be accepted", () => {
    test("default configuration", async ({ page, sonicConfig }) => {
      const result = await page.evaluate(async (config) => {
        try {
          new window.SuperSonic(config);
          return { threw: false };
        } catch (error) {
          return { threw: true, message: error.message };
        }
      }, sonicConfig);

      expect(result.threw).toBe(false);
    });

    test("custom numBuffers, maxNodes, realTimeMemorySize", async ({ page, sonicConfig }) => {
      const result = await page.evaluate(async (config) => {
        try {
          new window.SuperSonic({
            ...config,
            scsynthOptions: {
              numBuffers: 512,
              maxNodes: 256,
              realTimeMemorySize: 8192
            }
          });
          return { threw: false };
        } catch (error) {
          return { threw: true, message: error.message };
        }
      }, sonicConfig);

      expect(result.threw).toBe(false);
    });

    test("bufLength = 128", async ({ page, sonicConfig }) => {
      const result = await page.evaluate(async (config) => {
        try {
          new window.SuperSonic({
            ...config,
            scsynthOptions: { bufLength: 128 }
          });
          return { threw: false };
        } catch (error) {
          return { threw: true, message: error.message };
        }
      }, sonicConfig);

      expect(result.threw).toBe(false);
    });

    test("preferredSampleRate = 0 (auto)", async ({ page, sonicConfig }) => {
      const result = await page.evaluate(async (config) => {
        try {
          new window.SuperSonic({
            ...config,
            scsynthOptions: { preferredSampleRate: 0 }
          });
          return { threw: false };
        } catch (error) {
          return { threw: true, message: error.message };
        }
      }, sonicConfig);

      expect(result.threw).toBe(false);
    });

    test("preferredSampleRate = 48000", async ({ page, sonicConfig }) => {
      const result = await page.evaluate(async (config) => {
        try {
          new window.SuperSonic({
            ...config,
            scsynthOptions: { preferredSampleRate: 48000 }
          });
          return { threw: false };
        } catch (error) {
          return { threw: true, message: error.message };
        }
      }, sonicConfig);

      expect(result.threw).toBe(false);
    });

    test("loadGraphDefs = 0", async ({ page, sonicConfig }) => {
      const result = await page.evaluate(async (config) => {
        try {
          new window.SuperSonic({
            ...config,
            scsynthOptions: { loadGraphDefs: 0 }
          });
          return { threw: false };
        } catch (error) {
          return { threw: true, message: error.message };
        }
      }, sonicConfig);

      expect(result.threw).toBe(false);
    });

    test("loadGraphDefs = 1", async ({ page, sonicConfig }) => {
      const result = await page.evaluate(async (config) => {
        try {
          new window.SuperSonic({
            ...config,
            scsynthOptions: { loadGraphDefs: 1 }
          });
          return { threw: false };
        } catch (error) {
          return { threw: true, message: error.message };
        }
      }, sonicConfig);

      expect(result.threw).toBe(false);
    });
  });
});

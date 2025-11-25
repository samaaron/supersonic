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
      sonic.onMessage = (msg) => {
        messages.push(JSON.parse(JSON.stringify(msg)));
      };

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
      sonic.onMessage = (msg) => {
        messages.push(JSON.parse(JSON.stringify(msg)));
      };

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
});

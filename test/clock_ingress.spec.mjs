import { test, expect } from "./fixtures.mjs";

/**
 * /clock control API on web — routed through the worklet's IN-drain
 * (consumer-side ingress) into SuperClock. This is the web half of the goal
 * "all builds have access to the clock API": the same /clock/* address space
 * the native engine handles via EngineControl now reaches SuperClock on web too.
 */
test.describe("/clock ingress (web)", () => {
  test("/clock/tempo/set updates the SuperClock tempo", async ({ page, sonicConfig }) => {
    await page.goto("/test/harness.html");
    await page.waitForFunction(() => window.supersonicReady === true, { timeout: 10000 });

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await new Promise((r) => setTimeout(r, 200));

      const before = sonic.getMetrics().clockTempoMbpm;
      await sonic.send("/clock/tempo/set", 137.5);   // 137.5 bpm (non-integer → OSC float)
      await new Promise((r) => setTimeout(r, 400));   // let the audio thread apply + publish
      const after = sonic.getMetrics().clockTempoMbpm;

      return { before, after };
    }, sonicConfig);

    // 137.5 bpm => 137500 milli-BPM, published by SuperClock each block.
    expect(result.after).toBeGreaterThan(135000);
    expect(result.after).toBeLessThan(140000);
  });

  test("/clock/tempo/get round-trips a reply via the OUT ring", async ({ page, sonicConfig }) => {
    await page.goto("/test/harness.html");
    await page.waitForFunction(() => window.supersonicReady === true, { timeout: 10000 });

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await new Promise((r) => setTimeout(r, 200));

      await sonic.send("/clock/tempo/set", 142.5);
      await new Promise((r) => setTimeout(r, 150));

      // The GET verb's reply leaves via the OUT ring and arrives as an "in" event.
      const reply = await new Promise((resolve) => {
        const timer = setTimeout(() => { sonic.off("in", handler); resolve(null); }, 2000);
        const handler = (msg) => {
          if (msg[0] === "/clock/tempo.reply") { clearTimeout(timer); sonic.off("in", handler); resolve(msg); }
        };
        sonic.on("in", handler);
        sonic.send("/clock/tempo/get");
      });

      return { reply };
    }, sonicConfig);

    expect(result.reply).not.toBeNull();
    expect(result.reply[0]).toBe("/clock/tempo.reply");
    expect(result.reply[1]).toBeCloseTo(142.5, 1);
  });
});

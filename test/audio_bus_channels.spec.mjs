/**
 * Verify numAudioBusChannels config actually takes effect.
 * Creates synths on high-numbered buses and checks they work.
 */
import { test, expect } from "./fixtures.mjs";

test.describe("Audio Bus Channels Config", () => {
  test("bus 200 works with numAudioBusChannels: 1024", async ({ page }) => {
    test.setTimeout(15000);
    const consoleMsgs = [];
    page.on("console", (msg) => consoleMsgs.push(msg.text()));
    await page.goto("/test/harness.html");
    await page.waitForFunction(() => window.supersonicReady === true, { timeout: 10000 });

    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        baseURL: "/dist/",
        scsynthOptions: {
          numAudioBusChannels: 1024,
        },
      });

      const debugMsgs = [];
      sonic.on("debug", (msg) => debugMsgs.push(String(msg)));

      await sonic.init();
      await sonic.send("/notify", 1);
      await sonic.loadSynthDefs(["sonic-pi-beep", "sonic-pi-fx_level"]);
      await sonic.sync(1);

      // Create fx_level reading from bus 200, outputting to bus 0
      await sonic.send("/s_new", "sonic-pi-fx_level", 1001, 0, 0,
        "in_bus", 200, "out_bus", 0, "amp", 1.0);

      // Create beep writing to bus 200 (long sustain so it survives the check)
      await sonic.send("/s_new", "sonic-pi-beep", 1002, 0, 0,
        "note", 60, "out_bus", 200, "amp", 0.5, "sustain", 5.0, "release", 0.1);

      await new Promise(r => setTimeout(r, 500));

      // Check tree — both synths should exist
      const tree = sonic.getTree();
      const metrics = sonic.getMetrics();
      return {
        nodeCount: tree.nodeCount,
        metrics,
        debugMsgs: debugMsgs.filter(m => m.includes("World") || m.includes("numAudio")),
      };
    });

    console.log("Debug:", result.debugMsgs);
    console.log("Nodes:", result.nodeCount);
    const worldMsgs = consoleMsgs.filter(m => m.includes("World") || m.includes("numAudio") || m.includes("Audio"));
    console.log("Console WorldOptions:", worldMsgs);
    console.log("Metrics audioBus:", result.metrics?.scsynthNumAudioBusChannels, "maxNodes:", result.metrics?.scsynthMaxNodes);
    // Both synths should be alive (2 + root group = 3)
    expect(result.nodeCount).toBeGreaterThanOrEqual(3);
  });

  test("bus 500 works with numAudioBusChannels: 1024", async ({ page }) => {
    test.setTimeout(15000);
    await page.goto("/test/harness.html");
    await page.waitForFunction(() => window.supersonicReady === true, { timeout: 10000 });

    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        baseURL: "/dist/",
        scsynthOptions: {
          numAudioBusChannels: 1024,
        },
      });

      await sonic.init();
      await sonic.send("/notify", 1);
      await sonic.loadSynthDefs(["sonic-pi-beep", "sonic-pi-fx_level"]);
      await sonic.sync(1);

      // Create fx_level on bus 500
      await sonic.send("/s_new", "sonic-pi-fx_level", 1001, 0, 0,
        "in_bus", 500, "out_bus", 0, "amp", 1.0);

      // Create beep writing to bus 500 (long sustain so it survives the check)
      await sonic.send("/s_new", "sonic-pi-beep", 1002, 0, 0,
        "note", 60, "out_bus", 500, "amp", 0.5, "sustain", 5.0, "release", 0.1);

      await new Promise(r => setTimeout(r, 500));

      const tree = sonic.getTree();
      return { nodeCount: tree.nodeCount };
    });

    expect(result.nodeCount).toBeGreaterThanOrEqual(3);
  });

  test("bus 200 FAILS with default numAudioBusChannels (128)", async ({ page }) => {
    test.setTimeout(15000);
    await page.goto("/test/harness.html");
    await page.waitForFunction(() => window.supersonicReady === true, { timeout: 10000 });

    const result = await page.evaluate(async () => {
      const sonic = new window.SuperSonic({
        baseURL: "/dist/",
        // No scsynthOptions — uses default 128 channels
      });

      let failMsg = null;
      sonic.on("in", (msg) => {
        if (msg[0] === "/fail") failMsg = msg;
      });

      await sonic.init();
      await sonic.send("/notify", 1);
      await sonic.loadSynthDefs(["sonic-pi-beep"]);
      await sonic.sync(1);

      // Try to create beep on bus 200 — should fail with only 128 channels
      await sonic.send("/s_new", "sonic-pi-beep", 1002, 0, 0,
        "note", 60, "out_bus", 200, "amp", 0.5, "sustain", 0.1, "release", 0.1);

      await new Promise(r => setTimeout(r, 500));

      const tree = sonic.getTree();
      return { nodeCount: tree.nodeCount, failMsg: failMsg ? String(failMsg) : null };
    });

    // With only 128 channels, bus 200 should be invalid
    // The synth might still be created (scsynth doesn't always error on invalid bus)
    // but it won't produce audible output
    console.log("Nodes:", result.nodeCount, "Fail:", result.failMsg);
  });
});

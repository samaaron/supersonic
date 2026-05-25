import { test, expect } from "./fixtures.mjs";

// Regression: a debug batch posted by the worklet is dispatched to
// #debugRawHandler *after* #osc has been nulled by a teardown, throwing
// "Cannot read properties of null (reading 'handleDebugRaw')". reload() runs
// #partialShutdown() (nulling #osc) and then awaits context.close() + a fresh
// worklet load before #osc/#debugRawHandler are re-set — a wide async window
// during which a debugRawBatch queued from the old worklet lands on a null
// #osc. The handler must be cleared on teardown so late batches are dropped.
//
// postMessage-only: #debugRawHandler is wired solely in the PM init path; in
// SAB mode debug raw never routes through it, so there is nothing to race.
test.describe("debug teardown race", () => {
  test("reload under debug traffic never derefs a null transport", async ({
    page,
    sonicConfig,
    sonicMode,
  }) => {
    test.skip(sonicMode === "sab", "#debugRawHandler is postMessage-only");

    const pageErrors = [];
    page.on("pageerror", (err) => pageErrors.push(err.message));

    await page.goto("/test/harness.html");
    await page.waitForFunction(() => window.supersonicReady === true, {
      timeout: 10000,
    });

    await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();

      // /dumpOSC streams every incoming message into the debug ring; the burst
      // of /status keeps the worklet posting debugRawBatch right as reload()
      // tears the transport down, so a batch lands while #osc is null.
      for (let i = 0; i < 15; i++) {
        sonic.send("/dumpOSC", 1);
        for (let j = 0; j < 20; j++) sonic.send("/status");
        await sonic.reload();
      }
      await sonic.destroy();
    }, sonicConfig);

    // Let any in-flight worklet messages drain into the instance.
    await page.waitForTimeout(200);

    const derefs = pageErrors.filter((m) => /handleDebugRaw/.test(m));
    expect(
      derefs,
      `null-transport derefs after teardown:\n${derefs.join("\n")}`,
    ).toHaveLength(0);
  });
});

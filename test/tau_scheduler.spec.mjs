/**
 * clockwork's scheduler, exercised where it actually runs.
 *
 * This file exists because the scheduler was dead in the web build for its
 * whole life and every suite passed. Its Rust crate has 33 unit tests and a
 * no-alloc test proving the fire path allocates nothing; none of that runs in
 * a browser. What was never checked was whether the queue got BUILT — and a
 * queue that failed to build refuses every add exactly as a full one does, so
 * the only symptom was a dropped count that read as ordinary backpressure.
 *
 * So these assert the two things unit tests cannot: that the queue exists in
 * the shipped artifact, and that something parked in it comes back out.
 */
import { test, expect } from "./fixtures.mjs";

const boot = async (page) => {
  await page.goto("/test/harness.html");
  await page.waitForFunction(() => window.supersonicReady === true, { timeout: 10000 });
};

test("a scheduled bundle actually fires", async ({ page, sonicConfig }) => {
  await boot(page);
  const r = await page.evaluate(async (config) => {
    const sonic = new window.SuperSonic(config);
    await sonic.init();
    await sonic.loadSynthDef("sonic-pi-beep");
    const osc = window.SuperSonic.osc;

    const before = sonic.getRawTree().nodeCount;
    // Far enough ahead that it must be QUEUED rather than dispatched inline.
    const when = (Date.now() + 120) / 1000 + osc.NTP_EPOCH_OFFSET;
    sonic.sendOSC(osc.encodeBundle(when, [["/s_new", "sonic-pi-beep", 4242, 0, 0, "amp", 0.2]]));

    // While it is parked, the queue must show it. This is the assertion that
    // fails loudly if the store was never built: depth stays 0 and the event
    // is counted as dropped.
    await new Promise((r) => setTimeout(r, 40));
    const parked = sonic.getMetrics();

    await new Promise((r) => setTimeout(r, 300));
    const after = sonic.getRawTree();
    const fired = sonic.getMetrics();
    const out = {
      before,
      afterCount: after.nodeCount,
      hasNode: after.nodes.some((n) => n.id === 4242),
      peakDepth: parked.engineSchedulerPeakDepth,
      dropped: fired.engineSchedulerDropped,
    };
    await sonic.send("/n_free", 4242);
    await sonic.shutdown();
    return out;
  }, sonicConfig);

  expect(r.hasNode, "the scheduled synth never arrived").toBe(true);
  expect(r.afterCount).toBe(r.before + 1);
  expect(r.peakDepth, "nothing was ever parked in the queue").toBeGreaterThan(0);
  expect(r.dropped, "the queue refused the event").toBe(0);
});

test("a bundle whose time has passed fires immediately", async ({ page, sonicConfig }) => {
  await boot(page);
  const r = await page.evaluate(async (config) => {
    const sonic = new window.SuperSonic(config);
    await sonic.init();
    await sonic.loadSynthDef("sonic-pi-beep");
    const osc = window.SuperSonic.osc;

    // OSC says a past timetag executes now. It must not be shed.
    const past = (Date.now() - 5000) / 1000 + osc.NTP_EPOCH_OFFSET;
    sonic.sendOSC(osc.encodeBundle(past, [["/s_new", "sonic-pi-beep", 4243, 0, 0, "amp", 0.2]]));
    await new Promise((r) => setTimeout(r, 200));
    const t = sonic.getRawTree();
    const out = { hasNode: t.nodes.some((n) => n.id === 4243),
                  dropped: sonic.getMetrics().engineSchedulerDropped };
    await sonic.send("/n_free", 4243);
    await sonic.shutdown();
    return out;
  }, sonicConfig);

  expect(r.hasNode, "a bundle five seconds late was dropped instead of played").toBe(true);
  expect(r.dropped).toBe(0);
});

test("many parked events all come back", async ({ page, sonicConfig }) => {
  /*
   * The capacity assertion this replaced read bufferConstants.slot_count — a
   * BUILD constant, not the store — so it passed happily against a scheduler
   * that had never been built. Verified by reintroducing the bug: the two
   * tests above failed and that one did not. A regression test that cannot
   * fail on the defect is not a regression test.
   *
   * This parks sixteen events and counts them back out.
   */
  await boot(page);
  const r = await page.evaluate(async (config) => {
    const sonic = new window.SuperSonic(config);
    await sonic.init();
    await sonic.loadSynthDef("sonic-pi-beep");
    const osc = window.SuperSonic.osc;
    const base = Date.now() + 150;
    for (let i = 0; i < 16; i++) {
      const when = (base + i * 4) / 1000 + osc.NTP_EPOCH_OFFSET;
      sonic.sendOSC(osc.encodeBundle(when, [
        ["/s_new", "sonic-pi-beep", 5000 + i, 0, 0, "amp", 0.05],
      ]));
    }
    await new Promise((r) => setTimeout(r, 60));
    const peak = sonic.getMetrics().engineSchedulerPeakDepth;
    await new Promise((r) => setTimeout(r, 500));
    const tree = sonic.getRawTree();
    const arrived = tree.nodes.filter((n) => n.id >= 5000 && n.id < 5016).length;
    const dropped = sonic.getMetrics().engineSchedulerDropped;
    for (let i = 0; i < 16; i++) await sonic.send("/n_free", 5000 + i);
    await sonic.shutdown();
    return { peak, arrived, dropped };
  }, sonicConfig);

  expect(r.arrived, "not every parked event fired").toBe(16);
  expect(r.peak, "the queue never held anything").toBeGreaterThan(1);
  expect(r.dropped).toBe(0);
});

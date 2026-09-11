/*
 * clockwork's heap must never reach the guest's region — at any pool size.
 *
 * These are the sizes that used to decide whether two allocators collided.
 * Below the carve threshold clockwork's heap simply overran the boundary
 * (3.08 MB, on every boot); above it, clockwork carved clockwork_heap out of the
 * region base where the guest's own AllocPool already started, and the tab
 * crashed. Both are gone: clockwork claims its own heap and refuses to boot
 * if it reaches the guest region.
 *
 * A regression here is not subtle — init throws, or audio goes non-finite.
 */
import { test, expect } from './fixtures.mjs';

for (const rtMB of [8, 32, 128]) {
  test(`boots and plays with a ${rtMB}MB RT pool`, async ({ page, sonicConfig }) => {
    await page.goto("/test/harness.html");
    await page.waitForFunction(() => window.supersonicReady === true, { timeout: 10000 });

    const out = await page.evaluate(async (cfg) => {
      const debug = [];
      const s = new window.SuperSonic({ ...cfg,
        scsynthOptions: { realTimeMemorySize: cfg.__rtKB } });
      s.on("debug", (m) => debug.push(String(m?.text ?? m)));
      try { await s.init(); } catch (e) {
        return { error: String(e), fatal: debug.filter(d => d.includes("FATAL")) };
      }
      s.send("/s_new", "default", 2000, 0, 0);
      await new Promise(r => setTimeout(r, 400));
      // getMetrics(), not `.metrics` — the latter is not a property on this
      // class, so it read as undefined and every number taken from it was 0.
      // That is why `blocks` and `nonFinite` below were asserted against
      // nothing for as long as they have been collected.
      const m = s.getMetrics() ?? {};
      return {
        error: null,
        fatal: debug.filter(d => d.includes("FATAL")),
        blocks: m.engineProcessCount ?? 0,
        nonFinite: (m.engineWasmErrors ?? 0) + (m.glitchCount ?? 0),
        // Where the guest's region begins is everything below it — the wasm
        // heap, the ring reserve, and the placement arena the engine's pool is
        // taken from. It is the only number visible from here that moves when
        // the arena does.
        guestOffset: s.guestMemory().offset,
      };
    }, { ...sonicConfig, __rtKB: rtMB * 1024 });

    // The overlap guard is the thing under test: if it ever fires, say so.
    expect(out.fatal, `clockwork heap reached the guest region at ${rtMB}MB`).toEqual([]);
    expect(out.error).toBeNull();
    expect(out.nonFinite, `engine reported errors or glitches at ${rtMB}MB`).toBe(0);

    // IT HAS TO HAVE RENDERED. This was collected and never asserted, and it
    // was read from a property that does not exist, so an engine that booted
    // and then produced nothing passed every case here.
    expect(out.blocks, `no audio blocks rendered at ${rtMB}MB`).toBeGreaterThan(0);

    // AND THE POOL HAS TO BE THE SIZE THAT WAS ASKED FOR.
    //
    // This is the assertion this file was missing. The engine sizes its pool
    // with `min(requested, what the arena has)`, so a request larger than the
    // arena did not fail — it silently became a smaller pool, and every check
    // above still passed. Measured 2026-09-01: a 128MB request was served by a
    // 32MB build-time arena and this test was green.
    //
    // The arena is sized by the host from what the guest asked for, so the
    // guest region has to start above it.
    expect(out.guestOffset,
           `arena did not grow for a ${rtMB}MB pool — it was silently clamped`)
      .toBeGreaterThan(rtMB * 1024 * 1024);
  });
}

test("a pool larger than the arena is refused, not quietly shrunk", async ({ page, sonicConfig }) => {
  await page.goto("/test/harness.html");
  await page.waitForFunction(() => window.supersonicReady === true, { timeout: 10000 });

  const out = await page.evaluate(async (cfg) => {
    const debug = [];
    // An arena pinned far below what the guest asks for. Nothing does this by
    // accident — the host sizes the arena from the request — but it is the
    // shape of every silent clamp, so it is worth being able to see.
    const s = new window.SuperSonic({ ...cfg,
      memory: { memArenaSize: 4 * 1024 * 1024 },
      scsynthOptions: { realTimeMemorySize: 128 * 1024 } });
    s.on("debug", (m) => debug.push(String(m?.text ?? m)));
    let threw = false;
    try {
      await s.init();
    } catch (e) {
      threw = true;
    }
    // The engine's own words arrive over the debug ring, drained by a worker,
    // so they are not all in hand the moment init() settles. Reading straight
    // away sees an empty list and cannot tell a silent boot from a slow one.
    await new Promise((r) => setTimeout(r, 500));

    // Did it end up running anyway? That is the outcome under test: a clamp
    // produces a working engine with a pool a fraction of the requested size.
    let blocks = 0;
    try {
      s.send("/s_new", "default", 2000, 0, 0);
      await new Promise((r) => setTimeout(r, 300));
      blocks = (s.getMetrics() ?? {}).engineProcessCount ?? 0;
    } catch { /* a refused engine has nothing to ask */ }

    return { threw, debug, blocks };
  }, sonicConfig);

  // THE INVARIANT: it must not come up and run. A pool that cannot be honoured
  // has to be refused — silently shrinking it is what this file exists to
  // catch, and a shrunk pool renders audio quite happily.
  expect(out.blocks,
         "a 128MB pool in a 4MB arena booted and rendered — it was silently shrunk")
    .toBe(0);

  // AND IT SHOULD SAY WHY, where the client can still hear it. In SAB mode the
  // debug ring is drained by a worker that survives a failed bring-up; in
  // postMessage mode the worklet owns the only heap there is and a refused
  // engine takes the channel with it, so the reason cannot reach here. Assert
  // it where it is available rather than not at all.
  const complained = out.threw
    || out.debug.some(d => /RT pool|does not fit|arena/i.test(d));
  if (out.debug.length) {
    expect(complained,
           "the engine refused the pool without saying why").toBe(true);
  }
});

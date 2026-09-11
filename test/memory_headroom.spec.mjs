// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Sam Aaron
import { test, expect } from "./fixtures.mjs";

/**
 * The WASM memory margin, measured against what clockwork actually needs.
 *
 * emscripten's malloc knows nothing about our regions: it starts after the
 * static data and grows upward until it runs into whatever we placed next,
 * which is the guest's memory at `guestMemoryOffset`. That gap is the entire
 * malloc budget, and nothing enforced it.
 *
 * THIS TEST EXISTED IN clockwork-supersonic AND THE EXTRACTION DROPPED IT. It is
 * back, and with a floor that would have done its job: it asserted a flat 4MB,
 * which the layout always cleared — while `clockwork_heap` alone asks for 8MB out of
 * that same budget. So it passed throughout the entire period when
 * clockwork's heap was ending 3.08MB INSIDE the guest's region, on every boot.
 * A margin measured without the requirement it must cover is not a guard.
 *
 * The requirement is now named. init_memory also refuses to boot on a real
 * overlap (clockwork_heap_backing_end vs the guest base) — that is the hard stop;
 * this is the early warning that arrives with numbers before it fires.
 */

// What must fit below the guest region, beyond the static data:
//   clockwork_heap's backing block (CLOCKWORK_HEAP_SIZE, web profile)   8MB
//   the wasm stack (--stack-first)                          1MB
//   everything else emscripten mallocs                     slack
// Keep this in step with CLOCKWORK_HEAP_SIZE in src/memory_profile.h.
const CLOCKWORK_HEAP_BYTES = 8 * 1024 * 1024;
const STACK_BYTES    = 1 * 1024 * 1024;
const SLACK_BYTES    = 2 * 1024 * 1024;
const MIN_MALLOC_MARGIN = CLOCKWORK_HEAP_BYTES + STACK_BYTES + SLACK_BYTES;  // 11MB

test.describe("WASM memory headroom", () => {
  test("static regions leave room for clockwork heap below the guest region", async ({ page, sonicConfig }) => {
    test.skip(sonicConfig.mode === "postMessage", "needs the SAB region map");
    await page.goto("/test/harness.html");
    await page.waitForFunction(() => window.supersonicReady === true, { timeout: 10000 });

    const m = await page.evaluate(async ({ sonicConfig }) => {
      const { MemoryLayout } = await import("/clockwork/js/memory_layout.js");
      const sonic = new window.SuperSonic(sonicConfig);
      await sonic.init();
      const c = sonic.bufferConstants;

      // Highest end across every region the module publishes, so a region
      // added later is included without this test being told about it.
      let end = 0;
      const named = [];
      for (const k of Object.keys(c)) {
        if (!k.endsWith("_START")) continue;
        const base = k.replace(/_START$/, "");
        const size = c[base + "_SIZE"] ?? c[base + "_TOTAL_SIZE"] ?? 0;
        if (c[k] + size > end) end = c[k] + size;
        named.push({ name: base, end: c[k] + size });
      }
      named.sort((a, b) => b.end - a.end);
      return {
        ringBufferBase: sonic.ringBufferBase,
        staticEnd: sonic.ringBufferBase + end,
        guestMemoryOffset: MemoryLayout.guestMemoryOffset,
        wasmHeapSize: MemoryLayout.wasmHeapSize,
        ringBufferReserved: MemoryLayout.ringBufferReserved,
        largest: named.slice(0, 3),
      };
    }, { sonicConfig });

    const margin = m.guestMemoryOffset - m.staticEnd;
    const mb = (n) => (n / 1048576).toFixed(2) + "MB";
    console.log(
      `\n  static data ends at   ${mb(m.staticEnd)} (base ${mb(m.ringBufferBase)})` +
      `\n  guest region starts   ${mb(m.guestMemoryOffset)} (wasmHeapSize ${mb(m.wasmHeapSize)} + reserved ${mb(m.ringBufferReserved)})` +
      `\n  malloc margin         ${mb(margin)}  (need ${mb(MIN_MALLOC_MARGIN)})` +
      `\n  largest regions       ${m.largest.map((r) => r.name).join(", ")}`
    );

    // Hard: the static regions must not reach the guest region at all.
    expect(m.staticEnd).toBeLessThan(m.guestMemoryOffset);

    // And they must leave room for everything clockwork allocates below it —
    // clockwork_heap above all, which is claimed with malloc and so lands here.
    expect(
      margin,
      `only ${mb(margin)} below the guest region, but clockwork needs ` +
      `${mb(MIN_MALLOC_MARGIN)} (clockwork_heap ${mb(CLOCKWORK_HEAP_BYTES)} + stack + slack). ` +
      `Raise wasmHeapSize in js/memory_layout.js — its sum with ` +
      `ringBufferReserved is what moves guestMemoryOffset.`
    ).toBeGreaterThanOrEqual(MIN_MALLOC_MARGIN);
  });
});

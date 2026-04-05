/**
 * OUT Buffer Reply Corruption Test
 *
 * Tests the scsynth → JS reply path (OUT ring buffer, 128KB) for corruption
 * at the wrap boundary when flooded with /n_end replies.
 *
 * When many synths are created and freed rapidly, scsynth sends a flood
 * of /n_go + /n_end replies. If the OUT ring buffer writer wraps at the
 * boundary incorrectly, the reader (osc_in_worker.js) sees corrupted
 * messages and /n_end replies are lost.
 *
 * Symptom: [OSCInWorker] Corrupted message at position 131064
 * Impact: Lost /n_end → synth pool entries never clear → GC VMs stuck →
 *         FX chains pile up → live_loop audio stops
 *
 * Reproduces: DJ Dave track pressed Run 7+ times rapidly in tau-cam.
 */

import { test, expect } from "./fixtures.mjs";

test.describe("OUT Buffer Reply Corruption", () => {
  test("rapid synth create/free flood: zero corrupted replies", async ({
    page,
    sonicConfig,
  }) => {
    test.setTimeout(60000);

    const corruptionErrors = [];

    page.on("console", (msg) => {
      const text = msg.text();
      if (text.includes("Corrupted message")) {
        corruptionErrors.push(text);
      }
    });

    await page.goto("/test/harness.html");
    await page.waitForFunction(() => window.supersonicReady === true, {
      timeout: 10000,
    });

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);

      const nEndIds = [];
      sonic.on("in", (msg) => {
        if (msg[0] === "/n_end") nEndIds.push(msg[1]);
      });

      await sonic.init();
      await sonic.send("/notify", 1);
      await sonic.loadSynthDefs(["sonic-pi-beep"]);
      await sonic.sync(1);

      // Strategy: create + free synths in rapid bursts to flood the
      // OUT buffer with /n_go + /n_end replies.
      //
      // OUT_BUFFER_SIZE = 131072 (128KB)
      // Each /n_end reply ≈ 60 bytes after UUID expansion
      // ~2000 replies fills the buffer once; we want 3-4 wraps.

      const BURSTS = 10;
      const SYNTHS_PER_BURST = 500;
      const BASE_ID = 70000;
      let totalCreated = 0;

      for (let burst = 0; burst < BURSTS; burst++) {
        for (let i = 0; i < SYNTHS_PER_BURST; i++) {
          const id = BASE_ID + burst * SYNTHS_PER_BURST + i;
          sonic.send(
            "/s_new", "sonic-pi-beep", id, 0, 0,
            "note", 60, "amp", 0.0,
            "sustain", 0.001, "release", 0.001
          );
          sonic.send("/n_free", id);
          totalCreated++;
        }
        // Let audio thread process the burst
        await new Promise((r) => setTimeout(r, 100));
      }

      // Wait for all replies
      await new Promise((r) => setTimeout(r, 3000));

      const metrics = sonic.getMetrics();

      return {
        totalCreated,
        nEndReceived: nEndIds.length,
        messagesDropped: metrics?.scsynthMessagesDropped || 0,
        sequenceGaps: metrics?.scsynthSequenceGaps || 0,
      };
    }, sonicConfig);

    console.log(`Synths created/freed: ${result.totalCreated}`);
    console.log(`/n_end received: ${result.nEndReceived}`);
    console.log(`Messages dropped: ${result.messagesDropped}`);
    console.log(`Sequence gaps: ${result.sequenceGaps}`);
    console.log(`Corruption errors: ${corruptionErrors.length}`);

    if (corruptionErrors.length > 0) {
      console.log("CORRUPTED REPLIES:");
      for (const e of corruptionErrors.slice(0, 10)) console.log(`  ${e}`);
    }

    // Zero corruption errors in the OUT buffer reader
    expect(
      corruptionErrors.length,
      "OUT buffer should have zero corrupted messages"
    ).toBe(0);

    // Most /n_end replies should arrive (allow margin for natural synth end vs /n_free race)
    const ratio = result.nEndReceived / result.totalCreated;
    console.log(`Reply ratio: ${(ratio * 100).toFixed(1)}%`);
    expect(
      ratio,
      `Should receive >90% of /n_end replies (got ${result.nEndReceived}/${result.totalCreated})`
    ).toBeGreaterThan(0.9);
  });
});

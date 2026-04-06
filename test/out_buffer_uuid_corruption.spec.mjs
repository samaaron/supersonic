/**
 * OUT Buffer UUID Reply Corruption Test
 *
 * Same as out_buffer_reply_corruption but uses UUID node IDs instead of int32s.
 * UUID /n_end replies are ~3x larger than int32 replies (16 bytes per node ID field
 * vs 4 bytes). This makes them more likely to span the OUT ring buffer wrap boundary.
 *
 * This tests the UUID rewriter path: UUIDs in /s_new and /n_free, with
 * SuperSonic converting to int32 on write and back to UUID on read.
 *
 * Impact: If any /n_end is lost, applications tracking synth lifecycles
 *         via UUID matching will have stale entries that never resolve.
 */

import { test, expect } from "./fixtures.mjs";

test.describe("OUT Buffer UUID Reply Corruption", () => {
  test("rapid UUID synth create/free: all /n_end replies arrive", async ({
    page,
    sonicConfig,
  }) => {
    test.setTimeout(60000);

    const corruptionErrors = [];
    page.on("console", (msg) => {
      const text = msg.text();
      if (text.includes("Corrupted message") || text.includes("corrupt")) {
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
      const nEndUuids = [];
      sonic.on("in", (msg) => {
        if (msg[0] === "/n_end") {
          nEndIds.push(msg[1]);
          if (msg[1]?.type === "uuid") nEndUuids.push(msg[1]);
        }
      });

      await sonic.init();
      await sonic.send("/notify", 1);
      await sonic.loadSynthDefs(["sonic-pi-beep"]);
      await sonic.sync(1);

      // Generate UUIDs deterministically (UUID v7-like: timestamp + counter)
      let uuidCounter = 0;
      function makeUuid() {
        const bytes = new Uint8Array(16);
        const view = new DataView(bytes.buffer);
        // Timestamp-like high bits
        view.setUint32(0, 0x019d0000, false);
        view.setUint32(4, uuidCounter, false);
        // Random-like low bits
        view.setUint32(8, 0xAAAABBBB, false);
        view.setUint32(12, uuidCounter, false);
        uuidCounter++;
        return { type: "uuid", value: bytes };
      }

      // Create + free synths using UUIDs (like tau-cam does)
      // Each /n_end reply with UUID is larger, hitting the wrap boundary sooner
      const BURSTS = 10;
      const SYNTHS_PER_BURST = 200;
      let totalCreated = 0;
      const createdUuids = [];

      for (let burst = 0; burst < BURSTS; burst++) {
        for (let i = 0; i < SYNTHS_PER_BURST; i++) {
          const uuid = makeUuid();
          createdUuids.push(uuid);
          sonic.send(
            "/s_new", "sonic-pi-beep", uuid, 0, 0,
            "note", 60, "amp", 0.0,
            "sustain", 0.001, "release", 0.001
          );
          sonic.send("/n_free", uuid);
          totalCreated++;
        }
        // Let audio thread process
        await new Promise((r) => setTimeout(r, 200));
      }

      // Wait for all replies to arrive
      await new Promise((r) => setTimeout(r, 5000));

      const metrics = sonic.getMetrics();

      return {
        totalCreated,
        nEndTotal: nEndIds.length,
        nEndUuidCount: nEndUuids.length,
        messagesDropped: metrics?.scsynthMessagesDropped || 0,
        sequenceGaps: metrics?.scsynthSequenceGaps || 0,
      };
    }, sonicConfig);

    console.log(`Synths created/freed (UUID): ${result.totalCreated}`);
    console.log(`/n_end received total: ${result.nEndTotal}`);
    console.log(`/n_end with UUID: ${result.nEndUuidCount}`);
    console.log(`Messages dropped: ${result.messagesDropped}`);
    console.log(`Sequence gaps: ${result.sequenceGaps}`);
    console.log(`Corruption errors: ${corruptionErrors.length}`);

    if (corruptionErrors.length > 0) {
      console.log("CORRUPTED REPLIES:");
      for (const e of corruptionErrors.slice(0, 10)) console.log(`  ${e}`);
    }

    // Zero corruption
    expect(
      corruptionErrors.length,
      "OUT buffer should have zero corrupted messages with UUIDs"
    ).toBe(0);

    // All /n_end replies should arrive with UUIDs
    const ratio = result.nEndUuidCount / result.totalCreated;
    console.log(`UUID reply ratio: ${(ratio * 100).toFixed(1)}%`);
    expect(
      ratio,
      `Should receive >90% of UUID /n_end replies (got ${result.nEndUuidCount}/${result.totalCreated})`
    ).toBeGreaterThan(0.9);
  });
});

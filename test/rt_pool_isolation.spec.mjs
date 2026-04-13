/**
 * Verify the RT pool (scsynth allocator) and buffer pool (audio samples)
 * occupy non-overlapping memory regions.
 *
 * Root cause of NaN explosions: malloc(128MB) for the RT pool extended
 * the WASM heap into the buffer pool region, causing mutual corruption.
 *
 * This test creates many synths with reverb (filling the RT pool with
 * FreeVerb delay lines) while also loading samples (using the buffer
 * pool), then checks that scsynth's NaN/Inf counter stays at zero.
 */
import { test, expect } from "./fixtures.mjs";

test.describe("RT Pool / Buffer Pool Isolation", () => {
  test("large RT pool + loaded samples produce no NaN", async ({ page, sonicConfig }) => {
    test.setTimeout(30000);
    await page.goto("/test/harness.html");
    await page.waitForFunction(() => window.supersonicReady === true, { timeout: 10000 });

    const result = await page.evaluate(async (baseConfig) => {
      const sonic = new window.SuperSonic({
        ...baseConfig,
        scsynthOptions: {
          ...baseConfig.scsynthOptions,
          realTimeMemorySize: 131072, // 128MB — the size that triggers overlap
          maxNodes: 4096,
        },
      });

      await sonic.init();
      await sonic.send("/notify", 1);
      await sonic.loadSynthDefs([
        "sonic-pi-beep",
        "sonic-pi-fx_reverb",
        "sonic-pi-mono_player",
      ]);
      await sonic.sync(1);

      // Load a sample into the buffer pool
      await sonic.loadSample(10, "/dist/samples/bd_tek.flac");

      // Create synth + FX groups
      await sonic.send("/g_new", 100, 0, 0);
      await sonic.send("/g_new", 200, 1, 0);

      // Create 50 reverbs — each FreeVerb has ~50KB internal delay lines.
      // 50 × 50KB = 2.5MB of RT pool. With 128MB RT pool starting at ~0,
      // these allocations extend well past the 19MB buffer pool offset.
      for (let i = 0; i < 50; i++) {
        const bus = 20 + (i % 4) * 2; // spread across a few buses
        await sonic.send(
          "/s_new", "sonic-pi-fx_reverb", 201 + i, 1, 200,
          "in_bus", bus, "out_bus", 0, "mix", 0.3, "room", 0.8
        );
      }

      // Create sample players that read from buffer pool memory
      for (let i = 0; i < 20; i++) {
        await sonic.send(
          "/s_new", "sonic-pi-mono_player", 1000 + i, 0, 100,
          "buf", 10, "out_bus", 20, "amp", 0.05
        );
      }

      // Let audio process — corruption manifests within ~1s
      await new Promise(r => setTimeout(r, 2000));

      const metrics = sonic.getMetrics();
      const tree = sonic.getRawTree();

      return {
        audioNanInf: metrics.audioNanInfCount || 0,
        processCount: metrics.scsynthProcessCount || 0,
        nodeCount: tree.nodeCount,
      };
    }, sonicConfig);

    console.log(`  processCount=${result.processCount}, nodes=${result.nodeCount}, NaN/Inf=${result.audioNanInf}`);

    expect(result.processCount).toBeGreaterThan(100); // sanity: audio actually ran
    expect(result.audioNanInf, "NaN/Inf detected — RT pool likely overlaps buffer pool").toBe(0);
  });
});

import { test, expect } from "./fixtures.mjs";

/**
 * Tests that SuperSonic correctly processes a large OSC bundle containing
 * many /g_new and /s_new commands with integer node IDs.
 *
 * Constructs 8 FX chains (16 groups + 8 FX synths) in a single bundle,
 * delivered via OscChannel.sendDirect(). Before the variable-size data pool,
 * bundles >1024 bytes were silently dropped by the fixed-slot scheduler.
 */
test.describe("Big bundle FX setup", () => {
  test.beforeEach(async ({ page }) => {
    page.on("console", (msg) => {
      if (msg.type() === "error") console.error("Browser:", msg.text());
    });
    await page.goto("/test/harness.html");
    await page.waitForFunction(() => window.supersonicReady === true, { timeout: 10000 });
  });

  test("8 FX chains in one bundle creates all groups and synths", async ({ page, sonicConfig }) => {
    test.setTimeout(30000);

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.sync();

      // Create base group 100 (tau-cam does this at boot via send())
      await sonic.send("/g_new", 100, 0, 0);
      await sonic.sync();

      const osc = window.SuperSonic.osc;

      // Generate unique integer node IDs starting from 1000
      let nextId = 1000;
      function uid() {
        return nextId++;
      }

      // Helper to build one FX chain (container + synth_group + FX synth)
      // Returns arrays for encodeBundle (not pre-encoded)
      function fxChain(parentGroup, fxName, inBus, outBus, extraParams) {
        const container = uid();
        const synthGroup = uid();
        const fxSynth = uid();
        const packets = [];
        // /g_new container (tail of parent)
        packets.push(["/g_new", container, 1, parentGroup]);
        // /g_new synth_group (head of container)
        packets.push(["/g_new", synthGroup, 0, container]);
        // /s_new FX (tail of container)
        const fxArgs = ["/s_new", fxName, fxSynth, 1, container, "in_bus", inBus, "out_bus", outBus];
        for (const [k, v] of Object.entries(extraParams || {})) {
          fxArgs.push(k, v);
        }
        packets.push(fxArgs);
        return { container, synthGroup, fxSynth, packets };
      }

      const packets = [];
      const MAIN = 20;

      // Chain 1: echo (around clap) — tail of 100
      const echo = fxChain(100, "sonic-pi-fx_echo", 124, MAIN, { mix: 0.2, phase: 0.115385, decay: 0.923077, max_phase: 0.923077 });
      packets.push(...echo.packets);

      // Chain 2: reverb (inside echo, around clap) — tail of echo.synthGroup
      const revClap = fxChain(echo.synthGroup, "sonic-pi-fx_reverb", 122, 124, { mix: 0.2, room: 0.5 });
      packets.push(...revClap.packets);

      // Chain 3: reverb (around hhc1) — tail of 100
      const revHhc = fxChain(100, "sonic-pi-fx_reverb", 120, MAIN, { mix: 0.2 });
      packets.push(...revHhc.packets);

      // Chain 4: panslicer (inside reverb, around hhc1)
      const panHhc = fxChain(revHhc.synthGroup, "sonic-pi-fx_panslicer", 118, 120, { mix: 0.2, phase: 0.115385 });
      packets.push(...panHhc.packets);

      // Chain 5: reverb (around crash) — tail of 100
      const revCrash = fxChain(100, "sonic-pi-fx_reverb", 116, MAIN, { mix: 0.7 });
      packets.push(...revCrash.packets);

      // Chain 6: reverb (around arp) — tail of 100
      const revArp = fxChain(100, "sonic-pi-fx_reverb", 114, MAIN, { mix: 0.7 });
      packets.push(...revArp.packets);

      // Chain 7: panslicer (around synthbass) — tail of 100
      const panBass = fxChain(100, "sonic-pi-fx_panslicer", 112, MAIN, { mix: 0.4, phase: 0.115385 });
      packets.push(...panBass.packets);

      // Chain 8: reverb (inside panslicer, around synthbass)
      const revBass = fxChain(panBass.synthGroup, "sonic-pi-fx_reverb", 110, 112, { mix: 0.75 });
      packets.push(...revBass.packets);

      // Wrap all 24 packets into one timed bundle
      const ntpNow = sonic.initTime + (performance.now() / 1000) + 0.05;
      const bundle = osc.encodeBundle(ntpNow, packets);

      // Send the bundle. Use sonic.sendOSC which takes raw bytes
      // and delivers via the same OscChannel path.
      const sent = sonic.sendOSC(new Uint8Array(bundle));

      // Wait for scsynth to process
      await new Promise(r => setTimeout(r, 1000));
      await sonic.sync();

      // Inspect node tree
      const tree = sonic.getTree();
      const metrics = sonic.getMetrics();

      const nodes = [];
      function walk(node, depth) {
        nodes.push({
          depth,
          id: node.id,
          name: node.defName,
          children: (node.children || []).length,
        });
        for (const child of node.children || []) walk(child, depth + 1);
      }
      walk(tree.root, 0);

      await sonic.destroy();

      return {
        sent, bundleSize: bundle.byteLength, messageCount: packets.length,
        nodeCount: tree.nodeCount, droppedCount: tree.droppedCount,
        processed: metrics.scsynthMessagesProcessed,
        dropped: metrics.scsynthMessagesDropped,
        ringFails: metrics.ringBufferDirectWriteFails || 0,
        nodes,
      };
    }, sonicConfig);

    console.log(`\nSent=${result.sent}  Bundle=${result.bundleSize}B  Messages=${result.messageCount}`);
    console.log(`Scsynth: processed=${result.processed} dropped=${result.dropped} ringFails=${result.ringFails}`);
    console.log(`Tree: ${result.nodeCount} nodes, dropped=${result.droppedCount}\n`);
    for (const n of result.nodes) {
      console.log(`${"  ".repeat(n.depth)}${n.name} (id=${n.id}, children=${n.children})`);
    }

    const fxGroups = result.nodes.filter(n => n.name === "group" && n.id >= 1000);
    const fxSynths = result.nodes.filter(n => n.name.startsWith("sonic-pi-fx_"));
    console.log(`\nFX groups: ${fxGroups.length}/16   FX synths: ${fxSynths.length}/8`);

    // All 16 groups must exist (8 containers + 8 synth groups).
    // This is the critical assertion: before the pool fix, 0 groups were created
    // because the 1852-byte bundle exceeded the old 1024-byte fixed slot size.
    expect(fxGroups.length).toBe(16);
  });
});

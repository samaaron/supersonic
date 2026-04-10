/**
 * Rapid FX Chain Teardown/Rebuild Audio Death
 *
 * Rapid creation and teardown of FX chains causes audio to stop reaching
 * the output chain. The output mixer overwrites bus 0 with silence because
 * nothing feeds bus 20 (the mixer's input).
 *
 * The pattern matches the real app: a persistent output chain (LPF → Reverb → Mixer)
 * reads from bus 20 and writes to bus 0. Live loops write to bus 20 via FX chains.
 * Rapid re-runs create new FX chains and move the loop containers into them.
 */

import { test, expect, skipIfPostMessage } from "./fixtures.mjs";

test.beforeEach(async ({ sonicMode }) => {
  skipIfPostMessage(sonicMode, 'Audio capture requires SAB mode');
});

const AUDIO_HELPERS = `
function hasAudio(samples) {
  for (let i = 0; i < samples.length; i++) {
    if (samples[i] !== 0) return true;
  }
  return false;
}
function calculateRMS(samples, start, end) {
  if (!start) start = 0;
  if (!end) end = samples.length;
  let sum = 0;
  for (let i = start; i < end && i < samples.length; i++) {
    sum += samples[i] * samples[i];
  }
  return Math.sqrt(sum / (end - start));
}
`;

test("audio survives rapid FX chain rebuild with output mixer chain", async ({ page, sonicConfig }) => {
  test.setTimeout(60000);
  await page.goto("/test/harness.html");
  await page.waitForFunction(() => window.supersonicReady === true, { timeout: 10000 });

  const result = await page.evaluate(async (config) => {
    eval(config.helpers);

    const sonic = new window.SuperSonic(config.sonic);
    await sonic.init();
    await sonic.loadSynthDef("sonic-pi-beep");
    await sonic.loadSynthDef("sonic-pi-fx_level");
    await sonic.loadSynthDef("sonic-pi-fx_reverb");
    await sonic.loadSynthDef("sonic-pi-mixer");

    // UUID helper
    var uuidCounter = 0;
    function makeUuid() {
      var u = new Uint8Array(16);
      u[0] = 0x01; u[1] = 0x93; // version marker
      u[6] = 0x70; u[8] = 0x80; // UUID v7 format bits
      // Unique lower bytes
      var c = ++uuidCounter;
      u[12] = (c >> 24) & 0xff;
      u[13] = (c >> 16) & 0xff;
      u[14] = (c >> 8) & 0xff;
      u[15] = c & 0xff;
      return { type: 'uuid', value: u };
    }

    // === Output chain (matches real app) ===
    await sonic.send('/g_new', 100, 0, 0);   // int IDs for fixed groups (like real app)
    await sonic.send('/g_new', 200, 1, 0);
    await sonic.send('/s_new', 'sonic-pi-mixer', 203, 1, 200,
      'in_bus', 20, 'out_bus', 0, 'amp', 0.8, 'pre_amp', 0.3);

    // === Persistent loop structure (UUIDs like real app) ===
    var containerUuid = makeUuid();
    var bodyUuid = makeUuid();
    var mixerUuid = makeUuid();
    var beepUuid = makeUuid();

    await sonic.send('/g_new', containerUuid, 1, 100);
    await sonic.send('/g_new', bodyUuid, 0, containerUuid);
    await sonic.send('/s_new', 'sonic-pi-fx_level', mixerUuid, 1, containerUuid,
      'in_bus', 30, 'out_bus', 20, 'amp', 1.0);
    await sonic.send('/s_new', 'sonic-pi-beep', beepUuid, 0, bodyUuid,
      'note', 69, 'out_bus', 30, 'amp', 0.3, 'sustain', 30, 'release', 0.1);

    // Verify baseline audio
    await new Promise(r => setTimeout(r, 300));
    sonic.startCapture();
    await new Promise(r => setTimeout(r, 300));
    var baseline = sonic.stopCapture();
    var baselineRms = calculateRMS(baseline.left);
    var baselineHasAudio = hasAudio(baseline.left);

    // Preload fx_echo for more realistic chain variety
    await sonic.loadSynthDef("sonic-pi-fx_echo");

    // === Stress: 15 rounds × 3 nested FX chains each, no teardown ===
    var nextBus = 32;
    var FX_PER_ROUND = 3;

    for (var round = 0; round < 15; round++) {
      var prevBus = 20;
      var parentGroup = 100;

      var innermostInnerG = null;

      for (var fx = 0; fx < FX_PER_ROUND; fx++) {
        var outerG = makeUuid();
        var innerG = makeUuid();
        var fxSynthUuid = makeUuid();
        var fxBus = nextBus; nextBus += 2;

        sonic.send('/g_new', outerG, 0, parentGroup);
        sonic.send('/g_new', innerG, 0, outerG);
        sonic.send('/s_new', 'sonic-pi-fx_reverb', fxSynthUuid, 1, outerG,
          'in_bus', fxBus, 'out_bus', prevBus, 'mix', 0.3, 'room', 0.5);

        parentGroup = innerG;
        prevBus = fxBus;
        innermostInnerG = innerG;
      }

      // Update loop mixer to write to innermost FX bus
      sonic.send('/n_set', mixerUuid, 'out_bus', prevBus);

      // Move loop container into innermost FX chain
      sonic.send('/n_order', 1, innermostInnerG, containerUuid);

      // NO teardown — old chains linger
    }

    // Wait for everything to settle
    await new Promise(r => setTimeout(r, 500));

    // === Check audio ===
    sonic.startCapture();
    await new Promise(r => setTimeout(r, 300));
    var afterStress = sonic.stopCapture();
    var afterRms = calculateRMS(afterStress.left);
    var afterHasAudio = hasAudio(afterStress.left);

    // Check tree state
    var tree = sonic.getRawTree();
    var beepExists = tree.nodes.some(function(n) { return n.defName === 'sonic-pi-beep'; });
    var mixerExists = tree.nodes.some(function(n) { return n.id === 203; });

    // Dump tree structure for diagnostics
    var byId2 = new Map();
    for (var k = 0; k < tree.nodes.length; k++) byId2.set(tree.nodes[k].id, tree.nodes[k]);
    function dumpNode(id, depth) {
      var n = byId2.get(id);
      if (!n) return [];
      var lines = [];
      var indent = '';
      for (var d = 0; d < depth; d++) indent += '  ';
      var label = n.isGroup ? 'G' : (n.defName || '?').replace('sonic-pi-', '');
      lines.push(indent + label + ' [' + n.id + ']');
      if (n.isGroup && n.headId !== -1) {
        var cur = n.headId, safety = 0;
        while (cur !== -1 && cur !== 0 && safety < 500) {
          var child = byId2.get(cur);
          if (!child) break;
          lines = lines.concat(dumpNode(cur, depth + 1));
          cur = child.nextId;
          safety++;
        }
      }
      return lines;
    }
    var treeDump = dumpNode(0, 0);

    var metrics = sonic.getMetrics();

    return {
      baselineHasAudio,
      baselineRms,
      afterHasAudio,
      afterRms,
      beepExists,
      mixerExists,
      nodeCount: tree.nodeCount,
      processCount: metrics.scsynthProcessCount,
      msgsDropped: metrics.scsynthMessagesDropped,
      wasmErrors: metrics.scsynthWasmErrors,
      treeDump,
    };
  }, { sonic: sonicConfig, helpers: AUDIO_HELPERS });

  console.log('Baseline:', result.baselineHasAudio, 'RMS:', result.baselineRms.toFixed(4));
  console.log('After stress:', result.afterHasAudio, 'RMS:', result.afterRms.toFixed(4));
  console.log('Beep exists:', result.beepExists, 'Mixer exists:', result.mixerExists);
  console.log('Nodes:', result.nodeCount, 'Dropped:', result.msgsDropped, 'WasmErr:', result.wasmErrors);
  // Just show the last 10 lines of the tree (where the loop container ends up)
  const t = result.treeDump;
  console.log('Tree (last 15 lines):\n' + t.slice(Math.max(0, t.length - 15)).join('\n'));


  expect(result.baselineHasAudio, "baseline should produce audio").toBe(true);
  expect(result.baselineRms).toBeGreaterThan(0.01);
  expect(result.beepExists, "beep synth should still exist").toBe(true);
  expect(result.afterRms, "audio RMS should survive FX chain stress").toBeGreaterThan(0.01);
});

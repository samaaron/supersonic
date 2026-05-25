/**
 * Cross-layer parity: JS osc_fast encoder → SAB/postMessage transport
 * → WASM scsynth → C++ Graph_Ctor handling of OSC array tags ([ / ]).
 *
 * Confirms that bytes produced by osc_fast for a nested-array argument
 * are correctly applied as n_setn-style sequential controls by the
 * upstream-derived C++ decoder. This is the test layer that would have
 * caught the original "encoder throws on Array argument" bug: JS-side
 * unit tests round-trip encode↔decode in JS only and never cross the
 * boundary.
 *
 * SynthDef fixture: test/synthdefs/array_control_probe.scsyndef
 *   - declares freqs = #[110, 220, 330, 440] (4-slot arrayed control)
 *   - pipes each slot to a separate audio bus (verification path)
 *
 * Compile via:
 *   /Applications/SuperCollider.app/Contents/MacOS/sclang \
 *       test/synthdefs/compile_array_control_synthdefs.scd
 */
import { test, expect } from './fixtures.mjs';

test.describe('OSC array-tag parity (JS encode → scsynth decode)', () => {
  test('/s_new with array argument applies n_setn across consecutive slots',
       async ({ page, sonicConfig }) => {
    await page.goto('/test/harness.html');

    const result = await page.evaluate(async (cfg) => {
      const sonic = new window.SuperSonic(cfg);

      const messages = [];
      sonic.on('in', (m) => messages.push(m));

      await sonic.init();

      const r = await fetch('/test/synthdefs/array_control_probe.scsyndef');
      await sonic.loadSynthDef(new Uint8Array(await r.arrayBuffer()));

      // The array argument here is what exercises osc_fast's new
      // '[' / ']' tag emission. With the old encoder this would throw
      // "Unknown OSC argument type at index N: object".
      await sonic.send('/s_new', 'array_control_probe', 1001, 0, 0,
                       'freqs', [770, 660, 550, 440]);
      await sonic.sync(1);

      messages.length = 0;
      await sonic.send('/s_getn', 1001, 'freqs', 4);
      await sonic.sync(2);

      await sonic.send('/n_free', 1001);

      const reply = messages.find((m) => m[0] === '/n_setn');
      return {
        replyFound: !!reply,
        nodeId:  reply?.[1],
        ctlName: reply?.[2],
        count:   reply?.[3],
        slots: reply ? reply.slice(4) : null,
      };
    }, sonicConfig);

    expect(result.replyFound).toBe(true);
    expect(result.nodeId).toBe(1001);
    expect(result.ctlName).toBe('freqs');
    expect(result.count).toBe(4);
    expect(result.slots[0]).toBeCloseTo(770, 1);
    expect(result.slots[1]).toBeCloseTo(660, 1);
    expect(result.slots[2]).toBeCloseTo(550, 1);
    expect(result.slots[3]).toBeCloseTo(440, 1);
  });

  test('/n_set with array argument also applies sequentially',
       async ({ page, sonicConfig }) => {
    await page.goto('/test/harness.html');

    const result = await page.evaluate(async (cfg) => {
      const sonic = new window.SuperSonic(cfg);

      const messages = [];
      sonic.on('in', (m) => messages.push(m));

      await sonic.init();

      const r = await fetch('/test/synthdefs/array_control_probe.scsyndef');
      await sonic.loadSynthDef(new Uint8Array(await r.arrayBuffer()));

      // Start with the SynthDef defaults.
      await sonic.send('/s_new', 'array_control_probe', 1002, 0, 0);
      await sonic.sync(1);

      // Update via /n_set with an array — exercises the n_set Graph_Set
      // path in SC_MiscCmds.cpp.
      await sonic.send('/n_set', 1002, 'freqs', [123, 234, 345, 456]);
      await sonic.sync(2);

      messages.length = 0;
      await sonic.send('/s_getn', 1002, 'freqs', 4);
      await sonic.sync(3);

      await sonic.send('/n_free', 1002);

      const reply = messages.find((m) => m[0] === '/n_setn');
      return { slots: reply ? reply.slice(4) : null };
    }, sonicConfig);

    expect(result.slots).not.toBeNull();
    expect(result.slots[0]).toBeCloseTo(123, 1);
    expect(result.slots[1]).toBeCloseTo(234, 1);
    expect(result.slots[2]).toBeCloseTo(345, 1);
    expect(result.slots[3]).toBeCloseTo(456, 1);
  });
});

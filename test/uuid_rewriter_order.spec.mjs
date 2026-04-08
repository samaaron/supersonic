// Verify that node-creating messages within a single OSC bundle land in the
// node tree in the order they appear in the bundle, even when mixing head/tail
// add_actions. The with_fx codegen pattern is the motivating case.

import { test, expect, NODE_TREE_HELPERS } from "./fixtures.mjs";

test.describe("UUID rewriter bundle order", () => {
  test("with_fx pattern: bundle preserves [g_new, g_new head, s_new tail] order", async ({
    page,
    sonicConfig,
  }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async ({ config, helpers }) => {
      eval(helpers);
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-fx_echo");

      const osc = window.SuperSonic.osc;

      const containerBytes = makeUuid(0xc0);
      const synthGroupBytes = makeUuid(0xa0);
      const fxSynthBytes = makeUuid(0xf0);

      // Send all three messages in ONE bundle (immediate timetag = [0, 1])
      const bundle = osc.encodeBundle([0, 1], [
        ["/g_new", { type: "uuid", value: containerBytes }, 1, 0],          // tail of root
        ["/g_new", { type: "uuid", value: synthGroupBytes }, 0,             // head of container
                   { type: "uuid", value: containerBytes }],
        ["/s_new", "sonic-pi-fx_echo", { type: "uuid", value: fxSynthBytes },
                   1,                                                        // tail of container
                   { type: "uuid", value: containerBytes },
                   "in_bus", 100, "out_bus", 0, "mix", 0.0],
      ]);
      sonic.sendOSC(bundle);
      await sonic.sync(1);

      const tree = sonic.getRawTree();
      const containerId = findBySuffix(tree, 0xc0);
      const synthGroupId = findBySuffix(tree, 0xa0);
      const fxSynthId = findBySuffix(tree, 0xf0);
      const containerChildren = containerId ? childrenInOrder(tree, containerId) : null;

      try {
        await sonic.send("/g_freeAll", { type: "uuid", value: containerBytes });
        await sonic.send("/n_free", { type: "uuid", value: containerBytes });
      } catch (_) {}

      return { containerId, synthGroupId, fxSynthId, containerChildren };
    }, { config: sonicConfig, helpers: NODE_TREE_HELPERS });

    expect(result.containerId, "container created").not.toBeNull();
    expect(result.synthGroupId, "synth_group created").not.toBeNull();
    expect(result.fxSynthId, "fx_synth created").not.toBeNull();
    expect(
      result.containerChildren,
      "container children must be [synth_group, fx_synth] in order",
    ).toEqual([result.synthGroupId, result.fxSynthId]);
  });

  test("hot-swap rebase: /n_order bundled with new with_fx does not invert ordering", async ({
    page,
    sonicConfig,
  }) => {
    // Mirrors what tau-state's SpawnNamed-match emits during a re-run:
    //  - new with_fx container created (g_new + g_new head + s_new tail)
    //  - /n_order moves an existing loop_container into the new synth_group at TAIL
    // The ordering of the with_fx container itself must NOT be touched by /n_order.
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async ({ config, helpers }) => {
      eval(helpers);
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-fx_echo");

      const osc = window.SuperSonic.osc;

      // Step 1: pre-create a "loop_container" at root (simulates an existing live_loop)
      const loopContainerBytes = makeUuid(0xab);
      await sonic.send("/g_new", { type: "uuid", value: loopContainerBytes }, 1, 0);
      await sonic.sync(1);

      // Step 2: in ONE bundle: build new with_fx container + /n_order the
      // loop_container into the new synth_group at TAIL.
      const containerBytes = makeUuid(0xcc);
      const synthGroupBytes = makeUuid(0xaa);
      const fxSynthBytes = makeUuid(0xff);

      const bundle = osc.encodeBundle([0, 1], [
        ["/g_new", { type: "uuid", value: containerBytes }, 1, 0],
        ["/g_new", { type: "uuid", value: synthGroupBytes }, 0,
                   { type: "uuid", value: containerBytes }],
        ["/s_new", "sonic-pi-fx_echo", { type: "uuid", value: fxSynthBytes }, 1,
                   { type: "uuid", value: containerBytes },
                   "in_bus", 100, "out_bus", 0, "mix", 0.0],
        ["/n_order", 1,
                     { type: "uuid", value: synthGroupBytes },
                     { type: "uuid", value: loopContainerBytes }],
      ]);
      sonic.sendOSC(bundle);
      await sonic.sync(2);

      const tree = sonic.getRawTree();

      const containerId = findBySuffix(tree, 0xcc);
      const synthGroupId = findBySuffix(tree, 0xaa);
      const fxSynthId = findBySuffix(tree, 0xff);
      const loopContainerId = findBySuffix(tree, 0xab);

      const containerChildren = containerId ? childrenInOrder(tree, containerId) : null;
      const synthGroupChildren = synthGroupId ? childrenInOrder(tree, synthGroupId) : null;

      try {
        await sonic.send("/g_freeAll", { type: "uuid", value: containerBytes });
        await sonic.send("/n_free", { type: "uuid", value: containerBytes });
        await sonic.send("/n_free", { type: "uuid", value: loopContainerBytes });
      } catch (_) {}

      return {
        containerId, synthGroupId, fxSynthId, loopContainerId,
        containerChildren, synthGroupChildren,
      };
    }, { config: sonicConfig, helpers: NODE_TREE_HELPERS });

    expect(result.containerId).not.toBeNull();
    expect(result.synthGroupId).not.toBeNull();
    expect(result.fxSynthId).not.toBeNull();
    expect(result.loopContainerId).not.toBeNull();

    // Critical assertion: the with_fx container's children are [synth_group, fx_synth]
    // even after the /n_order moved the loop_container into the synth_group.
    expect(
      result.containerChildren,
      "with_fx container children = [synth_group, fx_synth]",
    ).toEqual([result.synthGroupId, result.fxSynthId]);

    // The synth_group should now contain the moved loop_container
    expect(
      result.synthGroupChildren,
      "synth_group now holds the moved loop_container",
    ).toEqual([result.loopContainerId]);
  });

  test("/n_order then /g_freeAll in same bundle: moved node survives the freeAll", async ({
    page,
    sonicConfig,
  }) => {
    // The hot-swap rebase pattern: /n_order moves loop_container OUT of OLD
    // with_fx synth_group, then /g_freeAll OLD with_fx container. The
    // loop_container MUST NOT be freed because it's no longer a child after step 1.
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async ({ config, helpers }) => {
      eval(helpers);
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-fx_echo");

      const osc = window.SuperSonic.osc;

      // OLD structure: oldC > oldSG > loopC, oldC > oldFX
      // NEW move target: newSG (separate)
      const oldC = makeUuid(0xc1);
      const oldSG = makeUuid(0xa1);
      const oldFX = makeUuid(0xf1);
      const loopC = makeUuid(0xab);
      const newSG = makeUuid(0xa2);

      // Initial setup — separate from the bundle under test
      await sonic.send("/g_new", { type: "uuid", value: oldC }, 1, 0);
      await sonic.send("/g_new", { type: "uuid", value: oldSG }, 0, { type: "uuid", value: oldC });
      await sonic.send("/s_new", "sonic-pi-fx_echo", { type: "uuid", value: oldFX }, 1,
                       { type: "uuid", value: oldC },
                       "in_bus", 100, "out_bus", 0, "mix", 0.0);
      await sonic.send("/g_new", { type: "uuid", value: loopC }, 1, { type: "uuid", value: oldSG });
      await sonic.send("/g_new", { type: "uuid", value: newSG }, 1, 0);
      await sonic.sync(1);

      const treeBefore = sonic.getRawTree();
      const beforeLoopC = treeBefore.nodes.find((n) => n.uuid && n.uuid[15] === 0xab);
      const beforeOldFX = treeBefore.nodes.find((n) => n.uuid && n.uuid[15] === 0xf1);
      const beforeOldC = treeBefore.nodes.find((n) => n.uuid && n.uuid[15] === 0xc1);
      const beforeOldSG = treeBefore.nodes.find((n) => n.uuid && n.uuid[15] === 0xa1);
      const beforeNewSG = treeBefore.nodes.find((n) => n.uuid && n.uuid[15] === 0xa2);

      // The bundle under test:
      //   1. /n_order [1, newSG, loopC]   ← move loopC to TAIL of newSG
      //   2. /g_freeAll oldC              ← free everything inside oldC
      const bundle = osc.encodeBundle([0, 1], [
        ["/n_order", 1,
                     { type: "uuid", value: newSG },
                     { type: "uuid", value: loopC }],
        ["/g_freeAll", { type: "uuid", value: oldC }],
      ]);
      sonic.sendOSC(bundle);
      await sonic.sync(2);

      const treeAfter = sonic.getRawTree();
      const afterLoopC = treeAfter.nodes.find((n) => n.uuid && n.uuid[15] === 0xab);
      const afterOldFX = treeAfter.nodes.find((n) => n.uuid && n.uuid[15] === 0xf1);
      const afterOldC = treeAfter.nodes.find((n) => n.uuid && n.uuid[15] === 0xc1);
      const afterOldSG = treeAfter.nodes.find((n) => n.uuid && n.uuid[15] === 0xa1);
      const afterNewSG = treeAfter.nodes.find((n) => n.uuid && n.uuid[15] === 0xa2);

      const newSGChildrenAfter = afterNewSG ? childrenInOrder(treeAfter, afterNewSG.id) : [];
      const oldCChildrenAfter = afterOldC ? childrenInOrder(treeAfter, afterOldC.id) : [];

      try {
        await sonic.send("/g_freeAll", { type: "uuid", value: newSG });
        await sonic.send("/n_free", { type: "uuid", value: newSG });
      } catch (_) {}

      return {
        before: {
          loopC: !!beforeLoopC, oldFX: !!beforeOldFX, oldC: !!beforeOldC,
          oldSG: !!beforeOldSG, newSG: !!beforeNewSG,
          loopCParent: beforeLoopC?.parentId,
        },
        after: {
          loopC: !!afterLoopC, oldFX: !!afterOldFX, oldC: !!afterOldC,
          oldSG: !!afterOldSG, newSG: !!afterNewSG,
          loopCParent: afterLoopC?.parentId,
          newSGId: afterNewSG?.id,
          loopCId: afterLoopC?.id,
          newSGChildren: newSGChildrenAfter,
          oldCChildren: oldCChildrenAfter,
        },
      };
    }, { config: sonicConfig, helpers: NODE_TREE_HELPERS });

    // Before the bundle: all nodes exist, loopC is inside oldSG
    expect(result.before.loopC).toBe(true);
    expect(result.before.oldFX).toBe(true);
    expect(result.before.oldC).toBe(true);
    expect(result.before.oldSG).toBe(true);
    expect(result.before.newSG).toBe(true);

    // CRITICAL: after the bundle, loopC should STILL EXIST (not freed by /g_freeAll).
    expect(result.after.loopC, "loop_container survived the /g_freeAll").toBe(true);
    expect(result.after.loopCParent, "loop_container moved to newSG").toBe(result.after.newSGId);
    expect(result.after.newSGChildren, "newSG contains the moved loop_container").toEqual([result.after.loopCId]);
    expect(result.after.oldFX, "old FX synth was freed").toBe(false);
    expect(result.after.oldSG, "old synth_group was freed").toBe(false);
  });

  test("with_fx pattern × 5 in one bundle, all containers correctly ordered", async ({
    page,
    sonicConfig,
  }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async ({ config, helpers }) => {
      eval(helpers);
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-fx_echo");

      const osc = window.SuperSonic.osc;

      // Build 5 with_fx containers' worth of messages in one bundle.
      const messages = [];
      const expected = [];
      for (let i = 0; i < 5; i++) {
        const cBytes = makeUuid(0x10 + i);
        const sBytes = makeUuid(0x20 + i);
        const fBytes = makeUuid(0x30 + i);
        expected.push({ cSuffix: 0x10 + i, sSuffix: 0x20 + i, fSuffix: 0x30 + i });
        messages.push(["/g_new", { type: "uuid", value: cBytes }, 1, 0]);
        messages.push(["/g_new", { type: "uuid", value: sBytes }, 0,
                       { type: "uuid", value: cBytes }]);
        messages.push(["/s_new", "sonic-pi-fx_echo",
                       { type: "uuid", value: fBytes }, 1,
                       { type: "uuid", value: cBytes },
                       "in_bus", 100, "out_bus", 0, "mix", 0.0]);
      }

      const bundle = osc.encodeBundle([0, 1], messages);
      sonic.sendOSC(bundle);
      await sonic.sync(1);

      const tree = sonic.getRawTree();

      const results = expected.map((e) => {
        const cId = findBySuffix(tree, e.cSuffix);
        const sId = findBySuffix(tree, e.sSuffix);
        const fId = findBySuffix(tree, e.fSuffix);
        return {
          ...e,
          containerId: cId,
          synthGroupId: sId,
          fxSynthId: fId,
          children: cId ? childrenInOrder(tree, cId) : null,
        };
      });

      try {
        for (let i = 0; i < 5; i++) {
          const cBytes = makeUuid(0x10 + i);
          await sonic.send("/g_freeAll", { type: "uuid", value: cBytes });
          await sonic.send("/n_free", { type: "uuid", value: cBytes });
        }
      } catch (_) {}

      return { results };
    }, { config: sonicConfig, helpers: NODE_TREE_HELPERS });

    for (const r of result.results) {
      expect(r.containerId, `container[suffix=0x${r.cSuffix.toString(16)}] exists`).not.toBeNull();
      expect(r.synthGroupId, `synth_group[suffix=0x${r.sSuffix.toString(16)}] exists`).not.toBeNull();
      expect(r.fxSynthId, `fx_synth[suffix=0x${r.fSuffix.toString(16)}] exists`).not.toBeNull();
      expect(
        r.children,
        `container[0x${r.cSuffix.toString(16)}] children = [synth_group, fx_synth]`,
      ).toEqual([r.synthGroupId, r.fxSynthId]);
    }
  });
});

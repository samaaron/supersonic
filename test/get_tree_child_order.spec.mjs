// Verify sonic.getTree() reports children in headId/nextId linked-list order
// (the source of truth) rather than raw.nodes iteration order.

import { test, expect, NODE_TREE_HELPERS } from "./fixtures.mjs";

test.describe("getTree() child order", () => {
  test("HEAD-action inserts produce reverse-creation order", async ({
    page,
    sonicConfig,
  }) => {
    // Create child A at HEAD of group, then child B at HEAD of group.
    // After: group's headId = B, B.nextId = A. Order via linked list: [B, A].
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async ({ config, helpers }) => {
      eval(helpers);
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      await sonic.send("/g_new", 500, 1, 0);             // group 500 at root
      await sonic.send("/s_new", "sonic-pi-beep", 1000, 0, 500, "release", 60); // A at HEAD
      await sonic.send("/s_new", "sonic-pi-beep", 1001, 0, 500, "release", 60); // B at HEAD
      await sonic.sync(1);

      const raw = sonic.getRawTree();
      const tree = sonic.getTree();
      const treeGroup = findInTree(tree.root, 500);
      const treeChildIds = treeGroup ? treeGroup.children.map((c) => c.id) : null;

      await sonic.send("/n_free", 500);

      return {
        rawChildren: childrenInOrder(raw, 500),
        treeChildren: treeChildIds,
      };
    }, { config: sonicConfig, helpers: NODE_TREE_HELPERS });

    // The raw linked-list order is the source of truth: B (1001) was added at
    // HEAD second, so it's now at the head. A (1000) follows.
    expect(result.rawChildren, "raw linked-list order").toEqual([1001, 1000]);
    expect(
      result.treeChildren,
      "getTree() child order must match linked-list order",
    ).toEqual([1001, 1000]);
  });

  test("/n_order move-to-head reorders getTree() output", async ({
    page,
    sonicConfig,
  }) => {
    // Create three children at TAIL: order = [A, B, C].
    // Then /n_order C to HEAD: order = [C, A, B].
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async ({ config, helpers }) => {
      eval(helpers);
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-beep");

      await sonic.send("/g_new", 500, 1, 0);
      await sonic.send("/s_new", "sonic-pi-beep", 1000, 1, 500, "release", 60); // A tail
      await sonic.send("/s_new", "sonic-pi-beep", 1001, 1, 500, "release", 60); // B tail
      await sonic.send("/s_new", "sonic-pi-beep", 1002, 1, 500, "release", 60); // C tail
      await sonic.send("/n_order", 0, 500, 1002);                                // move C to HEAD of 500
      await sonic.sync(1);

      const raw = sonic.getRawTree();
      const tree = sonic.getTree();
      const treeGroup = findInTree(tree.root, 500);
      const treeChildIds = treeGroup ? treeGroup.children.map((c) => c.id) : null;

      await sonic.send("/n_free", 500);

      return {
        rawChildren: childrenInOrder(raw, 500),
        treeChildren: treeChildIds,
      };
    }, { config: sonicConfig, helpers: NODE_TREE_HELPERS });

    expect(result.rawChildren, "raw linked-list order after /n_order").toEqual([1002, 1000, 1001]);
    expect(
      result.treeChildren,
      "getTree() must reflect /n_order moves",
    ).toEqual([1002, 1000, 1001]);
  });

  test("with_fx pattern: synth_group at HEAD, fx_synth at TAIL", async ({
    page,
    sonicConfig,
  }) => {
    // The exact pattern tau-state's compile_with_fx emits:
    //   /g_new container 1 root          ← container at TAIL of root
    //   /g_new synth_group 0 container   ← synth_group at HEAD of container
    //   /s_new fx_synth 1 container      ← fx_synth at TAIL of container
    // Expected getTree() order of container's children: [synth_group, fx_synth]
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async ({ config, helpers }) => {
      eval(helpers);
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef("sonic-pi-fx_echo");

      await sonic.send("/g_new", 500, 1, 0);                           // container at TAIL of root
      await sonic.send("/g_new", 501, 0, 500);                         // synth_group at HEAD of container
      await sonic.send("/s_new", "sonic-pi-fx_echo", 1000, 1, 500,     // fx_synth at TAIL of container
                       "in_bus", 100, "out_bus", 0, "mix", 0.0);
      await sonic.sync(1);

      const raw = sonic.getRawTree();
      const tree = sonic.getTree();
      const treeGroup = findInTree(tree.root, 500);
      const treeChildIds = treeGroup ? treeGroup.children.map((c) => c.id) : null;

      await sonic.send("/g_freeAll", 500);
      await sonic.send("/n_free", 500);

      return {
        rawChildren: childrenInOrder(raw, 500),
        treeChildren: treeChildIds,
      };
    }, { config: sonicConfig, helpers: NODE_TREE_HELPERS });

    // Expected: [501, 1000] (synth_group first, fx_synth second)
    expect(result.rawChildren, "raw linked-list order").toEqual([501, 1000]);
    expect(
      result.treeChildren,
      "getTree() child order must match for with_fx pattern",
    ).toEqual([501, 1000]);
  });
});

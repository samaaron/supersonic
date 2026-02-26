/**
 * UUID Rewriter Integration Tests
 *
 * Tests the C++ UUID↔int32 rewriter that transparently converts UUID node IDs
 * (OSC type tag 'u', 16 bytes) to int32 ('i', 4 bytes) for scsynth, and
 * reverse-maps int32 back to UUID in node-lifecycle replies.
 *
 * Outbound: tau-state sends UUID node IDs → rewriter converts to int32 → scsynth happy
 * Inbound: scsynth replies with int32 → rewriter expands back to UUID → JS sees UUIDs
 */

import { test, expect, skipIfPostMessage } from "./fixtures.mjs";

// Helper: create a random 16-byte UUID (as array of numbers for serialization)
function makeUuid() {
  const bytes = new Array(16);
  for (let i = 0; i < 16; i++) bytes[i] = Math.floor(Math.random() * 256);
  // Set version 7 bits for realism (not required for functionality)
  bytes[6] = (bytes[6] & 0x0f) | 0x70;
  bytes[8] = (bytes[8] & 0x3f) | 0x80;
  return bytes;
}

// =============================================================================
// OUTBOUND REWRITING (u → i)
// =============================================================================

test.describe("Outbound UUID rewriting", () => {
  test("UUID arg rewritten to int32 for /s_new", async ({
    page,
    sonicConfig,
  }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      const messages = [];
      sonic.on("in", (msg) => messages.push(msg));
      await sonic.init();
      await sonic.send("/notify", 1);
      await sonic.loadSynthDef("sonic-pi-beep");

      // Send /s_new with UUID node ID using osc_fast tagged type
      const uuid = new Uint8Array([
        0x01, 0x93, 0xa5, 0xb0, 0x7c, 0x8a, 0x70, 0x00, 0x80, 0x00, 0xde,
        0xad, 0xbe, 0xef, 0xca, 0xfe,
      ]);
      await sonic.send("/s_new", "sonic-pi-beep", { type: "uuid", value: uuid }, 0, 0, "release", 60);
      await sonic.sync(1);

      // Wait for /n_go reply
      await new Promise((r) => setTimeout(r, 200));

      const nGo = messages.find((m) => m[0] === "/n_go");
      return {
        gotReply: !!nGo,
        // If rewriter works, first arg should be UUID object (not int32)
        firstArgType: nGo ? typeof nGo[1] : null,
        firstArgIsUuid: nGo?.[1]?.type === "uuid",
        firstArgBytes: nGo?.[1]?.type === "uuid"
          ? Array.from(nGo[1].value)
          : null,
      };
    }, sonicConfig);

    expect(result.gotReply).toBe(true);
    expect(result.firstArgIsUuid).toBe(true);
    // UUID should match what we sent
    expect(result.firstArgBytes).toEqual([
      0x01, 0x93, 0xa5, 0xb0, 0x7c, 0x8a, 0x70, 0x00, 0x80, 0x00, 0xde,
      0xad, 0xbe, 0xef, 0xca, 0xfe,
    ]);
  });

  test("non-UUID messages pass through unchanged", async ({
    page,
    sonicConfig,
  }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      const messages = [];
      sonic.on("in", (msg) => messages.push(msg));
      await sonic.init();
      await sonic.send("/notify", 1);
      await sonic.loadSynthDef("sonic-pi-beep");

      // Send /s_new with normal int32 node ID
      await sonic.send("/s_new", "sonic-pi-beep", 2000, 0, 0, "release", 60);
      await sonic.sync(1);
      await new Promise((r) => setTimeout(r, 200));

      const nGo = messages.find((m) => m[0] === "/n_go");
      return {
        gotReply: !!nGo,
        firstArg: nGo?.[1],
        firstArgIsInt: typeof nGo?.[1] === "number",
      };
    }, sonicConfig);

    expect(result.gotReply).toBe(true);
    expect(result.firstArgIsInt).toBe(true);
    expect(result.firstArg).toBe(2000);
  });

  test("same UUID maps to same int32 across messages", async ({
    page,
    sonicConfig,
  }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      const messages = [];
      sonic.on("in", (msg) => messages.push(msg));
      await sonic.init();
      await sonic.send("/notify", 1);
      await sonic.loadSynthDef("sonic-pi-beep");

      const uuid = new Uint8Array([
        0x01, 0x93, 0xa5, 0xb0, 0x7c, 0x8a, 0x70, 0x01, 0x80, 0x00, 0xaa,
        0xbb, 0xcc, 0xdd, 0xee, 0xff,
      ]);

      // Create synth with UUID
      await sonic.send("/s_new", "sonic-pi-beep", { type: "uuid", value: uuid }, 0, 0, "release", 60);
      await sonic.sync(1);

      // Set a parameter on the same UUID
      await sonic.send("/n_set", { type: "uuid", value: uuid }, "freq", 440);
      await sonic.sync(2);
      await new Promise((r) => setTimeout(r, 200));

      // If both messages mapped to the same int32, /n_set would succeed
      // (no /fail reply for the set command)
      const fail = messages.find((m) => m[0] === "/fail");
      const nGo = messages.find((m) => m[0] === "/n_go");

      return {
        noFail: !fail,
        gotNGo: !!nGo,
      };
    }, sonicConfig);

    expect(result.noFail).toBe(true);
    expect(result.gotNGo).toBe(true);
  });
});

// =============================================================================
// INBOUND REWRITING (i → u)
// =============================================================================

test.describe("Inbound UUID rewriting", () => {
  test("/n_end reply contains UUID", async ({ page, sonicConfig }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      const messages = [];
      sonic.on("in", (msg) => messages.push(msg));
      await sonic.init();
      await sonic.send("/notify", 1);
      await sonic.loadSynthDef("sonic-pi-beep");

      const uuid = new Uint8Array([
        0x01, 0x93, 0xa5, 0xb0, 0x7c, 0x8a, 0x70, 0x02, 0x80, 0x00, 0x11,
        0x22, 0x33, 0x44, 0x55, 0x66,
      ]);

      // Create and immediately free synth
      await sonic.send("/s_new", "sonic-pi-beep", { type: "uuid", value: uuid }, 0, 0, "release", 60);
      await sonic.sync(1);
      await sonic.send("/n_free", { type: "uuid", value: uuid });
      await sonic.sync(2);
      await new Promise((r) => setTimeout(r, 200));

      const nEnd = messages.find((m) => m[0] === "/n_end");
      return {
        gotNEnd: !!nEnd,
        firstArgIsUuid: nEnd?.[1]?.type === "uuid",
        firstArgBytes: nEnd?.[1]?.type === "uuid"
          ? Array.from(nEnd[1].value)
          : null,
      };
    }, sonicConfig);

    expect(result.gotNEnd).toBe(true);
    expect(result.firstArgIsUuid).toBe(true);
    expect(result.firstArgBytes).toEqual([
      0x01, 0x93, 0xa5, 0xb0, 0x7c, 0x8a, 0x70, 0x02, 0x80, 0x00, 0x11,
      0x22, 0x33, 0x44, 0x55, 0x66,
    ]);
  });

  test("/n_go reply contains UUID", async ({ page, sonicConfig }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      const messages = [];
      sonic.on("in", (msg) => messages.push(msg));
      await sonic.init();
      await sonic.send("/notify", 1);
      await sonic.loadSynthDef("sonic-pi-beep");

      const uuid = new Uint8Array([
        0x01, 0x93, 0xa5, 0xb0, 0x7c, 0x8a, 0x70, 0x03, 0x80, 0x00, 0xfe,
        0xdc, 0xba, 0x98, 0x76, 0x54,
      ]);

      await sonic.send("/s_new", "sonic-pi-beep", { type: "uuid", value: uuid }, 0, 0, "release", 60);
      await sonic.sync(1);
      await new Promise((r) => setTimeout(r, 200));

      const nGo = messages.find((m) => m[0] === "/n_go");
      return {
        gotNGo: !!nGo,
        firstArgIsUuid: nGo?.[1]?.type === "uuid",
        firstArgBytes: nGo?.[1]?.type === "uuid"
          ? Array.from(nGo[1].value)
          : null,
      };
    }, sonicConfig);

    expect(result.gotNGo).toBe(true);
    expect(result.firstArgIsUuid).toBe(true);
    expect(result.firstArgBytes).toEqual([
      0x01, 0x93, 0xa5, 0xb0, 0x7c, 0x8a, 0x70, 0x03, 0x80, 0x00, 0xfe,
      0xdc, 0xba, 0x98, 0x76, 0x54,
    ]);
  });

  test("non-UUID node /n_end passes through as int32", async ({
    page,
    sonicConfig,
  }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      const messages = [];
      sonic.on("in", (msg) => messages.push(msg));
      await sonic.init();
      await sonic.send("/notify", 1);
      await sonic.loadSynthDef("sonic-pi-beep");

      // Create synth with plain int32
      await sonic.send("/s_new", "sonic-pi-beep", 3000, 0, 0, "release", 60);
      await sonic.sync(1);
      await sonic.send("/n_free", 3000);
      await sonic.sync(2);
      await new Promise((r) => setTimeout(r, 200));

      const nEnd = messages.find((m) => m[0] === "/n_end");
      return {
        gotNEnd: !!nEnd,
        firstArgIsInt: typeof nEnd?.[1] === "number",
        firstArg: nEnd?.[1],
      };
    }, sonicConfig);

    expect(result.gotNEnd).toBe(true);
    expect(result.firstArgIsInt).toBe(true);
    expect(result.firstArg).toBe(3000);
  });
});

// =============================================================================
// SECONDARY NODE ID REWRITING
// =============================================================================
// Lifecycle replies (/n_go, /n_end, /n_info etc.) return:
//   nodeID, parentGroupID, prevNodeID, nextNodeID, isGroup, [headNodeID, tailNodeID]
// Currently only the first arg (nodeID) is rewritten. These tests document
// the gaps where secondary node IDs should also be UUIDs.

test.describe("Secondary node ID rewriting", () => {
  test("/n_go prevNodeID should be UUID when prev node was UUID-created", async ({
    page,
    sonicConfig,
  }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      const messages = [];
      sonic.on("in", (msg) => messages.push(msg));
      await sonic.init();
      await sonic.send("/notify", 1);
      await sonic.loadSynthDef("sonic-pi-beep");

      const uuidA = new Uint8Array([
        0x01, 0x93, 0xa5, 0xb0, 0x7c, 0x8a, 0x70, 0x10, 0x80, 0x00, 0xAA,
        0xAA, 0xAA, 0xAA, 0xAA, 0xAA,
      ]);
      const uuidB = new Uint8Array([
        0x01, 0x93, 0xa5, 0xb0, 0x7c, 0x8a, 0x70, 0x11, 0x80, 0x00, 0xBB,
        0xBB, 0xBB, 0xBB, 0xBB, 0xBB,
      ]);

      // Create A then B in default group (addAction=0 = head)
      // B added to head, so B's prevNodeID should be -1 and A's prevNodeID is B
      // Use addAction=1 (tail) so B goes after A: B.prev = A
      await sonic.send("/s_new", "sonic-pi-beep", { type: "uuid", value: uuidA }, 1, 0, "release", 60);
      await sonic.sync(1);
      await sonic.send("/s_new", "sonic-pi-beep", { type: "uuid", value: uuidB }, 1, 0, "release", 60);
      await sonic.sync(2);
      await new Promise((r) => setTimeout(r, 200));

      // B's /n_go is the second one — its prevNodeID (arg index 3) should be A's UUID
      const nGoMsgs = messages.filter((m) => m[0] === "/n_go");
      const bNGo = nGoMsgs[1];

      return {
        gotBothNGo: nGoMsgs.length >= 2,
        // arg layout: ["/n_go", nodeID, parentGroupID, prevNodeID, nextNodeID, isGroup]
        prevNodeId: bNGo?.[3],
        prevNodeIdIsUuid: bNGo?.[3]?.type === "uuid",
        prevNodeIdBytes: bNGo?.[3]?.type === "uuid"
          ? Array.from(bNGo[3].value)
          : null,
      };
    }, sonicConfig);

    expect(result.gotBothNGo).toBe(true);
    expect(result.prevNodeIdIsUuid).toBe(true);
    expect(result.prevNodeIdBytes).toEqual([
      0x01, 0x93, 0xa5, 0xb0, 0x7c, 0x8a, 0x70, 0x10, 0x80, 0x00, 0xAA,
      0xAA, 0xAA, 0xAA, 0xAA, 0xAA,
    ]);
  });

  test("/n_info nextNodeID should be UUID when next node was UUID-created", async ({
    page,
    sonicConfig,
  }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      const messages = [];
      sonic.on("in", (msg) => messages.push(msg));
      await sonic.init();
      await sonic.send("/notify", 1);
      await sonic.loadSynthDef("sonic-pi-beep");

      const uuidA = new Uint8Array([
        0x01, 0x93, 0xa5, 0xb0, 0x7c, 0x8a, 0x70, 0x12, 0x80, 0x00, 0xCC,
        0xCC, 0xCC, 0xCC, 0xCC, 0xCC,
      ]);
      const uuidB = new Uint8Array([
        0x01, 0x93, 0xa5, 0xb0, 0x7c, 0x8a, 0x70, 0x13, 0x80, 0x00, 0xDD,
        0xDD, 0xDD, 0xDD, 0xDD, 0xDD,
      ]);

      // Create A then B (tail), so A.next = B
      await sonic.send("/s_new", "sonic-pi-beep", { type: "uuid", value: uuidA }, 1, 0, "release", 60);
      await sonic.sync(1);
      await sonic.send("/s_new", "sonic-pi-beep", { type: "uuid", value: uuidB }, 1, 0, "release", 60);
      await sonic.sync(2);

      // Query A's info — its nextNodeID should be B's UUID
      await sonic.send("/n_query", { type: "uuid", value: uuidA });
      await sonic.sync(3);
      await new Promise((r) => setTimeout(r, 200));

      const nInfo = messages.find((m) => m[0] === "/n_info");
      // /n_info layout: ["/n_info", nodeID, parentGroupID, prevNodeID, nextNodeID, isGroup]
      return {
        gotNInfo: !!nInfo,
        nextNodeId: nInfo?.[4],
        nextNodeIdIsUuid: nInfo?.[4]?.type === "uuid",
        nextNodeIdBytes: nInfo?.[4]?.type === "uuid"
          ? Array.from(nInfo[4].value)
          : null,
      };
    }, sonicConfig);

    expect(result.gotNInfo).toBe(true);
    expect(result.nextNodeIdIsUuid).toBe(true);
    expect(result.nextNodeIdBytes).toEqual([
      0x01, 0x93, 0xa5, 0xb0, 0x7c, 0x8a, 0x70, 0x13, 0x80, 0x00, 0xDD,
      0xDD, 0xDD, 0xDD, 0xDD, 0xDD,
    ]);
  });

  test("/n_go parentGroupID should be UUID when parent group was UUID-created", async ({
    page,
    sonicConfig,
  }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      const messages = [];
      sonic.on("in", (msg) => messages.push(msg));
      await sonic.init();
      await sonic.send("/notify", 1);
      await sonic.loadSynthDef("sonic-pi-beep");

      const groupUuid = new Uint8Array([
        0x01, 0x93, 0xa5, 0xb0, 0x7c, 0x8a, 0x70, 0x14, 0x80, 0x00, 0xEE,
        0xEE, 0xEE, 0xEE, 0xEE, 0xEE,
      ]);
      const synthUuid = new Uint8Array([
        0x01, 0x93, 0xa5, 0xb0, 0x7c, 0x8a, 0x70, 0x15, 0x80, 0x00, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
      ]);

      // Create group with UUID, then synth inside it
      await sonic.send("/g_new", { type: "uuid", value: groupUuid }, 1, 0);
      await sonic.sync(1);
      await sonic.send("/s_new", "sonic-pi-beep", { type: "uuid", value: synthUuid }, 0, { type: "uuid", value: groupUuid }, "release", 60);
      await sonic.sync(2);
      await new Promise((r) => setTimeout(r, 200));

      // The synth's /n_go should have the group UUID as parentGroupID (arg index 2)
      const nGoMsgs = messages.filter((m) => m[0] === "/n_go");
      // First /n_go is the group, second is the synth
      const synthNGo = nGoMsgs.find((m) => m[1]?.type === "uuid" &&
        Array.from(m[1].value).every((b, i) => b === synthUuid[i]));

      return {
        gotSynthNGo: !!synthNGo,
        parentGroupId: synthNGo?.[2],
        parentGroupIdIsUuid: synthNGo?.[2]?.type === "uuid",
        parentGroupIdBytes: synthNGo?.[2]?.type === "uuid"
          ? Array.from(synthNGo[2].value)
          : null,
      };
    }, sonicConfig);

    expect(result.gotSynthNGo).toBe(true);
    expect(result.parentGroupIdIsUuid).toBe(true);
    expect(result.parentGroupIdBytes).toEqual([
      0x01, 0x93, 0xa5, 0xb0, 0x7c, 0x8a, 0x70, 0x14, 0x80, 0x00, 0xEE,
      0xEE, 0xEE, 0xEE, 0xEE, 0xEE,
    ]);
  });

  test("/n_set reply should contain UUID as first arg", async ({
    page,
    sonicConfig,
  }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      const messages = [];
      sonic.on("in", (msg) => messages.push(msg));
      await sonic.init();
      await sonic.send("/notify", 1);
      await sonic.loadSynthDef("sonic-pi-beep");

      const uuid = new Uint8Array([
        0x01, 0x93, 0xa5, 0xb0, 0x7c, 0x8a, 0x70, 0x16, 0x80, 0x00, 0x11,
        0x11, 0x11, 0x11, 0x11, 0x11,
      ]);

      await sonic.send("/s_new", "sonic-pi-beep", { type: "uuid", value: uuid }, 0, 0, "release", 60);
      await sonic.sync(1);

      // /s_get triggers a /n_set reply with the node ID as first arg
      await sonic.send("/s_get", { type: "uuid", value: uuid }, "out");
      await sonic.sync(2);
      await new Promise((r) => setTimeout(r, 200));

      const nSet = messages.find((m) => m[0] === "/n_set");
      return {
        gotNSet: !!nSet,
        firstArgIsUuid: nSet?.[1]?.type === "uuid",
        firstArgBytes: nSet?.[1]?.type === "uuid"
          ? Array.from(nSet[1].value)
          : null,
      };
    }, sonicConfig);

    expect(result.gotNSet).toBe(true);
    expect(result.firstArgIsUuid).toBe(true);
    expect(result.firstArgBytes).toEqual([
      0x01, 0x93, 0xa5, 0xb0, 0x7c, 0x8a, 0x70, 0x16, 0x80, 0x00, 0x11,
      0x11, 0x11, 0x11, 0x11, 0x11,
    ]);
  });

  test("/n_end secondary args should be UUIDs for UUID-created neighbours", async ({
    page,
    sonicConfig,
  }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      const messages = [];
      sonic.on("in", (msg) => messages.push(msg));
      await sonic.init();
      await sonic.send("/notify", 1);
      await sonic.loadSynthDef("sonic-pi-beep");

      const uuidA = new Uint8Array([
        0x01, 0x93, 0xa5, 0xb0, 0x7c, 0x8a, 0x70, 0x17, 0x80, 0x00, 0x22,
        0x22, 0x22, 0x22, 0x22, 0x22,
      ]);
      const uuidB = new Uint8Array([
        0x01, 0x93, 0xa5, 0xb0, 0x7c, 0x8a, 0x70, 0x18, 0x80, 0x00, 0x33,
        0x33, 0x33, 0x33, 0x33, 0x33,
      ]);
      const uuidC = new Uint8Array([
        0x01, 0x93, 0xa5, 0xb0, 0x7c, 0x8a, 0x70, 0x19, 0x80, 0x00, 0x44,
        0x44, 0x44, 0x44, 0x44, 0x44,
      ]);

      // Create A, B, C in order (tail), so A.next=B, B.prev=A, B.next=C
      await sonic.send("/s_new", "sonic-pi-beep", { type: "uuid", value: uuidA }, 1, 0, "release", 60);
      await sonic.send("/s_new", "sonic-pi-beep", { type: "uuid", value: uuidB }, 1, 0, "release", 60);
      await sonic.send("/s_new", "sonic-pi-beep", { type: "uuid", value: uuidC }, 1, 0, "release", 60);
      await sonic.sync(1);

      // Free B — its /n_end should have A as prevNodeID and C as nextNodeID
      await sonic.send("/n_free", { type: "uuid", value: uuidB });
      await sonic.sync(2);
      await new Promise((r) => setTimeout(r, 200));

      const nEnd = messages.find((m) => m[0] === "/n_end" && m[1]?.type === "uuid" &&
        Array.from(m[1].value).every((b, i) => b === uuidB[i]));

      // /n_end layout: ["/n_end", nodeID, parentGroupID, prevNodeID, nextNodeID, isGroup]
      return {
        gotNEnd: !!nEnd,
        prevNodeIdIsUuid: nEnd?.[3]?.type === "uuid",
        prevNodeIdBytes: nEnd?.[3]?.type === "uuid"
          ? Array.from(nEnd[3].value)
          : null,
        nextNodeIdIsUuid: nEnd?.[4]?.type === "uuid",
        nextNodeIdBytes: nEnd?.[4]?.type === "uuid"
          ? Array.from(nEnd[4].value)
          : null,
      };
    }, sonicConfig);

    expect(result.gotNEnd).toBe(true);
    // prevNodeID should be UUID A
    expect(result.prevNodeIdIsUuid).toBe(true);
    expect(result.prevNodeIdBytes).toEqual([
      0x01, 0x93, 0xa5, 0xb0, 0x7c, 0x8a, 0x70, 0x17, 0x80, 0x00, 0x22,
      0x22, 0x22, 0x22, 0x22, 0x22,
    ]);
    // nextNodeID should be UUID C
    expect(result.nextNodeIdIsUuid).toBe(true);
    expect(result.nextNodeIdBytes).toEqual([
      0x01, 0x93, 0xa5, 0xb0, 0x7c, 0x8a, 0x70, 0x19, 0x80, 0x00, 0x44,
      0x44, 0x44, 0x44, 0x44, 0x44,
    ]);
  });
});

// =============================================================================
// MAP PRUNING
// =============================================================================

test.describe("UUID map pruning", () => {
  test("/n_end removes mapping", async ({ page, sonicConfig }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      const messages = [];
      sonic.on("in", (msg) => messages.push(msg));
      await sonic.init();
      await sonic.send("/notify", 1);
      await sonic.loadSynthDef("sonic-pi-beep");

      // Create and free synth with UUID A
      const uuidA = new Uint8Array([
        0x01, 0x93, 0xa5, 0xb0, 0x7c, 0x8a, 0x70, 0x04, 0x80, 0x00, 0xAA,
        0xAA, 0xAA, 0xAA, 0xAA, 0xAA,
      ]);
      await sonic.send("/s_new", "sonic-pi-beep", { type: "uuid", value: uuidA }, 0, 0, "release", 60);
      await sonic.sync(1);
      await sonic.send("/n_free", { type: "uuid", value: uuidA });
      await sonic.sync(2);
      await new Promise((r) => setTimeout(r, 200));

      // Create synth with UUID B
      const uuidB = new Uint8Array([
        0x01, 0x93, 0xa5, 0xb0, 0x7c, 0x8a, 0x70, 0x05, 0x80, 0x00, 0xBB,
        0xBB, 0xBB, 0xBB, 0xBB, 0xBB,
      ]);
      await sonic.send("/s_new", "sonic-pi-beep", { type: "uuid", value: uuidB }, 0, 0, "release", 60);
      await sonic.sync(3);
      await new Promise((r) => setTimeout(r, 200));

      // Both operations should succeed — B should get a fresh int32
      const nGoMsgs = messages.filter((m) => m[0] === "/n_go");
      const nEndMsgs = messages.filter((m) => m[0] === "/n_end");

      return {
        nGoCount: nGoMsgs.length,
        nEndCount: nEndMsgs.length,
        // UUID B's /n_go should have UUID B's bytes
        secondNGoIsUuid: nGoMsgs[1]?.[1]?.type === "uuid",
      };
    }, sonicConfig);

    expect(result.nGoCount).toBe(2);
    expect(result.nEndCount).toBe(1);
    expect(result.secondNGoIsUuid).toBe(true);
  });
});

// =============================================================================
// STRESS TESTS
// =============================================================================

test.describe("UUID stress tests", () => {
  test("UUID + non-UUID interleaved", async ({ page, sonicConfig }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      const messages = [];
      sonic.on("in", (msg) => messages.push(msg));
      await sonic.init();
      await sonic.send("/notify", 1);
      await sonic.loadSynthDef("sonic-pi-beep");

      // Create some synths with UUIDs and some with int32s
      const uuids = [];
      for (let i = 0; i < 5; i++) {
        const uuid = new Uint8Array(16);
        uuid[0] = 0x01;
        uuid[15] = i + 1;
        uuids.push(uuid);
        await sonic.send(
          "/s_new", "sonic-pi-beep", { type: "uuid", value: uuid }, 0, 0, "release", 60
        );
      }
      for (let i = 0; i < 5; i++) {
        await sonic.send("/s_new", "sonic-pi-beep", 4000 + i, 0, 0, "release", 60);
      }
      await sonic.sync(1);
      await new Promise((r) => setTimeout(r, 300));

      const nGoMsgs = messages.filter((m) => m[0] === "/n_go");
      const uuidReplies = nGoMsgs.filter((m) => m[1]?.type === "uuid");
      const intReplies = nGoMsgs.filter((m) => typeof m[1] === "number");

      return {
        totalNGo: nGoMsgs.length,
        uuidReplies: uuidReplies.length,
        intReplies: intReplies.length,
      };
    }, sonicConfig);

    expect(result.totalNGo).toBe(10);
    expect(result.uuidReplies).toBe(5);
    expect(result.intReplies).toBe(5);
  });

  test("12000 UUID synths across range boundaries — create/free cycles", async ({ page, sonicConfig }) => {
    test.setTimeout(120000);
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      const nGoMessages = [];
      const nEndMessages = [];
      sonic.on("in", (msg) => {
        if (msg[0] === "/n_go") nGoMessages.push(msg);
        else if (msg[0] === "/n_end") nEndMessages.push(msg);
      });
      await sonic.init();
      await sonic.send("/notify", 1);
      await sonic.loadSynthDef("sonic-pi-beep");

      // 12,000 total UUID synths — exceeds the 10,000 initial worklet range
      // in PM mode, forcing at least one async range refill.
      // Create/free in cycles of 200 to keep live synth count low.
      const TOTAL = 12000;
      const BATCH = 200;
      const sentUuids = new Set();
      let syncId = 1;

      for (let offset = 0; offset < TOTAL; offset += BATCH) {
        const uuids = [];
        const end = Math.min(offset + BATCH, TOTAL);

        // Create batch
        for (let i = offset; i < end; i++) {
          const uuid = new Uint8Array(16);
          uuid[0] = 0x01; uuid[1] = 0x93; uuid[2] = 0xa5; uuid[3] = 0xb0;
          uuid[4] = (i >> 24) & 0xff;
          uuid[5] = (i >> 16) & 0xff;
          uuid[6] = 0x70 | ((i >> 12) & 0x0f);
          uuid[7] = (i >> 4) & 0xff;
          uuid[8] = 0x80 | (i & 0x0f);
          uuid[9] = 0x00;
          uuid[10] = (i >> 8) & 0xff;
          uuid[11] = i & 0xff;
          uuid[12] = 0xDE; uuid[13] = 0xAD; uuid[14] = 0xBE; uuid[15] = 0xEF;
          uuids.push(uuid);
          const hex = Array.from(uuid).map(b => b.toString(16).padStart(2, '0')).join('');
          sentUuids.add(hex);
          await sonic.send("/s_new", "sonic-pi-beep", { type: "uuid", value: uuid }, 0, 0, "release", 60);
        }
        await sonic.sync(syncId++);

        // Free batch
        for (const uuid of uuids) {
          await sonic.send("/n_free", { type: "uuid", value: uuid });
        }
        await sonic.sync(syncId++);

        // Brief yield to let replies flow and PM range refills arrive
        await new Promise((r) => setTimeout(r, 50));
      }

      // Final wait for stragglers
      await new Promise((r) => setTimeout(r, 500));

      // Collect received UUID hex strings from /n_go
      const receivedHexSet = new Set();
      for (const msg of nGoMessages) {
        if (msg[1]?.type === "uuid") {
          const hex = Array.from(msg[1].value).map(b => b.toString(16).padStart(2, '0')).join('');
          receivedHexSet.add(hex);
        }
      }

      let missingCount = 0;
      for (const hex of sentUuids) {
        if (!receivedHexSet.has(hex)) missingCount++;
      }

      return {
        sent: TOTAL,
        nGoTotal: nGoMessages.length,
        nGoUuids: nGoMessages.filter(m => m[1]?.type === "uuid").length,
        nEndTotal: nEndMessages.length,
        uniqueUuidsReceived: receivedHexSet.size,
        missingUuids: missingCount,
      };
    }, sonicConfig);

    expect(result.nGoTotal).toBe(12000);
    expect(result.nGoUuids).toBe(12000);
    expect(result.uniqueUuidsReceived).toBe(12000);
    expect(result.missingUuids).toBe(0);
    // All synths were freed
    expect(result.nEndTotal).toBe(12000);
  });
});

/**
 * Completion Message Test Suite
 *
 * Tests OSC completion messages — embedded OSC messages that scsynth
 * auto-executes after an async command completes. Supported on:
 * - /d_recv
 * - /b_free
 * - /b_zero
 *
 * Note: /b_alloc, /b_allocRead, /b_allocReadChannel completion messages
 * are NOT forwarded because these commands are rewritten by the OSC rewriter.
 */

import { test, expect } from "./fixtures.mjs";

test.describe("Completion Messages", () => {
  test("/d_recv completion message triggers /status", async ({ page, sonicConfig }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);

      const messages = [];
      sonic.on('message', (msg) => messages.push(msg));

      await sonic.init();

      // Fetch synthdef bytes manually
      const resp = await fetch(config.synthdefBaseURL + "sonic-pi-beep.scsyndef");
      const synthdefBytes = new Uint8Array(await resp.arrayBuffer());

      // Encode a /status message to use as completion
      const completionMsg = window.SuperSonic.osc.encodeMessage("/status");

      // Clear and send /d_recv with completion message
      messages.length = 0;
      sonic.send("/d_recv", synthdefBytes, completionMsg);
      await sonic.sync(1);

      // The completion message should have triggered a /status.reply
      const doneMsg = messages.find(
        (m) => m[0] === "/done" && m[1] === "/d_recv"
      );
      const statusReply = messages.find((m) => m[0] === "/status.reply");

      return {
        hasDone: !!doneMsg,
        hasStatusReply: !!statusReply,
        allMessages: messages.map(m => m[0]),
        completionMsgSize: completionMsg.length,
      };
    }, sonicConfig);

    expect(result.hasDone).toBe(true);
    expect(result.hasStatusReply).toBe(true);
  });

  test("/b_free completion message triggers /status", async ({ page, sonicConfig }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);

      const messages = [];
      sonic.on('message', (msg) => messages.push(msg));

      await sonic.init();

      // Allocate a buffer first
      sonic.send("/b_alloc", 0, 1024, 1);
      await sonic.sync(1);

      // Encode a /status message as the completion
      const completionMsg = window.SuperSonic.osc.encodeMessage("/status");

      // Free the buffer with a completion message
      messages.length = 0;
      sonic.send("/b_free", 0, completionMsg);
      await sonic.sync(2);

      const doneMsg = messages.find(
        (m) => m[0] === "/done" && m[1] === "/b_free"
      );
      const statusReply = messages.find((m) => m[0] === "/status.reply");

      return {
        hasDone: !!doneMsg,
        hasStatusReply: !!statusReply,
      };
    }, sonicConfig);

    expect(result.hasDone).toBe(true);
    expect(result.hasStatusReply).toBe(true);
  });

  test("/b_zero completion message triggers /status", async ({ page, sonicConfig }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);

      const messages = [];
      sonic.on('message', (msg) => messages.push(msg));

      await sonic.init();

      // Allocate a buffer first
      sonic.send("/b_alloc", 0, 1024, 1);
      await sonic.sync(1);

      // Encode a /status message as the completion
      const completionMsg = window.SuperSonic.osc.encodeMessage("/status");

      // Zero the buffer with a completion message
      messages.length = 0;
      sonic.send("/b_zero", 0, completionMsg);
      await sonic.sync(2);

      const doneMsg = messages.find(
        (m) => m[0] === "/done" && m[1] === "/b_zero"
      );
      const statusReply = messages.find((m) => m[0] === "/status.reply");

      // Cleanup
      sonic.send("/b_free", 0);

      return {
        hasDone: !!doneMsg,
        hasStatusReply: !!statusReply,
      };
    }, sonicConfig);

    expect(result.hasDone).toBe(true);
    expect(result.hasStatusReply).toBe(true);
  });

  test("/d_recv completion message can chain /s_new", async ({ page, sonicConfig }) => {
    await page.goto("/test/harness.html");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);

      const messages = [];
      sonic.on('message', (msg) => messages.push(msg));

      await sonic.init();

      await sonic.send("/notify", 1);

      // Fetch synthdef bytes
      const resp = await fetch(config.synthdefBaseURL + "sonic-pi-beep.scsyndef");
      const synthdefBytes = new Uint8Array(await resp.arrayBuffer());

      // Completion message: create a synth immediately after synthdef loads
      const completionMsg = window.SuperSonic.osc.encodeMessage(
        "/s_new", ["sonic-pi-beep", 9999, 0, 0]
      );

      messages.length = 0;
      sonic.send("/d_recv", synthdefBytes, completionMsg);
      await sonic.sync(1);

      // The synth should have been created by the completion message
      const nGo = messages.find(
        (m) => m[0] === "/n_go" && m[1] === 9999
      );

      // Verify via node tree
      const tree = sonic.getRawTree();
      const synthNode = tree.nodes.find((n) => n.id === 9999);

      // Cleanup
      sonic.send("/n_free", 9999);

      return {
        hasNGo: !!nGo,
        synthInTree: !!synthNode,
        synthDefName: synthNode?.defName,
      };
    }, sonicConfig);

    expect(result.hasNGo).toBe(true);
    expect(result.synthInTree).toBe(true);
    expect(result.synthDefName).toBe("sonic-pi-beep");
  });
});

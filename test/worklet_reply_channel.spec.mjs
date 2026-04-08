// Verify the full reply path from scsynth → OUT ring → osc_in_worker fan-out →
// reply channel buffer → worklet pollReplies() delivers /n_end events.

import { test, expect, REPLY_WORKLET_MSG } from "./fixtures.mjs";

test.describe("Worklet Reply Channel", () => {
  test("pollReplies receives /n_end in AudioWorklet", async ({
    page,
    sonicConfig,
    sonicMode,
  }) => {
    test.skip(sonicMode !== "sab", "Reply channels require SAB mode");
    test.setTimeout(30000);

    await page.goto("/test/harness.html");
    await page.waitForFunction(() => window.supersonicReady === true, {
      timeout: 10000,
    });

    const result = await page.evaluate(async ({ config, MSG }) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.send("/notify", 1);
      await sonic.loadSynthDefs(["sonic-pi-beep"]);
      await sonic.sync(1);

      // Create OscChannel and transfer to worklet
      const channel = sonic.createOscChannel({ blocking: false });
      const ctx = sonic.audioContext;
      await ctx.audioWorklet.addModule("/test/assets/reply_oscchannel_test_worklet.js");

      const node = new AudioWorkletNode(ctx, "reply-oscchannel-test-processor");
      node.connect(ctx.destination);

      // Init channel in worklet
      await new Promise((resolve, reject) => {
        const timeout = setTimeout(() => reject(new Error("Worklet init timeout")), 5000);
        node.port.onmessage = (e) => {
          if (e.data.type === MSG.READY) { clearTimeout(timeout); resolve(); }
          else if (e.data.type === "error") { clearTimeout(timeout); reject(new Error(e.data.error)); }
        };
        node.port.postMessage(
          { type: MSG.INIT_CHANNEL, channelData: channel.transferable },
          channel.transferList
        );
      });

      // Start collecting replies
      await new Promise((resolve) => {
        node.port.onmessage = (e) => {
          if (e.data.type === MSG.COLLECTING_STARTED) resolve();
        };
        node.port.postMessage({ type: MSG.START_COLLECTING });
      });

      // Create + free synths to generate /n_end replies
      const SYNTH_COUNT = 20;
      for (let i = 0; i < SYNTH_COUNT; i++) {
        const id = 50000 + i;
        sonic.send("/s_new", "sonic-pi-beep", id, 0, 0,
          "note", 60, "amp", 0.0, "sustain", 0.001, "release", 0.001);
        sonic.send("/n_free", id);
      }

      // Wait for replies to propagate
      await new Promise((r) => setTimeout(r, 2000));

      // Get collected replies
      const replies = await new Promise((resolve) => {
        node.port.onmessage = (e) => {
          if (e.data.type === MSG.RESULTS) resolve(e.data.replies);
        };
        node.port.postMessage({ type: MSG.GET_RESULTS });
      });

      node.disconnect();

      const nEndCount = replies.filter((addr) => addr === "/n_end").length;
      const nGoCount = replies.filter((addr) => addr === "/n_go").length;

      return { totalReplies: replies.length, nEndCount, nGoCount, synthCount: SYNTH_COUNT };
    }, { config: sonicConfig, MSG: REPLY_WORKLET_MSG });

    console.log(
      `Total replies: ${result.totalReplies}, /n_end: ${result.nEndCount}, /n_go: ${result.nGoCount} (${result.synthCount} synths)`
    );

    expect(result.nEndCount, "worklet should receive /n_end via pollReplies()").toBeGreaterThan(0);
    expect(result.nEndCount).toBeGreaterThanOrEqual(result.synthCount * 0.8);
  });
});

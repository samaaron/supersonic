import { test, expect } from "@playwright/test";

const POSTMESSAGE_CONFIG = {
  mode: 'postMessage',
  workerBaseURL: "/dist/workers/",
  wasmBaseURL: "/dist/wasm/",
  sampleBaseURL: "/dist/samples/",
  synthdefBaseURL: "/dist/synthdefs/",
  debug: true,
};

test("debug OSC replies", async ({ page }) => {
  const logs = [];
  page.on("console", (msg) => {
    logs.push("[" + msg.type() + "] " + msg.text());
  });

  await page.goto("/test/harness.html");
  await page.waitForFunction(() => window.supersonicReady === true, { timeout: 10000 });

  const result = await page.evaluate(async (config) => {
    const sonic = new window.SuperSonic(config);

    const allMessages = [];
    sonic.on("message", (msg) => {
      allMessages.push(msg);
      console.log("[TEST] Got message: " + msg.address);
    });

    sonic.on("debug", (msg) => {
      console.log("[TEST] Debug: " + (msg.text || "").slice(0, 100));
    });

    await sonic.init();
    console.log("[TEST] Initialized, mode: " + sonic.mode);

    // Get audio context from node wrapper
    const ctx = sonic.node?.context;
    console.log("[TEST] AudioContext state: " + (ctx ? ctx.state : "no context"));

    // Resume AudioContext if suspended (autoplay policy)
    if (ctx && ctx.state === 'suspended') {
      console.log("[TEST] Resuming AudioContext...");
      await ctx.resume();
      console.log("[TEST] AudioContext resumed, state: " + ctx.state);
    }

    await new Promise(r => setTimeout(r, 200));

    console.log("[TEST] Sending /status...");
    await sonic.send("/status");

    await new Promise(r => setTimeout(r, 2000));

    console.log("[TEST] All messages received: " + allMessages.length);

    return {
      mode: sonic.mode,
      messageCount: allMessages.length,
      messages: allMessages.map(m => m.address),
    };
  }, POSTMESSAGE_CONFIG);

  console.log("Result:", result);
  console.log("Relevant logs:");
  logs.filter(l => l.includes("[TEST]") || l.includes("[OSC") || l.includes("[DEBUG]") || l.includes("error")).forEach(l => console.log(l));
});

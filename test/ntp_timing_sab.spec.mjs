import { test, expect } from "./fixtures.mjs";

/**
 * NTP timing SAB correctness — verifies that the NTP start time written
 * by the main thread is read correctly by the WASM audio thread.
 *
 * The NTP start time is a Float64 in shared memory. Writing a Float64
 * non-atomically to a SharedArrayBuffer risks a torn read on the audio
 * thread. This test verifies the value survives the round trip.
 */
test.describe("NTP timing SAB safety", () => {
  test.beforeEach(async ({ page }) => {
    await page.goto("/test/harness.html");
    await page.waitForFunction(() => window.supersonicReady === true, { timeout: 10000 });
  });

  test("NTP start time survives SAB round trip", async ({ page, sonicConfig }) => {
    // Only meaningful in SAB mode — PM mode uses postMessage (implicit barrier)
    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();

      // Wait for NTP timing to initialize (sets ntp_start_time in shared memory)
      await new Promise(r => setTimeout(r, 1000));

      // Schedule a bundle 200ms in the future — if NTP start time is torn,
      // the C++ scheduler will compute wrong current_ntp, and the bundle
      // will fire at the wrong time (or not at all)
      const NTP_EPOCH_OFFSET = 2208988800;
      function getNTPNow() {
        return (performance.timeOrigin + performance.now()) / 1000 + NTP_EPOCH_OFFSET;
      }

      const targetNtp = getNTPNow() + 0.2;
      const ntpSec = Math.floor(targetNtp);
      const ntpFrac = Math.floor((targetNtp - ntpSec) * 0x100000000);

      // Create a simple synth and schedule a control change
      await sonic.send("/s_new", "sonic-pi-beep", 2000, 1, 0, "note", 60, "out_bus", 0);

      // Build a bundle with scheduled timetag
      const msg = window.SuperSonic.osc.encodeMessage("/n_set", [2000, "gate", 0]);
      const bundle = new Uint8Array(20 + msg.length);
      const view = new DataView(bundle.buffer);
      bundle.set([0x23, 0x62, 0x75, 0x6e, 0x64, 0x6c, 0x65, 0x00]); // #bundle\0
      view.setUint32(8, ntpSec);
      view.setUint32(12, ntpFrac);
      view.setUint32(16, msg.length);
      bundle.set(new Uint8Array(msg), 20);
      sonic.sendOSC(bundle);

      // Wait 500ms — the scheduled gate-off should have fired by now
      await new Promise(r => setTimeout(r, 500));

      const metrics = sonic.getMetrics();
      return {
        mode: config.mode,
        messagesProcessed: metrics?.scsynthMessagesProcessed || 0,
        schedulerDropped: metrics?.scsynthSchedulerDropped || 0,
      };
    }, sonicConfig);

    // Scheduler should not have dropped the bundle
    expect(result.schedulerDropped, "scheduled bundle should not be dropped").toBe(0);
  });
});

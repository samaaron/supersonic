// PM-mode ingress backpressure accounting.
//
// In postMessage mode the worklet writes inbound OSC to the IN ring itself
// (handleMessage → writeOscToRingBuffer). When the ring is full the write
// returns false — the message is gone, and the sender was already told
// "true" at postMessage time. The one place that can still account for the
// loss is the worklet: it must count the drop into scsynthMessagesDropped
// and must NOT count the message as received/sent traffic.
//
// SAB mode already reports backpressure to the sender synchronously
// (OscChannel.send returns false and RING_BUFFER_DIRECT_WRITE_FAILS counts
// it), so this spec covers the postMessage path only.
import { test, expect, skipIfSAB } from "./fixtures.mjs";

test.describe("PM-mode ingress backpressure", () => {
  test.beforeEach(async ({ page }) => {
    page.on("pageerror", (err) => console.error("Page error:", err.message));
    await page.goto("/test/harness.html");
    await page.waitForFunction(() => window.supersonicReady === true, {
      timeout: 10000,
    });
  });

  test("ring-full drops are counted and not reported as received", async ({
    page,
    sonicConfig,
    sonicMode,
  }) => {
    skipIfSAB(sonicMode, "SAB senders get backpressure synchronously; PM path only");

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.sync(1);

      const before = sonic.getMetrics();

      // Flood the IN ring (768 KB) between render quanta: 150 messages of
      // ~128 KB ≈ 25× ring capacity, so the burst cannot drain fast enough
      // and a large fraction must hit the ring-full path.
      const pad = new Uint8Array(128 * 1024);
      const msg = window.SuperSonic.osc.encodeMessage("/supersonic/test_pad", [pad]);
      const SENT = 150;
      for (let i = 0; i < SENT; i++) sonic.sendOSC(msg);

      // Wait for the burst to resolve and a metrics snapshot reflecting it.
      const deadline = Date.now() + 5000;
      let after = sonic.getMetrics();
      while (Date.now() < deadline) {
        after = sonic.getMetrics();
        if (after.scsynthMessagesDropped - before.scsynthMessagesDropped > 0)
          break;
        await new Promise((r) => setTimeout(r, 100));
      }

      return {
        sent: SENT,
        sentDelta: after.oscOutMessagesSent - before.oscOutMessagesSent,
        droppedDelta:
          after.scsynthMessagesDropped - before.scsynthMessagesDropped,
      };
    }, sonicConfig);

    // A 25×-capacity burst must produce drops, and every flooded message
    // must be accounted exactly once: accepted → sent, rejected → dropped.
    expect(result.droppedDelta).toBeGreaterThan(0);
    expect(result.sentDelta).toBeLessThan(result.sent);
    expect(result.sentDelta + result.droppedDelta).toBe(result.sent);
  });
});

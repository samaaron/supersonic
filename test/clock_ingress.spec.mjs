import { test, expect } from "./fixtures.mjs";

/**
 * /clock control API on web — routed through the worklet's IN-drain
 * (consumer-side ingress) into SuperClock. This is the web half of the goal
 * "all builds have access to the clock API": the same /clock/* address space
 * the native engine handles via EngineControl now reaches SuperClock on web too.
 */
test.describe("/clock ingress (web)", () => {
  test("/clock/tempo/set updates the SuperClock tempo", async ({ page, sonicConfig }) => {
    await page.goto("/test/harness.html");
    await page.waitForFunction(() => window.supersonicReady === true, { timeout: 10000 });

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await new Promise((r) => setTimeout(r, 200));

      const before = sonic.getMetrics().clockTempoMbpm;
      await sonic.send("/clock/tempo/set", 137.5);   // 137.5 bpm (non-integer → OSC float)
      await new Promise((r) => setTimeout(r, 400));   // let the audio thread apply + publish
      const after = sonic.getMetrics().clockTempoMbpm;

      return { before, after };
    }, sonicConfig);

    // 137.5 bpm => 137500 milli-BPM, published by SuperClock each block.
    expect(result.after).toBeGreaterThan(135000);
    expect(result.after).toBeLessThan(140000);
  });

  test("/clock/tempo/get round-trips a reply via the OUT ring", async ({ page, sonicConfig }) => {
    await page.goto("/test/harness.html");
    await page.waitForFunction(() => window.supersonicReady === true, { timeout: 10000 });

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await new Promise((r) => setTimeout(r, 200));

      await sonic.send("/clock/tempo/set", 142.5);
      await new Promise((r) => setTimeout(r, 150));

      // The GET verb's reply leaves via the OUT ring and arrives as an "in" event.
      const reply = await new Promise((resolve) => {
        const timer = setTimeout(() => { sonic.off("in", handler); resolve(null); }, 2000);
        const handler = (msg) => {
          if (msg[0] === "/clock/tempo.reply") { clearTimeout(timer); sonic.off("in", handler); resolve(msg); }
        };
        sonic.on("in", handler);
        sonic.send("/clock/tempo/get");
      });

      return { reply };
    }, sonicConfig);

    expect(result.reply).not.toBeNull();
    expect(result.reply[0]).toBe("/clock/tempo.reply");
    expect(result.reply[1]).toBeCloseTo(142.5, 1);
  });

  test("/clock/start_stop_sync set→get round-trips on the web build", async ({ page, sonicConfig }) => {
    await page.goto("/test/harness.html");
    await page.waitForFunction(() => window.supersonicReady === true, { timeout: 10000 });

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await new Promise((r) => setTimeout(r, 200));

      // Send /clock/start_stop_sync/get and resolve its reply value (or null).
      const getSync = () => new Promise((resolve) => {
        const timer = setTimeout(() => { sonic.off("in", handler); resolve(null); }, 2000);
        const handler = (msg) => {
          if (msg[0] === "/clock/start_stop_sync.reply") {
            clearTimeout(timer); sonic.off("in", handler); resolve(msg[1]);
          }
        };
        sonic.on("in", handler);
        sonic.send("/clock/start_stop_sync/get");
      });

      const initial = await getSync();
      await sonic.send("/clock/start_stop_sync/set", 1);
      await new Promise((r) => setTimeout(r, 150));
      const afterEnable = await getSync();
      await sonic.send("/clock/start_stop_sync/set", 0);
      await new Promise((r) => setTimeout(r, 150));
      const afterDisable = await getSync();

      return { initial, afterEnable, afterDisable };
    }, sonicConfig);

    // The web build has no Ableton Link, so start/stop-sync lives in the SAB
    // and the get verb reads that flag back: set→get reflects the value set.
    expect(result.initial).toBe(0);
    expect(result.afterEnable).toBe(1);
    expect(result.afterDisable).toBe(0);
  });

  test("/clock/rpc beat/time conversions answer from the SAB mirror", async ({ page, sonicConfig }) => {
    await page.goto("/test/harness.html");
    await page.waitForFunction(() => window.supersonicReady === true, { timeout: 10000 });

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await new Promise((r) => setTimeout(r, 200));

      const rpc = (addr, replyAddr, ...args) => new Promise((resolve) => {
        const timer = setTimeout(() => { sonic.off("in", handler); resolve(null); }, 2000);
        const handler = (msg) => {
          if (msg[0] === replyAddr) { clearTimeout(timer); sonic.off("in", handler); resolve(msg); }
        };
        sonic.on("in", handler);
        sonic.send(addr, ...args);
      });

      await sonic.send("/clock/tempo/set", 120.5);
      // Anchor beat 0 at "now" like a real client: with the virgin origin (0 =
      // NTP 1900) beats are ~8e9 and the f32 beat argument of time_at_beat
      // quantises to ±512 beats. Anchored, beats stay small and exact.
      sonic.superClock.requestBeatAtTime(0, sonic.superClock.wallNow(), 4);
      await new Promise((r) => setTimeout(r, 150));

      const nowMsg = await rpc("/clock/time/now/get", "/clock/time/now.reply");
      if (!nowMsg) return { failed: "time/now" };
      const tNow = Number(nowMsg[1]);   // NTP-1900 micros

      const beatAt = async (t) => {
        const m = await rpc("/clock/rpc/beat_at_time", "/clock/rpc/beat_at_time.reply",
                            { type: "int64", value: Math.round(t) }, { type: "float", value: 4 });
        return m ? m[1] : null;
      };
      const b0 = await beatAt(tNow);
      const b30 = await beatAt(tNow + 30_000_000);
      if (b0 === null || b30 === null) return { failed: "beat_at_time" };

      const timeAtMsg = await rpc("/clock/rpc/time_at_beat", "/clock/rpc/time_at_beat.reply",
                                  { type: "float", value: b30 }, { type: "float", value: 4 });
      if (!timeAtMsg) return { failed: "time_at_beat" };

      return { tNow, b0, b30, tBack: Number(timeAtMsg[1]) };
    }, sonicConfig);

    expect(result.failed).toBeUndefined();
    // NTP-1900 micros are ~3.99e15 — a garbage 0/offset-only value fails this.
    expect(result.tNow).toBeGreaterThan(3.9e15);
    // 30 s at 120.5 bpm = 60.25 beats; the SAB-mirror math must honour that.
    expect(result.b30 - result.b0).toBeCloseTo(60.25, 1);
    // time_at_beat inverts beat_at_time to within float32-beat resolution
    // (the wire carries beats as f32: ~2^-23 relative ≈ ms-scale here).
    expect(Math.abs(result.tBack - (result.tNow + 30_000_000))).toBeLessThan(50_000);
  });

  test("/clock/transport set→time/get stamps a real NTP time", async ({ page, sonicConfig }) => {
    await page.goto("/test/harness.html");
    await page.waitForFunction(() => window.supersonicReady === true, { timeout: 10000 });

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await new Promise((r) => setTimeout(r, 300));

      const rpc = (addr, replyAddr, ...args) => new Promise((resolve) => {
        const timer = setTimeout(() => { sonic.off("in", handler); resolve(null); }, 2000);
        const handler = (msg) => {
          if (msg[0] === replyAddr) { clearTimeout(timer); sonic.off("in", handler); resolve(msg); }
        };
        sonic.on("in", handler);
        sonic.send(addr, ...args);
      });

      const before = await rpc("/clock/transport/time/get", "/clock/transport/time.reply");
      await sonic.send("/clock/transport/set", 1);
      await new Promise((r) => setTimeout(r, 150));
      const after = await rpc("/clock/transport/time/get", "/clock/transport/time.reply");
      const nowMsg = await rpc("/clock/time/now/get", "/clock/time/now.reply");

      return {
        before: before ? Number(before[1]) : null,
        after: after ? Number(after[1]) : null,
        tNow: nowMsg ? Number(nowMsg[1]) : null,
      };
    }, sonicConfig);

    // No transition yet → the 0 sentinel survives the domain conversion.
    expect(result.before).toBe(0);
    // After a transport/set the stamp is the audio-anchored NTP "now", not the
    // 1900 epoch (the old worklet wallNow()==0 bug) and not Link-domain garbage.
    expect(result.after).toBeGreaterThan(3.9e15);
    expect(Math.abs(result.tNow - result.after)).toBeLessThan(30_000_000);
  });

  test("capabilities report no link/midi on web; native-only verbs are refused", async ({ page, sonicConfig }) => {
    await page.goto("/test/harness.html");
    await page.waitForFunction(() => window.supersonicReady === true, { timeout: 10000 });

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await new Promise((r) => setTimeout(r, 200));

      const rpc = (addr, replyAddr, ...args) => new Promise((resolve) => {
        const timer = setTimeout(() => { sonic.off("in", handler); resolve(null); }, 2000);
        const handler = (msg) => {
          if (msg[0] === replyAddr) { clearTimeout(timer); sonic.off("in", handler); resolve(msg); }
        };
        sonic.on("in", handler);
        sonic.send(addr, ...args);
      });

      const caps = await rpc("/clock/capabilities/get", "/clock/capabilities.reply");
      // /clock/visibility/get is native-only (Link session surface) — the web
      // build must refuse it explicitly rather than dropping it.
      const refused = await rpc("/clock/visibility/get", "/clock/unsupported");

      return { caps, refused };
    }, sonicConfig);

    expect(result.caps).not.toBeNull();
    // Pairs: link 0, link_audio 0, midi 0 on the web build.
    expect(result.caps.slice(1)).toEqual(["link", 0, "link_audio", 0, "midi", 0]);
    expect(result.refused).not.toBeNull();
    expect(result.refused[1]).toBe("/clock/visibility/get");
  });
});

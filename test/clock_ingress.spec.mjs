import { test, expect } from "./fixtures.mjs";

/**
 * /clock control API on web — routed through the worklet's IN-drain
 * (consumer-side ingress) into ClockworkClock. This is the web half of the goal
 * "all builds have access to the clock API": the same /clock/* address space
 * the native engine handles via EngineControl now reaches ClockworkClock on web too.
 */
test.describe("/clock ingress (web)", () => {
  test("/clockwork/clock/tempo/set updates the ClockworkClock tempo", async ({ page, sonicConfig }) => {
    await page.goto("/test/harness.html");
    await page.waitForFunction(() => window.supersonicReady === true, { timeout: 10000 });

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await new Promise((r) => setTimeout(r, 200));

      const before = sonic.getMetrics().clockTempoMbpm;
      await sonic.send("/clockwork/clock/tempo/set", 137.5);   // 137.5 bpm (non-integer → OSC float)
      await new Promise((r) => setTimeout(r, 400));   // let the audio thread apply + publish
      const after = sonic.getMetrics().clockTempoMbpm;

      return { before, after };
    }, sonicConfig);

    // 137.5 bpm => 137500 milli-BPM, published by ClockworkClock each block.
    expect(result.after).toBeGreaterThan(135000);
    expect(result.after).toBeLessThan(140000);
  });

  test("/clockwork/clock/tempo/get round-trips a reply via the OUT ring", async ({ page, sonicConfig }) => {
    await page.goto("/test/harness.html");
    await page.waitForFunction(() => window.supersonicReady === true, { timeout: 10000 });

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await new Promise((r) => setTimeout(r, 200));

      await sonic.send("/clockwork/clock/tempo/set", 142.5);
      await new Promise((r) => setTimeout(r, 150));

      // The GET verb's reply leaves via the OUT ring and arrives as an "in" event.
      const reply = await new Promise((resolve) => {
        const timer = setTimeout(() => { sonic.off("in", handler); resolve(null); }, 2000);
        const handler = (msg) => {
          if (msg[0] === "/clockwork/clock/tempo.reply") { clearTimeout(timer); sonic.off("in", handler); resolve(msg); }
        };
        sonic.on("in", handler);
        sonic.send("/clockwork/clock/tempo/get");
      });

      return { reply };
    }, sonicConfig);

    expect(result.reply).not.toBeNull();
    expect(result.reply[0]).toBe("/clockwork/clock/tempo.reply");
    expect(result.reply[1]).toBeCloseTo(142.5, 1);
  });

  test("/clockwork/clock/start_stop_sync set→get round-trips on the web build", async ({ page, sonicConfig }) => {
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
          if (msg[0] === "/clockwork/clock/start_stop_sync.reply") {
            clearTimeout(timer); sonic.off("in", handler); resolve(msg[1]);
          }
        };
        sonic.on("in", handler);
        sonic.send("/clockwork/clock/start_stop_sync/get");
      });

      const initial = await getSync();
      await sonic.send("/clockwork/clock/start_stop_sync/set", 1);
      await new Promise((r) => setTimeout(r, 150));
      const afterEnable = await getSync();
      await sonic.send("/clockwork/clock/start_stop_sync/set", 0);
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

  test("/clockwork/clock/rpc beat/time conversions answer from the SAB mirror", async ({ page, sonicConfig }) => {
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

      await sonic.send("/clockwork/clock/tempo/set", 120.5);
      // Anchor beat 0 at "now" like a real client: with the virgin origin (0 =
      // NTP 1900) beats are ~8e9 and the f32 beat argument of time_at_beat
      // quantises to ±512 beats. Anchored, beats stay small and exact.
      sonic.clock.requestBeatAtTime(0, sonic.clock.wallNow(), 4);
      await new Promise((r) => setTimeout(r, 150));

      const nowMsg = await rpc("/clockwork/clock/time/now/get", "/clockwork/clock/time/now.reply");
      if (!nowMsg) return { failed: "time/now" };
      const tNow = Number(nowMsg[1]);   // NTP-1900 micros

      const beatAt = async (t) => {
        const m = await rpc("/clockwork/clock/rpc/beat_at_time", "/clockwork/clock/rpc/beat_at_time.reply",
                            { type: "int64", value: Math.round(t) }, { type: "float", value: 4 });
        return m ? m[1] : null;
      };
      const b0 = await beatAt(tNow);
      const b30 = await beatAt(tNow + 30_000_000);
      if (b0 === null || b30 === null) return { failed: "beat_at_time" };

      const timeAtMsg = await rpc("/clockwork/clock/rpc/time_at_beat", "/clockwork/clock/rpc/time_at_beat.reply",
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

  test("/clockwork/clock/transport set→time/get stamps a real NTP time", async ({ page, sonicConfig }) => {
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

      const before = await rpc("/clockwork/clock/transport/time/get", "/clockwork/clock/transport/time.reply");
      await sonic.send("/clockwork/clock/transport/set", 1);
      await new Promise((r) => setTimeout(r, 150));
      const after = await rpc("/clockwork/clock/transport/time/get", "/clockwork/clock/transport/time.reply");
      const nowMsg = await rpc("/clockwork/clock/time/now/get", "/clockwork/clock/time/now.reply");

      return {
        before: before ? Number(before[1]) : null,
        after: after ? Number(after[1]) : null,
        tNow: nowMsg ? Number(nowMsg[1]) : null,
      };
    }, sonicConfig);

    // THE CLOCK IS STAMPED BEFORE ANY TRANSPORT CHANGE, and that is the fix.
    //
    // This asserted `before === 0` — that with no transition yet, the reply
    // carried the 0 sentinel. But is_playing_at_ntp is an NTP-1900 time, and a
    // literal 0 there does not mean "unset", it means midnight on 1 January
    // 1900. Any client doing arithmetic across the reply got an answer 126
    // years early, which is what this test was quietly certifying.
    //
    // ClockworkClock::bindStateToShm now anchors both beat_origin_ntp and
    // is_playing_at_ntp to wall time as the clock enters service (see
    // LinkSession::anchorToWallClockIfUnset), so a real stamp is present from
    // the start. The transport state itself is unchanged — still stopped; what
    // is recorded is WHEN that became true.
    expect(result.before).toBeGreaterThan(3.9e15);
    // After a transport/set the stamp is the audio-anchored NTP "now", not the
    // 1900 epoch and not Link-domain garbage — and never earlier than the
    // anchor it replaces.
    expect(result.after).toBeGreaterThan(3.9e15);
    expect(result.after).toBeGreaterThanOrEqual(result.before);
    expect(Math.abs(result.tNow - result.after)).toBeLessThan(30_000_000);
  });

  test("/clockwork/clock/rpc/beat_phase_now answers time+beat+phase in one round-trip", async ({ page, sonicConfig }) => {
    await page.goto("/test/harness.html");
    await page.waitForFunction(() => window.supersonicReady === true, { timeout: 10000 });

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await new Promise((r) => setTimeout(r, 200));

      const reply = await new Promise((resolve) => {
        const timer = setTimeout(() => { sonic.off("in", handler); resolve(null); }, 2000);
        const handler = (msg) => {
          if (msg[0] === "/clockwork/clock/rpc/beat_phase_now.reply") {
            clearTimeout(timer); sonic.off("in", handler); resolve(msg);
          }
        };
        sonic.on("in", handler);
        sonic.send("/clockwork/clock/rpc/beat_phase_now", { type: "float", value: 4 });
      });
      if (!reply) return { failed: true };
      return { t: Number(reply[1]), beat: reply[2], phase: reply[3] };
    }, sonicConfig);

    expect(result.failed).toBeUndefined();
    expect(result.t).toBeGreaterThan(3.9e15);
    expect(result.phase).toBeGreaterThanOrEqual(0);
    expect(result.phase).toBeLessThan(4);
    const expectPhase = ((result.beat % 4) + 4) % 4;
    expect(result.phase).toBeCloseTo(expectPhase, 5);
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

      const caps = await rpc("/clockwork/clock/capabilities/get", "/clockwork/clock/capabilities.reply");
      // /clock/visibility/get is native-only (Link session surface) — the web
      // build must refuse it explicitly rather than dropping it.
      const refused = await rpc("/clockwork/clock/visibility/get", "/clockwork/clock/unsupported");

      return { caps, refused };
    }, sonicConfig);

    expect(result.caps).not.toBeNull();
    // Pairs: link 0, link_audio 0, midi 0 on the web build.
    expect(result.caps.slice(1)).toEqual(["link", 0, "link_audio", 0, "midi", 0]);
    expect(result.refused).not.toBeNull();
    expect(result.refused[1]).toBe("/clockwork/clock/visibility/get");
  });
});

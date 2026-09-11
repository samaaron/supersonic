/*
 * supersonic_clock_parity.spec.mjs — cross-build parity tests for ClockworkClock.
 *
 * Each scenario has an identically-shaped Catch2 counterpart in
 * clockwork/test/test_clockwork_clock.cpp. Beat math is deterministic IEEE 754
 * and lives in the same ClockworkClock methods on both sides.
 */
import { test, expect } from './fixtures.mjs';

/*
 * How close is "equal" for a beat?
 *
 * DERIVED, not chosen. The clock anchors beat 0 to wall-clock time, so every
 * beat/time conversion passes through an absolute NTP-1900 value of ~3.98e9
 * seconds. A float64 there has a ULP of 2^(31-52) = 4.77e-7 s — call it half a
 * microsecond — and that is the floor on any round trip through absolute time,
 * whatever the arithmetic does.
 *
 * Converting that floor to beats at the tempos used below (120-140 bpm) gives
 * ~1.1e-6 beats. This is an order of magnitude above that, which leaves room
 * for a second rounding without admitting a real error: one audio sample at
 * 48 kHz is 20.8 us, roughly 44x coarser again, so a failure here still means
 * the beat grid is wrong long before anything is audible.
 *
 * These assertions read 1e-12 before the clock was anchored. That value was
 * only ever achievable because beat 0 sat at the NTP epoch, where doubles are
 * dense — it was measuring the bug, not the precision.
 */
const BEAT_EPS = 1e-5;

test.describe('ClockworkClock parity (cross-build)', () => {
  test.beforeEach(async ({ page }) => {
    await page.goto('/test/harness.html');
    await page.waitForFunction(() => window.supersonicReady === true, {
      timeout: 10000,
    });
  });

  test('default state on construction', async ({ page, sonicConfig }) => {
    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      const sc = sonic.clock;
      const out = {
        bpm: sc.getBpm(),
        isPlaying: sc.isPlaying(),
        isLinkEnabled: sc.isLinkEnabled(),
        numPeers: sc.numPeers(),
      };
      await sonic.destroy();
      return out;
    }, sonicConfig);

    expect(result.bpm).toBe(120);
    expect(result.isPlaying).toBe(false);
    expect(result.isLinkEnabled).toBe(false);
    expect(result.numPeers).toBe(0);
  });

  test('setBpm round-trips', async ({ page, sonicConfig }) => {
    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      const sc = sonic.clock;
      sc.setBpm(140.0, 0); const a = sc.getBpm();
      sc.setBpm(60.5,  0); const b = sc.getBpm();
      sc.setBpm(120.0, 0); const c = sc.getBpm();
      await sonic.destroy();
      return { a, b, c };
    }, sonicConfig);
    expect(result.a).toBe(140.0);
    expect(result.b).toBe(60.5);
    expect(result.c).toBe(120.0);
  });

  test('setIsPlaying round-trips', async ({ page, sonicConfig }) => {
    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      const sc = sonic.clock;
      const initial = sc.isPlaying();
      sc.setIsPlaying(true,  1234.5);
      const playing = sc.isPlaying();
      const at = sc.getIsPlayingAtNtp();
      sc.setIsPlaying(false, 0);
      const stopped = sc.isPlaying();
      await sonic.destroy();
      return { initial, playing, at, stopped };
    }, sonicConfig);
    expect(result.initial).toBe(false);
    expect(result.playing).toBe(true);
    expect(result.at).toBe(1234.5);
    expect(result.stopped).toBe(false);
  });

  test('setLinkEnabled is a no-op without a Link backing',
        async ({ page, sonicConfig }) => {
    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      const sc = sonic.clock;
      sc.setLinkEnabled(true);
      const after = sc.isLinkEnabled();
      const peers = sc.numPeers();
      await sonic.destroy();
      return { after, peers };
    }, sonicConfig);
    expect(result.after).toBe(false);
    expect(result.peers).toBe(0);
  });

  test('beatAtTime at non-integer-ratio BPM', async ({ page, sonicConfig }) => {
    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      const sc = sonic.clock;
      sc.setBpm(137.0, 0);
      // MEASURE FROM THE CLOCK'S OWN ORIGIN, not from NTP zero.
      //
      // This used to pass 0.0, 1.5 and 3.0 as absolute NTP seconds and assert
      // beatAtTime(0.0) == 0 — which only holds when beat 0 sits at the NTP
      // epoch, i.e. midnight on 1 January 1900. That was a bug the clock had
      // (beat_origin_ntp left at its 0 initialiser on no-Link builds), and this
      // test was pinning it. The clock now anchors to wall time when it binds
      // into the shared arena, so beatAtTime(0.0) correctly reports ~9.1e9
      // beats — 126 years of them.
      //
      // What the test is actually for is that beats advance linearly at
      // bpm/60 per second, which is a statement about DIFFERENCES and needs no
      // opinion about where the origin is.
      const t0 = sc.timeAtBeat(0.0, 4.0);
      const out = {
        atZero: sc.beatAtTime(t0, 4.0),
        atOneFive: sc.beatAtTime(t0 + 1.5, 4.0),
        atThree: sc.beatAtTime(t0 + 3.0, 4.0),
      };
      await sonic.destroy();
      return out;
    }, sonicConfig);
    expect(Math.abs(result.atZero)).toBeLessThan(BEAT_EPS);
    expect(Math.abs(result.atOneFive - (1.5 * 137.0 / 60.0))).toBeLessThan(BEAT_EPS);
    expect(Math.abs(result.atThree   - (3.0 * 137.0 / 60.0))).toBeLessThan(BEAT_EPS);
  });

  test('timeAtBeat is inverse of beatAtTime', async ({ page, sonicConfig }) => {
    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      const sc = sonic.clock;
      sc.setBpm(140.0, 0);
      const beats = [0.0, 0.5, 1.0, 4.0, 17.25];
      const errs = beats.map(b => Math.abs(sc.beatAtTime(sc.timeAtBeat(b, 4.0), 4.0) - b));
      await sonic.destroy();
      return errs;
    }, sonicConfig);
    // Not 1e-12. A round trip goes through an absolute NTP-1900 time, and a
    // float64 holding ~3.98e9 seconds has a ULP of 0.477 us — so 1e-12 s is
    // over five orders of magnitude below what the representation can express.
    // The old tolerance only passed because the origin was 0, where doubles
    // are dense. See BEAT_EPS.
    for (const err of result) expect(err).toBeLessThan(BEAT_EPS);
  });

  test('phaseAtTime is non-negative and < quantum', async ({ page, sonicConfig }) => {
    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      const sc = sonic.clock;
      sc.setBpm(120.0, 0);
      // Origin-relative, for the same reason as beatAtTime above: phase is a
      // position within the bar measured from beat 0, so it moves with the
      // origin and cannot be asserted against absolute NTP seconds.
      const t0 = sc.timeAtBeat(0.0, 4.0);
      const phases = [0.0, 0.5, 1.0, 2.5, 7.0].map(t => sc.phaseAtTime(t0 + t, 4.0));
      const negPhase = sc.phaseAtTime(t0 - 0.5, 4.0);  // beat = -1.0 → phase = 3.0
      await sonic.destroy();
      return { phases, negPhase };
    }, sonicConfig);
    for (const phase of result.phases) {
      expect(phase).toBeGreaterThanOrEqual(0);
      expect(phase).toBeLessThan(4.0);
    }
    expect(result.negPhase).toBeGreaterThanOrEqual(0);
    expect(result.negPhase).toBeLessThan(4.0);
    expect(Math.abs(result.negPhase - 3.0)).toBeLessThan(BEAT_EPS);
  });

  test('requestBeatAtTime maps beat to time', async ({ page, sonicConfig }) => {
    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      const sc = sonic.clock;
      sc.setBpm(120.0, 0);
      sc.requestBeatAtTime(4.0, 2.0, 4.0);
      const beatAt2 = sc.beatAtTime(2.0, 4.0);
      sc.requestBeatAtTime(0.0, 10.0, 4.0);
      const beatAt10  = sc.beatAtTime(10.0, 4.0);
      const beatAt105 = sc.beatAtTime(10.5, 4.0);
      await sonic.destroy();
      return { beatAt2, beatAt10, beatAt105 };
    }, sonicConfig);
    expect(Math.abs(result.beatAt2   - 4.0)).toBeLessThan(1e-12);
    expect(Math.abs(result.beatAt10  - 0.0)).toBeLessThan(1e-12);
    expect(Math.abs(result.beatAt105 - 1.0)).toBeLessThan(1e-12);
  });

  test('now() returns a sensible NTP time near wallNow()', async ({ page, sonicConfig }) => {
    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      const sc = sonic.clock;
      const t1 = sc.now();
      const wall = sc.wallNow();
      await new Promise(r => setTimeout(r, 100));
      const t2 = sc.now();
      await sonic.destroy();
      return { t1, t2, wall };
    }, sonicConfig);
    // Both should be sensible NTP times (post-1900 epoch, well past 2024).
    expect(result.t1).toBeGreaterThan(3.9e9);
    expect(result.wall).toBeGreaterThan(3.9e9);
    // now() should track wallNow() at boot — audio clock and wall clock
    // start aligned. Tolerance generous to absorb headless-environment
    // throttling and the 100ms sleep between samples.
    expect(Math.abs(result.t1 - result.wall)).toBeLessThan(1.0);
    // now() must advance with elapsed time. In a healthy real-time audio
    // worklet, t2 - t1 ≈ 0.1s after sleep(100). Headless can throttle, so
    // allow [0, 0.3].
    expect(result.t2).toBeGreaterThanOrEqual(result.t1);
    expect(result.t2 - result.t1).toBeLessThan(0.3);
  });

  test('nowAt() applies drift / clock-offset with correct units', async ({ page, sonicConfig }) => {
    // Regression: nowAt previously divided getDriftOffset()/1_000_000
    // treating it as microseconds, but the JS API returns milliseconds.
    // Forces a known drift via setClockOffset (which writes the SAB
    // global_offset in ms) and verifies nowAt reflects it at the right scale.
    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      const sc = sonic.clock;

      // Capture nowAt at a fixed audioCurrentTime with zero offsets first.
      sc.setClockOffset(0);
      const baseline = sc.nowAt(10.0);

      // Inject 200ms clock offset — nowAt should grow by exactly 0.2s.
      sc.setClockOffset(0.2);
      const shifted = sc.nowAt(10.0);

      await sonic.destroy();
      return { baseline, shifted, delta: shifted - baseline };
    }, sonicConfig);

    // Expect delta = 0.2s (200ms clock offset → 0.2s in nowAt). Tolerance
    // is bounded by float64 ULP at the NTP-magnitude (~3.988e9) of the
    // sums inside nowAt — about 5e-7.
    expect(Math.abs(result.delta - 0.2)).toBeLessThan(1e-6);
  });

  test('wallNow() advances with elapsed wall-clock time', async ({ page, sonicConfig }) => {
    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      const sc = sonic.clock;
      const w1 = sc.wallNow();
      await new Promise(r => setTimeout(r, 100));
      const w2 = sc.wallNow();
      await sonic.destroy();
      return { w1, w2 };
    }, sonicConfig);
    // wallNow uses performance.now() which advances at wall-clock rate
    // regardless of audio worklet throttling. After 100ms sleep we expect
    // ~0.1s elapsed, allow [0.08, 0.5] to absorb test scheduling jitter.
    const elapsed = result.w2 - result.w1;
    expect(elapsed).toBeGreaterThan(0.08);
    expect(elapsed).toBeLessThan(0.5);
  });

  // Proves the audio thread observes JS-side setBpm/setIsPlaying. Implicit in
  // SAB mode (shared memory) but only verifiable in PM mode via this OSC
  // round trip. The verb is clockwork's: it answers from the published mirror
  // on the audio thread, which is the same read scsynth's dispatcher would
  // have done.
  test('audio thread observes JS setBpm via /clockwork/clock/state/get',
        async ({ page, sonicConfig }) => {
    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      const replies = [];
      sonic.on('in', (msg) => {
        if (msg[0] === '/clockwork/clock/state.reply') replies.push(msg);
      });
      await sonic.init();

      sonic.clock.setBpm(137.5, 0);
      sonic.clock.setIsPlaying(true, 0);

      // Drain the worklet's message queue and any in-flight OSC before
      // querying. /sync waits for scsynth to reach the synced point, by
      // which time prior postMessage state updates have been applied.
      await sonic.sync(1);
      sonic.send('/clockwork/clock/state/get');
      await sonic.sync(1);

      await sonic.destroy();
      return { replies };
    }, sonicConfig);

    expect(result.replies.length).toBeGreaterThanOrEqual(1);
    const reply = result.replies[result.replies.length - 1];
    // reply = ['/clockwork/clock/state.reply', bpm, isPlaying, beatOriginNtp,
    //          isPlayingAtNtp, flags, meterNum, meterDen]
    expect(reply[1]).toBeCloseTo(137.5, 6);
    expect(reply[2]).toBe(1);
  });

  test('forceBeatAtTime is identical to requestBeatAtTime in session-of-one',
        async ({ page, sonicConfig }) => {
    const result = await page.evaluate(async (config) => {
      const sonicA = new window.SuperSonic(config);
      const sonicB = new window.SuperSonic(config);
      await sonicA.init();
      await sonicB.init();
      sonicA.clock.setBpm(140.0, 0);
      sonicB.clock.setBpm(140.0, 0);
      sonicA.clock.requestBeatAtTime(8.0, 5.0, 4.0);
      sonicB.clock.forceBeatAtTime(8.0, 5.0, 4.0);
      const out = {
        beatA7:  sonicA.clock.beatAtTime(7.0, 4.0),
        beatB7:  sonicB.clock.beatAtTime(7.0, 4.0),
        beatA10: sonicA.clock.beatAtTime(10.0, 4.0),
        beatB10: sonicB.clock.beatAtTime(10.0, 4.0),
      };
      await sonicA.destroy();
      await sonicB.destroy();
      return out;
    }, sonicConfig);
    expect(result.beatA7).toBe(result.beatB7);
    expect(result.beatA10).toBe(result.beatB10);
  });
});

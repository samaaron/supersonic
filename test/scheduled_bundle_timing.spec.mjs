import { test, expect, skipIfPostMessage } from './fixtures.mjs';

/**
 * Behavioural test for the web scheduling path.
 *
 * A timestamped OSC bundle is scheduled by the engine's EngineScheduler against
 * SuperClock, where
 *
 *     engine_now_ntp = audioContextTime + ntp_start + drift + global   (the worklet TimeSource)
 *
 * The hazard: nowAt() silently falls back to `ntp_start = 0.0` when the SAB
 * NTP-start field isn't published yet. With ntp_start = 0, the engine's clock
 * sits ~1.7e9 s away from the performance.now()-NTP domain that a client stamps
 * bundle timetags in, so a *scheduled* (non-immediate) bundle would be queued
 * far-future and eff_never fire — while *immediate* bundles (timetag 0/1) bypass
 * the comparison and still work. That asymmetry is exactly what makes the app
 * "boot fine" while timestamped scheduling is silently broken.
 *
 * We observe firing via the scheduler queue depth (public getMetrics()):
 *   - peak/seen depth ≥ 1  ⇒ the bundle was *queued* (not immediate-bypassed)
 *   - depth returns to 0    ⇒ the bundle *fired*; the time of that transition is
 *                             when it fired, relative to send.
 *
 * SAB-only: postMessage mode delivers metrics via periodic snapshots, too coarse
 * to time a sub-second transition.
 */
test.describe('Scheduled bundle timing (web engine scheduler)', () => {
  test.setTimeout(30000);

  test.beforeEach(async ({ page }) => {
    page.on('console', (msg) => {
      if (msg.type() === 'error') console.error('Browser console error:', msg.text());
    });
    await page.goto('/test/harness.html');
    await page.waitForFunction(() => window.supersonicReady === true, { timeout: 10000 });
  });

  // Drive a future-timetagged bundle and watch the scheduler queue depth to
  // learn (a) whether it was queued, and (b) when it fired, relative to send.
  // `settleMs` lets the caller probe the pre-sync window (0) vs the stabilised
  // clock (>0, after the 1s drift timer has run).
  const probeFn = async ({ config, N, settleMs }) => {
    const NTP_EPOCH_OFFSET = 2208988800;
    const nowNtp = () =>
      (performance.timeOrigin + performance.now()) / 1000 + NTP_EPOCH_OFFSET;

    const sonic = new window.SuperSonic(config);
    await sonic.init();
    await sonic.sync(); // engine is up and draining
    if (settleMs > 0) await new Promise((r) => setTimeout(r, settleMs));

    const driftAtSend = sonic.getMetrics().driftOffsetMs;
    const peakBefore = sonic.getMetrics().scsynthSchedulerPeakDepth;

    const t0 = performance.now();
    const timetag = nowNtp() + N / 1000;
    const bundle = window.SuperSonic.osc.encodeBundle(timetag, [
      ['/sync', Math.floor(Math.random() * 2_000_000_000)],
    ]);
    sonic.sendOSC(bundle);

    // Poll the queue depth: catch the queued state, then the fire (→0).
    let queuedSeen = false;
    let firedDeltaMs = null;
    const deadline = performance.now() + N + 3000;
    while (performance.now() < deadline) {
      const depth = sonic.getMetrics().scsynthSchedulerDepth;
      if (depth >= 1) queuedSeen = true;
      if (queuedSeen && depth === 0) {
        firedDeltaMs = performance.now() - t0;
        break;
      }
      await new Promise((r) => setTimeout(r, 4));
    }

    const peakAfter = sonic.getMetrics().scsynthSchedulerPeakDepth;
    await sonic.destroy();
    return {
      N,
      driftAtSend,
      queuedSeen,
      peakRose: peakAfter > peakBefore,
      firedDeltaMs,
    };
  };

  test('a future bundle is queued and fires at ~N ms (stabilised clock)', async ({
    page,
    sonicConfig,
    sonicMode,
  }) => {
    skipIfPostMessage(sonicMode, 'Needs live SAB metrics to time the fire');

    const N = 300;
    const r = await page.evaluate(probeFn, { config: sonicConfig, N, settleMs: 1500 });
    console.log('stabilised:', JSON.stringify(r));

    // It went through the scheduler (was NOT executed immediately).
    expect(r.queuedSeen, 'bundle should enter the scheduler queue').toBe(true);
    // It actually fired (not stuck far-future / never).
    expect(r.firedDeltaMs, 'bundle should fire within the deadline').not.toBeNull();
    // It did not fire immediately…
    expect(r.firedDeltaMs).toBeGreaterThan(N * 0.6);
    // …and it fired close to its scheduled time (allow scheduler/poll slack).
    expect(r.firedDeltaMs).toBeLessThan(N + 200);
  });

  test('pre-sync window: a future bundle still fires (ntp_start published before bundles)', async ({
    page,
    sonicConfig,
    sonicMode,
  }) => {
    skipIfPostMessage(sonicMode, 'Needs live SAB metrics to time the fire');

    // Send immediately after init(), before the 1s drift timer has run. If
    // ntp_start were unpublished (nowAt's 0.0 fallback), the engine clock would
    // be ~1.7e9 s off the timetag domain and the bundle would never fire —
    // immediate playback would still work, hiding the break. So the decisive
    // assertion here is "it fires at all"; timing tolerance is loose because
    // drift hasn't been corrected yet.
    const N = 400;
    const r = await page.evaluate(probeFn, { config: sonicConfig, N, settleMs: 0 });
    console.log('pre-sync:', JSON.stringify(r));

    expect(r.queuedSeen, 'bundle should enter the scheduler queue').toBe(true);
    // The crucial probe: a scheduled bundle must fire even pre-drift-sync.
    expect(
      r.firedDeltaMs,
      'pre-sync scheduled bundle never fired — ntp_start epoch hazard',
    ).not.toBeNull();
    expect(r.firedDeltaMs).toBeGreaterThan(N * 0.4); // not collapsed to immediate
    expect(r.firedDeltaMs).toBeLessThan(N + 800);    // generous: drift uncorrected
  });
});

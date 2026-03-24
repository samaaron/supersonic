import { test, expect } from './fixtures.mjs';

/**
 * WASM AudioWorklet Performance Benchmark
 *
 * Boots the engine in Chromium, creates sustained synths at increasing counts,
 * and monitors audio health metrics to detect when the worklet starts struggling.
 *
 * Run with:
 *   npx playwright test test/wasm_benchmark.spec.mjs --project=SAB --reporter=line
 *
 * This test is designed to compare against the native benchmark
 * (test/native/test_benchmark.cpp) to quantify the WASM overhead.
 */

test.describe('WASM AudioWorklet Benchmark', () => {
  test.beforeEach(async ({ page }) => {
    await page.goto('/test/harness.html');
    await page.waitForFunction(() => window.supersonicReady === true, {
      timeout: 10000,
    });
  });

  test('scaling: beep synths with health monitoring', async ({ page, sonicConfig, sonicMode }) => {
    test.setTimeout(120_000);

    const results = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDefs(["sonic-pi-beep"]);

      // Wait for engine to settle
      await new Promise(r => setTimeout(r, 500));

      const counts = [0, 1, 5, 10, 25, 50, 75, 100];
      const rows = [];

      for (const count of counts) {
        // Create synths with long sustain
        for (let i = 0; i < count; i++) {
          await sonic.send("/s_new", "sonic-pi-beep", 5000 + i, 0, 0,
            "note", 60 + (i % 24), "sustain", 3600, "amp", 0.0);
        }

        // Let it run for 3 seconds to gather stable health readings
        await new Promise(r => setTimeout(r, 3000));

        // Read metrics
        const metrics = sonic.getMetrics();
        const report = sonic.getSystemReport();

        rows.push({
          count,
          healthPct: metrics.audioHealthPct,
          glitchCount: metrics.glitchCount ?? -1,
          glitchDurationMs: metrics.glitchDurationMs ?? -1,
          avgLatencyUs: metrics.averageLatencyUs ?? -1,
          maxLatencyUs: metrics.maxLatencyUs ?? -1,
          processCount: metrics.scsynthProcessCount,
          schedulerLates: metrics.scsynthSchedulerLates,
          hasPlaybackStats: metrics.hasPlaybackStats === 1,
          healthIssues: report.health?.issues?.length ?? 0,
          healthSummary: report.health?.summary ?? '',
        });

        // Free synths before next round
        for (let i = 0; i < count; i++) {
          await sonic.send("/n_free", 5000 + i);
        }
        await sonic.sync(count + 100);

        // Brief pause between rounds
        await new Promise(r => setTimeout(r, 500));
      }

      await sonic.destroy();
      return rows;
    }, sonicConfig);

    // Print results table
    console.log(`\n  ╔══════════════════════════════════════════════════════════════════╗`);
    console.log(`  ║  WASM BENCHMARK — ${sonicMode.toUpperCase()} mode                                  ║`);
    console.log(`  ║  128 samples/block @ 48kHz = 2,667 us budget                    ║`);
    console.log(`  ╚══════════════════════════════════════════════════════════════════╝\n`);

    const hasStats = results.some(r => r.hasPlaybackStats);
    console.log(`  playbackStats available: ${hasStats}`);
    console.log('');

    console.log('  synths  health%  glitches  glitchMs  avgLatUs  maxLatUs  lates  issues');
    console.log('  ─────  ───────  ────────  ────────  ────────  ────────  ─────  ──────');
    for (const r of results) {
      const g = r.glitchCount === -1 ? '   n/a' : String(r.glitchCount).padStart(6);
      const gd = r.glitchDurationMs === -1 ? '   n/a' : String(r.glitchDurationMs).padStart(6);
      const al = r.avgLatencyUs === -1 ? '   n/a' : String(r.avgLatencyUs).padStart(6);
      const ml = r.maxLatencyUs === -1 ? '   n/a' : String(r.maxLatencyUs).padStart(6);
      console.log(
        `  ${String(r.count).padStart(5)}  ${String(r.healthPct).padStart(7)}  ${g}  ${gd}  ${al}  ${ml}  ${String(r.schedulerLates).padStart(5)}  ${String(r.healthIssues).padStart(6)}`
      );
    }
    console.log('');

    // The idle case should be healthy
    const idle = results.find(r => r.count === 0);
    expect(idle.healthPct).toBeGreaterThanOrEqual(95);

    // Log any degradation
    for (const r of results) {
      if (r.healthPct < 95) {
        console.log(`  ⚠ health dropped to ${r.healthPct}% at ${r.count} synths`);
      }
    }
  });

  test('sustained load: 10 beep synths for 10 seconds', async ({ page, sonicConfig, sonicMode }) => {
    test.setTimeout(60_000);

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDefs(["sonic-pi-beep"]);

      // Create 10 sustained synths
      for (let i = 0; i < 10; i++) {
        await sonic.send("/s_new", "sonic-pi-beep", 5000 + i, 0, 0,
          "note", 60 + i, "sustain", 3600, "amp", 0.0);
      }

      // Sample health every second for 10 seconds
      const samples = [];
      for (let s = 0; s < 10; s++) {
        await new Promise(r => setTimeout(r, 1000));
        const m = sonic.getMetrics();
        samples.push({
          second: s + 1,
          healthPct: m.audioHealthPct,
          glitchCount: m.glitchCount ?? -1,
          processCount: m.scsynthProcessCount,
        });
      }

      // Free synths
      for (let i = 0; i < 10; i++) {
        await sonic.send("/n_free", 5000 + i);
      }
      await sonic.destroy();

      return samples;
    }, sonicConfig);

    console.log(`\n  --- Sustained load: 10x beep for 10s (${sonicMode}) ---`);
    console.log('  sec  health%  glitches  processCount');
    console.log('  ───  ───────  ────────  ────────────');
    for (const s of result) {
      const g = s.glitchCount === -1 ? '   n/a' : String(s.glitchCount).padStart(6);
      console.log(
        `  ${String(s.second).padStart(3)}  ${String(s.healthPct).padStart(7)}  ${g}  ${String(s.processCount).padStart(12)}`
      );
    }

    // Health should stay above 95% for 10 synths
    const minHealth = Math.min(...result.map(s => s.healthPct));
    console.log(`\n  min health: ${minHealth}%`);
    expect(minHealth).toBeGreaterThanOrEqual(90);
  });

  test('prophet stress: find the breaking point', async ({ page, sonicConfig, sonicMode }) => {
    test.setTimeout(120_000);

    const results = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDefs(["sonic-pi-prophet"]);

      const counts = [1, 3, 5, 7, 10];
      const rows = [];

      for (const count of counts) {
        for (let i = 0; i < count; i++) {
          await sonic.send("/s_new", "sonic-pi-prophet", 6000 + i, 0, 0,
            "note", 48 + i * 7, "sustain", 3600, "amp", 0.0);
        }

        await new Promise(r => setTimeout(r, 3000));

        const metrics = sonic.getMetrics();
        rows.push({
          count,
          healthPct: metrics.audioHealthPct,
          glitchCount: metrics.glitchCount ?? -1,
          avgLatencyUs: metrics.averageLatencyUs ?? -1,
          maxLatencyUs: metrics.maxLatencyUs ?? -1,
        });

        for (let i = 0; i < count; i++) {
          await sonic.send("/n_free", 6000 + i);
        }
        await sonic.sync(count + 200);
        await new Promise(r => setTimeout(r, 500));
      }

      await sonic.destroy();
      return rows;
    }, sonicConfig);

    console.log(`\n  --- Prophet stress test (${sonicMode}) ---`);
    console.log('  synths  health%  glitches  avgLatUs  maxLatUs');
    console.log('  ─────  ───────  ────────  ────────  ────────');
    for (const r of results) {
      const g = r.glitchCount === -1 ? '   n/a' : String(r.glitchCount).padStart(6);
      const al = r.avgLatencyUs === -1 ? '   n/a' : String(r.avgLatencyUs).padStart(6);
      const ml = r.maxLatencyUs === -1 ? '   n/a' : String(r.maxLatencyUs).padStart(6);
      console.log(
        `  ${String(r.count).padStart(5)}  ${String(r.healthPct).padStart(7)}  ${g}  ${al}  ${ml}`
      );
    }

    // 1 prophet should be fine
    expect(results[0].healthPct).toBeGreaterThanOrEqual(90);
  });
});

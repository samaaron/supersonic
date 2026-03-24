import { test, expect } from './fixtures.mjs';

/**
 * WASM AudioWorklet Benchmark — Audible synths with real audio output.
 *
 * Unlike the silent benchmark (amp=0), this uses audible synths to exercise
 * the full audio pipeline including output buffer writes and device I/O.
 * Monitors audio health by sampling AudioContext.currentTime progression
 * at high frequency to detect individual block overruns.
 *
 * Run with system Chromium (real audio):
 *   DISPLAY=:0 npx playwright test test/wasm_benchmark_audible.spec.mjs \
 *     --project=SAB --config=playwright.benchmark.mjs --reporter=line
 */

test.describe('WASM Audible Benchmark', () => {
  test.beforeEach(async ({ page }) => {
    await page.goto('/test/harness.html');
    await page.waitForFunction(() => window.supersonicReady === true, {
      timeout: 10000,
    });
  });

  test('audible scaling with fine-grained timing', async ({ page, sonicConfig, sonicMode }) => {
    test.setTimeout(180_000);

    const results = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDefs(["sonic-pi-beep", "sonic-pi-prophet"]);

      // Wait for engine to settle
      await new Promise(r => setTimeout(r, 1000));

      const ctx = sonic.audioContext;
      const sampleRate = ctx.sampleRate;
      const blockSize = 128;
      const blockDurationS = blockSize / sampleRate;

      // Fine-grained health check: sample currentTime rapidly to detect stalls
      function measureHealth(durationMs) {
        return new Promise(resolve => {
          const samples = [];
          const startWall = performance.now();
          const startAudio = ctx.currentTime;
          let prevAudio = startAudio;
          let prevWall = startWall;
          let stallCount = 0;
          let maxGapMs = 0;

          const interval = setInterval(() => {
            const nowWall = performance.now();
            const nowAudio = ctx.currentTime;

            const wallDelta = nowWall - prevWall;
            const audioDelta = nowAudio - prevAudio;

            // A "stall" is when audio time didn't advance as much as wall time
            // Allow 20% tolerance for timer jitter
            if (wallDelta > 10 && audioDelta < wallDelta / 1000 * 0.8) {
              stallCount++;
              const gapMs = (wallDelta / 1000 - audioDelta) * 1000;
              if (gapMs > maxGapMs) maxGapMs = gapMs;
            }

            prevAudio = nowAudio;
            prevWall = nowWall;
          }, 5); // Sample every 5ms

          setTimeout(() => {
            clearInterval(interval);
            const totalWall = (performance.now() - startWall) / 1000;
            const totalAudio = ctx.currentTime - startAudio;
            const healthPct = Math.min(100, Math.round((totalAudio / totalWall) * 100));
            resolve({ healthPct, stallCount, maxGapMs: Math.round(maxGapMs), sampleRate });
          }, durationMs);
        });
      }

      const rows = [];

      // Test beep synths at increasing counts (audible at low volume)
      const beepCounts = [0, 1, 10, 25, 50, 75, 100];
      for (const count of beepCounts) {
        for (let i = 0; i < count; i++) {
          await sonic.send("/s_new", "sonic-pi-beep", 5000 + i, 0, 0,
            "note", 60 + (i % 24), "sustain", 3600, "amp", 0.01);
        }

        // Let synths start, then measure for 3 seconds
        await new Promise(r => setTimeout(r, 200));
        const health = await measureHealth(3000);
        const metrics = sonic.getMetrics();

        rows.push({
          type: 'beep',
          count,
          ...health,
          processCount: metrics.scsynthProcessCount,
          schedulerLates: metrics.scsynthSchedulerLates,
          glitchCount: metrics.glitchCount ?? -1,
        });

        // Free synths
        for (let i = 0; i < count; i++) {
          await sonic.send("/n_free", 5000 + i);
        }
        await sonic.sync(count + 100);
        await new Promise(r => setTimeout(r, 300));
      }

      // Test prophet synths (much heavier)
      const prophetCounts = [1, 3, 5, 7, 10, 15];
      for (const count of prophetCounts) {
        for (let i = 0; i < count; i++) {
          await sonic.send("/s_new", "sonic-pi-prophet", 7000 + i, 0, 0,
            "note", 48 + i * 7, "sustain", 3600, "amp", 0.01);
        }

        await new Promise(r => setTimeout(r, 200));
        const health = await measureHealth(3000);
        const metrics = sonic.getMetrics();

        rows.push({
          type: 'prophet',
          count,
          ...health,
          processCount: metrics.scsynthProcessCount,
          schedulerLates: metrics.scsynthSchedulerLates,
          glitchCount: metrics.glitchCount ?? -1,
        });

        for (let i = 0; i < count; i++) {
          await sonic.send("/n_free", 7000 + i);
        }
        await sonic.sync(count + 200);
        await new Promise(r => setTimeout(r, 300));
      }

      await sonic.destroy();
      return { rows, sampleRate };
    }, sonicConfig);

    console.log(`\n  ╔══════════════════════════════════════════════════════════════════╗`);
    console.log(`  ║  WASM AUDIBLE BENCHMARK — ${sonicMode.toUpperCase()} mode @ ${results.sampleRate}Hz             ║`);
    console.log(`  ║  128 samples/block = ${(128 / results.sampleRate * 1e6).toFixed(0)} us budget                            ║`);
    console.log(`  ╚══════════════════════════════════════════════════════════════════╝\n`);

    console.log('  type      synths  health%  stalls  maxGapMs  glitches  lates');
    console.log('  ───────  ──────  ───────  ──────  ────────  ────────  ─────');
    for (const r of results.rows) {
      const g = r.glitchCount === -1 ? '   n/a' : String(r.glitchCount).padStart(6);
      console.log(
        `  ${r.type.padEnd(7)}  ${String(r.count).padStart(6)}  ${String(r.healthPct).padStart(7)}  ${String(r.stallCount).padStart(6)}  ${String(r.maxGapMs).padStart(8)}  ${g}  ${String(r.schedulerLates).padStart(5)}`
      );
    }
    console.log('');

    // Find the breaking point
    const broken = results.rows.find(r => r.healthPct < 95);
    if (broken) {
      console.log(`  ⚠ Health dropped below 95% at ${broken.count}x ${broken.type} (${broken.healthPct}%)`);
    } else {
      console.log(`  ✓ All workloads maintained ≥95% health`);
    }
  });
});

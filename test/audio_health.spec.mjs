import { test, expect } from './fixtures.mjs';

test.describe('Audio Health Diagnostics', () => {
  test.beforeEach(async ({ page }) => {
    await page.goto('/test/harness.html');
    await page.waitForFunction(() => window.supersonicReady === true, {
      timeout: 10000,
    });
  });

  test('schema includes audio health metrics in AudioWorklet panel', async ({ page }) => {
    const result = await page.evaluate(() => {
      const schema = window.SuperSonic.getMetricsSchema();
      const panel = schema.layout.panels.find(p => p.title === 'AudioWorklet');
      if (!panel) return { found: false };

      const keys = [];
      for (const row of panel.rows) {
        for (const cell of (row.cells || [])) {
          if (cell.key) keys.push(cell.key);
        }
      }

      // Check all health keys exist in the panel
      const healthKeys = ['audioHealthPct', 'glitchCount', 'glitchDurationMs', 'averageLatencyUs', 'maxLatencyUs'];
      const missing = healthKeys.filter(k => !keys.includes(k));

      return { found: true, keys, missing };
    });

    expect(result.found).toBe(true);
    expect(result.missing).toEqual([]);
  });

  test('audio health metric offsets 59-63 are defined in schema', async ({ page }) => {
    const result = await page.evaluate(() => {
      const schema = window.SuperSonic.getMetricsSchema();
      return {
        glitchCount: schema.metrics.glitchCount?.offset,
        glitchDurationMs: schema.metrics.glitchDurationMs?.offset,
        averageLatencyUs: schema.metrics.averageLatencyUs?.offset,
        maxLatencyUs: schema.metrics.maxLatencyUs?.offset,
        audioHealthPct: schema.metrics.audioHealthPct?.offset,
      };
    });

    expect(result.glitchCount).toBe(59);
    expect(result.glitchDurationMs).toBe(60);
    expect(result.averageLatencyUs).toBe(61);
    expect(result.maxLatencyUs).toBe(62);
    expect(result.audioHealthPct).toBe(63);
  });

  test('audioHealthPct is populated in metrics after boot', async ({ page, sonicConfig }) => {
    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();

      // Wait for audio to run and health monitor to take a reading
      await new Promise(r => setTimeout(r, 500));

      const metrics = sonic.getMetrics();
      const arr = sonic.getMetricsArray();

      await sonic.destroy();

      return {
        objectValue: metrics.audioHealthPct,
        arrayValue: arr[63],
      };
    }, sonicConfig);

    // Health should be between 0-100 and likely close to 100 on a healthy system
    expect(result.objectValue).toBeGreaterThanOrEqual(0);
    expect(result.objectValue).toBeLessThanOrEqual(100);
    expect(result.arrayValue).toBeGreaterThanOrEqual(0);
    expect(result.arrayValue).toBeLessThanOrEqual(100);
  });

  test('glitchCount is 0 on clean boot', async ({ page, sonicConfig }) => {
    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();

      await new Promise(r => setTimeout(r, 200));
      const metrics = sonic.getMetrics();

      await sonic.destroy();

      return {
        glitchCount: metrics.glitchCount,
        glitchDurationMs: metrics.glitchDurationMs,
      };
    }, sonicConfig);

    // On a clean boot with no load, there should be no glitches
    // These may be undefined on browsers without playbackStats support
    if (result.glitchCount !== undefined) {
      expect(result.glitchCount).toBe(0);
    }
    if (result.glitchDurationMs !== undefined) {
      expect(result.glitchDurationMs).toBe(0);
    }
  });

  test('getSystemReport() returns valid structure', async ({ page, sonicConfig }) => {
    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();

      await new Promise(r => setTimeout(r, 300));
      const report = sonic.getSystemReport();

      await sonic.destroy();

      return {
        hasTimestamp: typeof report.timestamp === 'string',
        hasSystem: typeof report.system === 'object' && report.system !== null,
        hasAudio: typeof report.audio === 'object' && report.audio !== null,
        hasEngine: typeof report.engine === 'object' && report.engine !== null,
        hasHealth: typeof report.health === 'object' && report.health !== null,
        hasMetrics: typeof report.metrics === 'object' && report.metrics !== null,
        // playbackStats may be null on non-Chrome
        playbackStatsIsObjectOrNull: report.playbackStats === null || typeof report.playbackStats === 'object',

        // System fields
        hasUserAgent: typeof report.system.userAgent === 'string',
        hasPlatform: typeof report.system.platform === 'string',

        // Audio fields
        hasSampleRate: typeof report.audio.sampleRate === 'number',
        hasState: typeof report.audio.state === 'string',

        // Engine fields
        hasMode: typeof report.engine.mode === 'string',

        // Health fields
        hasHealthPct: typeof report.health.audioHealthPct === 'number',
        hasIssues: Array.isArray(report.health.issues),
        hasSummary: typeof report.health.summary === 'string',
        healthPct: report.health.audioHealthPct,
        issueCount: report.health.issues.length,
        summary: report.health.summary,
      };
    }, sonicConfig);

    expect(result.hasTimestamp).toBe(true);
    expect(result.hasSystem).toBe(true);
    expect(result.hasAudio).toBe(true);
    expect(result.hasEngine).toBe(true);
    expect(result.hasHealth).toBe(true);
    expect(result.hasMetrics).toBe(true);
    expect(result.playbackStatsIsObjectOrNull).toBe(true);
    expect(result.hasUserAgent).toBe(true);
    expect(result.hasPlatform).toBe(true);
    expect(result.hasSampleRate).toBe(true);
    expect(result.hasState).toBe(true);
    expect(result.hasMode).toBe(true);
    expect(result.hasHealthPct).toBe(true);
    expect(result.hasIssues).toBe(true);
    expect(result.hasSummary).toBe(true);
    expect(result.healthPct).toBeGreaterThanOrEqual(0);
    expect(result.healthPct).toBeLessThanOrEqual(100);
  });

  test('getInfo() capabilities includes playbackStats', async ({ page, sonicConfig }) => {
    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();

      const info = sonic.getInfo();
      await sonic.destroy();

      return {
        hasPlaybackStats: 'playbackStats' in info.capabilities,
        playbackStatsType: typeof info.capabilities.playbackStats,
      };
    }, sonicConfig);

    expect(result.hasPlaybackStats).toBe(true);
    expect(result.playbackStatsType).toBe('boolean');
  });
});

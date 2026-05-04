// SPDX-License-Identifier: MIT OR GPL-3.0-or-later
// Copyright (c) 2025 Sam Aaron

/**
 * Coverage for prescheduler metrics that don't have dedicated tests
 * elsewhere: preschedulerPendingPeak, preschedulerMessagesRetried,
 * preschedulerMaxLateMs.
 *
 * Each is mirrored on the native side (src/workers/Prescheduler.cpp) and
 * tested there in test/native/test_metrics.cpp. This spec verifies the
 * web-side equivalents.
 */

import { test, expect } from './fixtures.mjs';

const NTP_EPOCH_OFFSET = 2208988800;

test.describe('Prescheduler metric coverage', () => {

  test('preschedulerPendingPeak grows with concurrent FAR_FUTURE schedules',
       async ({ page, sonicConfig }) => {
    await page.goto('/test/harness.html');
    await page.waitForFunction(() => window.supersonicReady === true, { timeout: 10000 });

    const result = await page.evaluate(async ({ config, NTP_EPOCH_OFFSET }) => {
      // Inline helpers (mirrors prescheduler_demand_dispatch.spec.mjs).
      const getCurrentNTP = () => {
        const perfTimeMs = performance.timeOrigin + performance.now();
        return (perfTimeMs / 1000) + NTP_EPOCH_OFFSET;
      };
      const createTimedBundle = (ntpTime, nodeId) => {
        const enc = window.SuperSonic.osc.encodeMessage('/s_new',
          ['sonic-pi-beep', nodeId, 0, 0, 'amp', 0.0, 'release', 0.01]);
        const bundle = new Uint8Array(8 + 8 + 4 + enc.byteLength);
        const view = new DataView(bundle.buffer);
        bundle.set([0x23, 0x62, 0x75, 0x6e, 0x64, 0x6c, 0x65, 0x00], 0);
        const sec = Math.floor(ntpTime);
        const frac = Math.floor((ntpTime % 1) * 0x100000000);
        view.setUint32(8, sec, false);
        view.setUint32(12, frac, false);
        view.setInt32(16, enc.byteLength, false);
        bundle.set(enc, 20);
        return bundle;
      };

      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.loadSynthDef('sonic-pi-beep');
      await sonic.sync(1);

      const before = sonic.getMetrics().preschedulerPendingPeak;
      const farNTP = getCurrentNTP() + 60.0;
      const N = 5;
      for (let i = 0; i < N; i++) {
        sonic.sendOSC(createTimedBundle(farNTP + i, 90000 + i));
      }

      // Give the prescheduler worker a moment to register the schedules.
      await new Promise(r => setTimeout(r, 100));

      const after = sonic.getMetrics().preschedulerPendingPeak;
      return { before, after, N };
    }, { config: sonicConfig, NTP_EPOCH_OFFSET });

    expect(result.after).toBeGreaterThanOrEqual(result.before + result.N);
  });

  test('preschedulerMessagesRetried is readable and starts at 0',
       async ({ page, sonicConfig }) => {
    await page.goto('/test/harness.html');
    await page.waitForFunction(() => window.supersonicReady === true, { timeout: 10000 });

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      return sonic.getMetrics().preschedulerMessagesRetried;
    }, sonicConfig);

    expect(typeof result).toBe('number');
    expect(result).toBe(0);
  });

  test('preschedulerMaxLateMs is readable and starts at 0',
       async ({ page, sonicConfig }) => {
    await page.goto('/test/harness.html');
    await page.waitForFunction(() => window.supersonicReady === true, { timeout: 10000 });

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      return sonic.getMetrics().preschedulerMaxLateMs;
    }, sonicConfig);

    expect(typeof result).toBe('number');
    expect(result).toBe(0);
  });

});

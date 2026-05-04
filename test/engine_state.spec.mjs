// SPDX-License-Identifier: MIT OR GPL-3.0-or-later
// Copyright (c) 2025 Sam Aaron

/**
 * Engine state observation tests.
 *
 * Validates the symmetric `isRunning()` and `getEngineState()` API that
 * mirrors the native SupersonicEngine accessors of the same name.
 */

import { test, expect } from './fixtures.mjs';

test.describe('Engine state observation', () => {

  test('isRunning() and getEngineState() before init', async ({ page, sonicConfig }) => {
    await page.goto('/test/harness.html');
    await page.waitForFunction(() => window.supersonicReady === true, { timeout: 10000 });

    const result = await page.evaluate((config) => {
      const sonic = new window.SuperSonic(config);
      return {
        isRunning: sonic.isRunning(),
        engineState: sonic.getEngineState(),
      };
    }, sonicConfig);

    expect(result.isRunning).toBe(false);
    expect(result.engineState).toBe('stopped');
  });

  test('isRunning() and getEngineState() after init', async ({ page, sonicConfig }) => {
    await page.goto('/test/harness.html');
    await page.waitForFunction(() => window.supersonicReady === true, { timeout: 10000 });

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      return {
        isRunning: sonic.isRunning(),
        engineState: sonic.getEngineState(),
      };
    }, sonicConfig);

    expect(result.isRunning).toBe(true);
    expect(result.engineState).toBe('running');
  });

  test('isRunning() and getEngineState() after shutdown', async ({ page, sonicConfig }) => {
    await page.goto('/test/harness.html');
    await page.waitForFunction(() => window.supersonicReady === true, { timeout: 10000 });

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();
      await sonic.shutdown();
      return {
        isRunning: sonic.isRunning(),
        engineState: sonic.getEngineState(),
      };
    }, sonicConfig);

    expect(result.isRunning).toBe(false);
    expect(result.engineState).toBe('stopped');
  });

  test('getEngineState() reports booting during init', async ({ page, sonicConfig }) => {
    await page.goto('/test/harness.html');
    await page.waitForFunction(() => window.supersonicReady === true, { timeout: 10000 });

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      const initPromise = sonic.init();
      // Synchronously after starting init: state should reflect that boot is in flight.
      const duringInit = {
        isRunning: sonic.isRunning(),
        engineState: sonic.getEngineState(),
      };
      await initPromise;
      return duringInit;
    }, sonicConfig);

    expect(result.isRunning).toBe(false);
    expect(result.engineState).toBe('booting');
  });

  test('isRunning() matches the existing initialized getter', async ({ page, sonicConfig }) => {
    await page.goto('/test/harness.html');
    await page.waitForFunction(() => window.supersonicReady === true, { timeout: 10000 });

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      const beforeInit = { isRunning: sonic.isRunning(), initialized: sonic.initialized };
      await sonic.init();
      const afterInit = { isRunning: sonic.isRunning(), initialized: sonic.initialized };
      return { beforeInit, afterInit };
    }, sonicConfig);

    expect(result.beforeInit.isRunning).toBe(result.beforeInit.initialized);
    expect(result.afterInit.isRunning).toBe(result.afterInit.initialized);
  });

});

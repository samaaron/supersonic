// SPDX-License-Identifier: MIT OR GPL-3.0-or-later
// Copyright (c) 2025 Sam Aaron

/**
 * Buffer Pool Growth Tests
 *
 * Validates that the buffer pool grows on demand when the initial pool
 * is exhausted, in both SAB and postMessage modes.
 */

import { test, expect } from './fixtures.mjs';

test.describe('Buffer pool growth', () => {

  test('pool grows when initial allocation is exhausted', async ({ page, sonicConfig }) => {
    await page.goto('/test/harness.html');
    await page.waitForFunction(() => window.supersonicReady === true, { timeout: 10000 });

    const result = await page.evaluate(async (config) => {
      // Use a tiny 1MB initial pool to force growth quickly
      const sonic = new window.SuperSonic({
        ...config,
        memory: { bufferPoolSize: 1 * 1024 * 1024 },
        maxBufferMemory: 64 * 1024 * 1024,
        bufferGrowIncrement: 2 * 1024 * 1024,
      });

      const growthEvents = [];
      sonic.on('buffer:pool:grown', (info) => growthEvents.push(info));

      await sonic.init();

      // Load several large samples to exceed the 1MB initial pool.
      // Each decoded stereo FLAC at 48kHz is much larger than the compressed file.
      const samples = [
        'ambi_haunted_hum.flac',
        'ambi_lunar_land.flac',
        'ambi_sauna.flac',
      ];
      for (let i = 0; i < samples.length; i++) {
        await sonic.loadSample(i, samples[i]);
      }

      // Verify all buffers are queryable
      const queryResults = [];
      sonic.on('in', (msg) => { if (msg[0] === '/b_info') queryResults.push(msg); });
      for (let i = 0; i < samples.length; i++) {
        await sonic.send('/b_query', i);
      }
      await sonic.sync();

      const metrics = sonic.getMetrics();

      await sonic.shutdown();

      return {
        samplesLoaded: samples.length,
        buffersQueried: queryResults.length,
        allHaveFrames: queryResults.every(m => m[2] > 0),
        growthEvents: growthEvents.length,
        growthDetails: growthEvents,
        poolCount: metrics.bufferPoolPoolCount,
        growthCount: metrics.bufferPoolGrowthCount,
        totalCapacity: metrics.bufferPoolTotalCapacity,
        maxCapacity: metrics.bufferPoolMaxCapacity,
        usedBytes: metrics.bufferPoolUsedBytes,
      };
    }, sonicConfig);

    // All samples loaded and queryable
    expect(result.buffersQueried).toBe(result.samplesLoaded);
    expect(result.allHaveFrames).toBe(true);

    // Growth must have occurred (1MB initial pool can't hold 3 large decoded samples)
    expect(result.growthCount).toBeGreaterThan(0);
    expect(result.poolCount).toBeGreaterThan(1);
    expect(result.growthEvents).toBeGreaterThan(0);

    // Capacity grew beyond the initial 1MB
    expect(result.totalCapacity).toBeGreaterThan(1 * 1024 * 1024);

    // Max capacity matches config
    expect(result.maxCapacity).toBe(64 * 1024 * 1024);

    // Pool is actually being used
    expect(result.usedBytes).toBeGreaterThan(0);
  });

  test('pool growth event includes correct metadata', async ({ page, sonicConfig }) => {
    await page.goto('/test/harness.html');
    await page.waitForFunction(() => window.supersonicReady === true, { timeout: 10000 });

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic({
        ...config,
        memory: { bufferPoolSize: 512 * 1024 },  // 512KB — very small
        maxBufferMemory: 32 * 1024 * 1024,
        bufferGrowIncrement: 2 * 1024 * 1024,
      });

      const growthEvents = [];
      sonic.on('buffer:pool:grown', (info) => growthEvents.push(info));

      await sonic.init();

      // Load multiple samples to guarantee exceeding 512KB
      await sonic.loadSample(0, 'ambi_sauna.flac');
      await sonic.loadSample(1, 'ambi_haunted_hum.flac');

      const metrics = sonic.getMetrics();
      await sonic.shutdown();

      return { growthEvents, growthCount: metrics.bufferPoolGrowthCount };
    }, sonicConfig);

    expect(result.growthCount).toBeGreaterThan(0);
    expect(result.growthEvents.length).toBeGreaterThan(0);

    const first = result.growthEvents[0];
    expect(first.poolIndex).toBe(1);
    expect(first.newBytes).toBeGreaterThan(0);
    expect(first.totalCapacity).toBeGreaterThan(512 * 1024);
  });

  test('allocation fails gracefully when max capacity reached', async ({ page, sonicConfig }) => {
    await page.goto('/test/harness.html');
    await page.waitForFunction(() => window.supersonicReady === true, { timeout: 10000 });

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic({
        ...config,
        memory: { bufferPoolSize: 512 * 1024 },   // 512KB initial
        maxBufferMemory: 4 * 1024 * 1024,          // 4MB max — fits 1-2 samples then fails
        bufferGrowIncrement: 2 * 1024 * 1024,
      });

      await sonic.init();

      // Try to load samples until we exceed the max
      const loaded = [];
      const errors = [];
      const samples = [
        'ambi_haunted_hum.flac',
        'ambi_lunar_land.flac',
        'ambi_sauna.flac',
        'arovane_beat_a.flac',
        'arovane_beat_b.flac',
      ];

      for (let i = 0; i < samples.length; i++) {
        try {
          await sonic.loadSample(i, samples[i]);
          loaded.push(samples[i]);
        } catch (e) {
          errors.push(e.message);
        }
      }

      await sonic.shutdown();

      return { loadedCount: loaded.length, errorCount: errors.length, hasAllocationError: errors.some(e => e.includes('allocation failed')) };
    }, sonicConfig);

    // Some should load, some should fail
    expect(result.loadedCount).toBeGreaterThan(0);
    expect(result.errorCount).toBeGreaterThan(0);
    expect(result.hasAllocationError).toBe(true);
  });

  test('buffers loaded after growth are queryable with correct metadata', async ({ page, sonicConfig }) => {
    await page.goto('/test/harness.html');
    await page.waitForFunction(() => window.supersonicReady === true, { timeout: 10000 });

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic({
        ...config,
        memory: { bufferPoolSize: 512 * 1024 },
        maxBufferMemory: 64 * 1024 * 1024,
        bufferGrowIncrement: 2 * 1024 * 1024,
      });

      await sonic.init();

      // Load enough samples to trigger growth
      await sonic.loadSample(0, 'ambi_haunted_hum.flac');
      await sonic.loadSample(1, 'ambi_sauna.flac');

      // Query both — if growth corrupted pointers, scsynth would return wrong data
      const messages = [];
      sonic.on('in', (msg) => { if (msg[0] === '/b_info') messages.push(msg); });
      await sonic.send('/b_query', 0);
      await sonic.send('/b_query', 1);
      await sonic.sync();

      const metrics = sonic.getMetrics();
      await sonic.shutdown();

      return {
        bufferInfos: messages,
        growthCount: metrics.bufferPoolGrowthCount,
        poolCount: metrics.bufferPoolPoolCount,
      };
    }, sonicConfig);

    // Growth occurred
    expect(result.growthCount).toBeGreaterThan(0);
    // Both buffers queryable with valid frame counts
    expect(result.bufferInfos.length).toBe(2);
    expect(result.bufferInfos[0][2]).toBeGreaterThan(0);  // frames > 0
    expect(result.bufferInfos[1][2]).toBeGreaterThan(0);
  });

  test('freeing buffers reclaims space in grown pools', async ({ page, sonicConfig }) => {
    await page.goto('/test/harness.html');
    await page.waitForFunction(() => window.supersonicReady === true, { timeout: 10000 });

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic({
        ...config,
        memory: { bufferPoolSize: 512 * 1024 },
        maxBufferMemory: 64 * 1024 * 1024,
        bufferGrowIncrement: 2 * 1024 * 1024,
      });

      await sonic.init();

      // Load samples to trigger growth
      await sonic.loadSample(0, 'ambi_sauna.flac');
      await sonic.loadSample(1, 'ambi_haunted_hum.flac');

      const metricsAfterLoad = sonic.getMetrics();
      const usedAfterLoad = metricsAfterLoad.bufferPoolUsedBytes;

      // Free both buffers
      await sonic.send('/b_free', 0);
      await sonic.send('/b_free', 1);
      await sonic.sync();

      // Wait for free to propagate
      await new Promise(r => setTimeout(r, 100));

      const metricsAfterFree = sonic.getMetrics();
      const usedAfterFree = metricsAfterFree.bufferPoolUsedBytes;

      // Reallocate into the freed space
      await sonic.loadSample(2, 'ambi_choir.flac');

      await sonic.shutdown();

      return {
        usedAfterLoad,
        usedAfterFree,
        freedMemory: usedAfterLoad > usedAfterFree,
        reloadSucceeded: true,
      };
    }, sonicConfig);

    expect(result.usedAfterLoad).toBeGreaterThan(0);
    expect(result.freedMemory).toBe(true);
    expect(result.reloadSucceeded).toBe(true);
  });

});

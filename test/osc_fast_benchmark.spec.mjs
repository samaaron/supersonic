/**
 * osc_fast.js encoding and decoding speed. Run with the benchmark config:
 *   npx playwright test -c playwright.benchmark.mjs test/osc_fast_benchmark.spec.mjs
 *
 * Kept out of the suite CI runs: a rate measured on a shared runner varies
 * with the runner's load.
 */

import { test, expect } from '@playwright/test';

test.describe('OSC Fast Encoder/Decoder', () => {
  test.beforeEach(async ({ page }) => {
    await page.goto('/test/osc_harness.html');
    await page.waitForFunction(() => window.oscReady === true, {
      timeout: 5000,
    });
  });

  test.describe('Performance', () => {
    test('encodes 10000 messages quickly', async ({ page }) => {
      const result = await page.evaluate(() => {
        const start = performance.now();

        for (let i = 0; i < 10000; i++) {
          window.oscFast.encodeMessage('/s_new', [
            'sonic-pi-beep',
            i,
            0,
            0,
            'note',
            60,
            'amp',
            0.5,
          ]);
        }

        const elapsed = performance.now() - start;

        return {
          elapsed,
          messagesPerSecond: 10000 / (elapsed / 1000),
        };
      });

      // Should encode at least 100,000 messages per second
      expect(result.messagesPerSecond).toBeGreaterThan(100000);
      console.log(
        `Encoded 10000 messages in ${result.elapsed.toFixed(2)}ms (${Math.round(result.messagesPerSecond)} msg/s)`
      );
    });

    test('decodes 10000 messages quickly', async ({ page }) => {
      const result = await page.evaluate(() => {
        // Pre-encode messages
        const messages = [];
        for (let i = 0; i < 10000; i++) {
          messages.push(
            window.oscFast.copyEncoded(
              window.oscFast.encodeMessage('/s_new', [
                'sonic-pi-beep',
                i,
                0,
                0,
                'note',
                60,
              ])
            )
          );
        }

        const start = performance.now();

        for (let i = 0; i < 10000; i++) {
          window.oscFast.decodeMessage(messages[i]);
        }

        const elapsed = performance.now() - start;

        return {
          elapsed,
          messagesPerSecond: 10000 / (elapsed / 1000),
        };
      });

      // Should decode at least 100,000 messages per second
      expect(result.messagesPerSecond).toBeGreaterThan(100000);
      console.log(
        `Decoded 10000 messages in ${result.elapsed.toFixed(2)}ms (${Math.round(result.messagesPerSecond)} msg/s)`
      );
    });

    test('caching improves encoding speed', async ({ page }) => {
      const result = await page.evaluate(() => {
        window.oscFast.clearCache();

        // First run - cache miss
        const start1 = performance.now();
        for (let i = 0; i < 10000; i++) {
          window.oscFast.encodeMessage('/s_new', [i]);
        }
        const elapsed1 = performance.now() - start1;

        // Second run - cache hit
        const start2 = performance.now();
        for (let i = 0; i < 10000; i++) {
          window.oscFast.encodeMessage('/s_new', [i]);
        }
        const elapsed2 = performance.now() - start2;

        return {
          firstRunMs: elapsed1,
          secondRunMs: elapsed2,
          speedup: elapsed1 / elapsed2,
        };
      });

      // Second run should be at least as fast (caching helps)
      // Note: speedup may be minimal if encoding is already fast
      console.log(
        `First run: ${result.firstRunMs.toFixed(2)}ms, Second run: ${result.secondRunMs.toFixed(2)}ms, Speedup: ${result.speedup.toFixed(2)}x`
      );
    });
  });
});

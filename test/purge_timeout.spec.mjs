/**
 * purge() must not hang when the worklet is gone.
 *
 * purge() flushes the WASM scheduler + IN ring by posting a `clearSched` to the
 * worklet and awaiting its `clearSchedAck`. The worklet acks synchronously from
 * its message handler, so a live worklet (even a suspended one) replies almost
 * instantly. If the worklet's global scope was reclaimed (a long-backgrounded
 * mobile tab — the exact case recover() exists for), no ack ever arrives.
 *
 * Without a timeout the await never settles, so resume() — which calls purge()
 * first — wedges, and recover() never reaches its reload() fallback. These tests
 * pin the bounded-wait behaviour by simulating the dead worklet at the transport
 * level: dropping the `clearSched` postMessage means the ack can never come,
 * exactly as purge() would observe with a dead worklet.
 */
import { test, expect } from './fixtures.mjs';

test.describe('purge() ack timeout', () => {
  test('resolves on a bounded timeout when no ack arrives (dead worklet)', async ({ page, sonicConfig }) => {
    test.setTimeout(30000);
    await page.goto('/test/harness.html');
    await page.waitForFunction(() => window.supersonicReady === true, { timeout: 10000 });

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();

      // Simulate a dead worklet: the clearSched never reaches it, so it never acks.
      const origPost = MessagePort.prototype.postMessage;
      MessagePort.prototype.postMessage = function (msg, ...rest) {
        if (msg && msg.type === 'clearSched') return;
        return origPost.call(this, msg, ...rest);
      };

      const start = performance.now();
      let hung = false;
      try {
        await Promise.race([
          sonic.purge(),
          new Promise((_, reject) => setTimeout(() => reject(new Error('HUNG')), 8000)),
        ]);
      } catch (e) {
        if (e.message === 'HUNG') hung = true;
        else throw e;
      } finally {
        MessagePort.prototype.postMessage = origPost;
      }
      return { hung, ms: Math.round(performance.now() - start) };
    }, sonicConfig);

    expect(result.hung).toBe(false);          // must not hang forever
    expect(result.ms).toBeGreaterThan(500);   // the timeout path actually fired
    expect(result.ms).toBeLessThan(4000);     // ...and resolved promptly after it
  });

  test('still resolves promptly when the worklet acks normally', async ({ page, sonicConfig }) => {
    test.setTimeout(30000);
    await page.goto('/test/harness.html');
    await page.waitForFunction(() => window.supersonicReady === true, { timeout: 10000 });

    const result = await page.evaluate(async (config) => {
      const sonic = new window.SuperSonic(config);
      await sonic.init();

      // Positive control: a healthy worklet acks synchronously, so purge() must
      // resolve well before the dead-worklet timeout — the fix must not make the
      // common path wait.
      const start = performance.now();
      await sonic.purge();
      return { ms: Math.round(performance.now() - start) };
    }, sonicConfig);

    expect(result.ms).toBeLessThan(500);
  });
});

import { test, expect } from "./fixtures.mjs";

/**
 * Latency Benchmark
 *
 * Measures round-trip latency by sending /status and timing /status.reply.
 * Runs in both SAB and postMessage modes via the standard fixture pattern.
 */

async function measureLatency(page, config, iterations = 100) {
  return await page.evaluate(async ({ config, iterations }) => {
    const sonic = new window.SuperSonic(config);
    await sonic.init();

    // Helper to wait for message with timeout (fail fast, don't hang)
    function waitForMessage(address, timeoutMs = 5000) {
      return new Promise((resolve, reject) => {
        const timer = setTimeout(() => {
          sonic.off("message", handler);
          reject(new Error(`Timeout waiting for ${address} after ${timeoutMs}ms`));
        }, timeoutMs);
        const handler = (msg) => {
          if (msg.address === address) {
            clearTimeout(timer);
            sonic.off("message", handler);
            resolve(msg);
          }
        };
        sonic.on("message", handler);
      });
    }

    // Warm up - first few messages often slower
    for (let i = 0; i < 5; i++) {
      sonic.send("/status");
      await waitForMessage("/status.reply");
    }

    // Measure round-trip latencies
    const latencies = [];

    for (let i = 0; i < iterations; i++) {
      const start = performance.now();

      sonic.send("/status");
      await waitForMessage("/status.reply");

      const elapsed = performance.now() - start;
      latencies.push(elapsed);
    }

    await sonic.destroy();

    // Calculate statistics
    latencies.sort((a, b) => a - b);
    const min = latencies[0];
    const max = latencies[latencies.length - 1];
    const sum = latencies.reduce((a, b) => a + b, 0);
    const mean = sum / latencies.length;
    const median = latencies[Math.floor(latencies.length / 2)];
    const p95 = latencies[Math.floor(latencies.length * 0.95)];
    const p99 = latencies[Math.floor(latencies.length * 0.99)];

    // Calculate standard deviation
    const squaredDiffs = latencies.map(x => Math.pow(x - mean, 2));
    const avgSquaredDiff = squaredDiffs.reduce((a, b) => a + b, 0) / latencies.length;
    const stdDev = Math.sqrt(avgSquaredDiff);

    return {
      mode: config.mode,
      iterations,
      min: min.toFixed(3),
      max: max.toFixed(3),
      mean: mean.toFixed(3),
      median: median.toFixed(3),
      p95: p95.toFixed(3),
      p99: p99.toFixed(3),
      stdDev: stdDev.toFixed(3),
      raw: latencies.slice(0, 20).map(l => l.toFixed(2)), // First 20 samples
    };
  }, { config, iterations });
}

test.describe("Latency Benchmark", () => {
  test.beforeEach(async ({ page }) => {
    await page.goto("/test/harness.html");
    await page.waitForFunction(() => window.supersonicReady === true, {
      timeout: 10000,
    });
  });

  test("round-trip latency", async ({ page, sonicConfig, sonicMode }) => {
    const results = await measureLatency(page, sonicConfig, 100);

    console.log(`\n=== ${sonicMode.toUpperCase()} Mode Latency ===`);
    console.log(`  Min:    ${results.min} ms`);
    console.log(`  Max:    ${results.max} ms`);
    console.log(`  Mean:   ${results.mean} ms`);
    console.log(`  Median: ${results.median} ms`);
    console.log(`  P95:    ${results.p95} ms`);
    console.log(`  P99:    ${results.p99} ms`);
    console.log(`  StdDev: ${results.stdDev} ms`);
    console.log(`  First samples: [${results.raw.join(", ")}]`);

    expect(parseFloat(results.mean)).toBeLessThan(50); // Sanity check
  });
});

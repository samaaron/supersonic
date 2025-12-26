import { test, expect } from "@playwright/test";

/**
 * Latency Benchmark: SAB vs postMessage
 *
 * Measures round-trip latency by sending /status and timing /status.reply
 */

const SAB_CONFIG = {
  mode: 'sab',
  workerBaseURL: "/dist/workers/",
  wasmBaseURL: "/dist/wasm/",
  sampleBaseURL: "/dist/samples/",
  synthdefBaseURL: "/dist/synthdefs/",
};

const POSTMESSAGE_CONFIG = {
  mode: 'postMessage',
  workerBaseURL: "/dist/workers/",
  wasmBaseURL: "/dist/wasm/",
  sampleBaseURL: "/dist/samples/",
  synthdefBaseURL: "/dist/synthdefs/",
};

async function measureLatency(page, config, iterations = 100) {
  return await page.evaluate(async ({ config, iterations }) => {
    const sonic = new window.SuperSonic(config);
    await sonic.init();

    // Warm up - first few messages often slower
    for (let i = 0; i < 5; i++) {
      await new Promise((resolve) => {
        const handler = (msg) => {
          if (msg.address === "/status.reply") {
            sonic.off("message", handler);
            resolve();
          }
        };
        sonic.on("message", handler);
        sonic.send("/status");
      });
    }

    // Measure round-trip latencies
    const latencies = [];

    for (let i = 0; i < iterations; i++) {
      const start = performance.now();

      await new Promise((resolve) => {
        const handler = (msg) => {
          if (msg.address === "/status.reply") {
            sonic.off("message", handler);
            resolve();
          }
        };
        sonic.on("message", handler);
        sonic.send("/status");
      });

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

  test("SAB mode round-trip latency", async ({ page }) => {
    // Skip if crossOriginIsolated is not available
    const isCOI = await page.evaluate(() => crossOriginIsolated);
    if (!isCOI) {
      test.skip();
      return;
    }

    const results = await measureLatency(page, SAB_CONFIG, 100);

    console.log("\n=== SAB Mode Latency ===");
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

  test("postMessage mode round-trip latency", async ({ page }) => {
    const results = await measureLatency(page, POSTMESSAGE_CONFIG, 100);

    console.log("\n=== postMessage Mode Latency ===");
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

  test("compare SAB vs postMessage latency", async ({ page }) => {
    // Skip if crossOriginIsolated is not available
    const isCOI = await page.evaluate(() => crossOriginIsolated);
    if (!isCOI) {
      console.log("Skipping comparison - crossOriginIsolated not available");
      test.skip();
      return;
    }

    const sabResults = await measureLatency(page, SAB_CONFIG, 50);

    // Need to reload page between tests to get clean state
    await page.goto("/test/harness.html");
    await page.waitForFunction(() => window.supersonicReady === true, {
      timeout: 10000,
    });

    const pmResults = await measureLatency(page, POSTMESSAGE_CONFIG, 50);

    console.log("\n╔══════════════════════════════════════════════════════════╗");
    console.log("║          SAB vs postMessage Latency Comparison           ║");
    console.log("╠═══════════════╦═══════════════════╦══════════════════════╣");
    console.log("║    Metric     ║    SAB Mode       ║   postMessage Mode   ║");
    console.log("╠═══════════════╬═══════════════════╬══════════════════════╣");
    console.log(`║ Min           ║ ${sabResults.min.padStart(10)} ms   ║ ${pmResults.min.padStart(13)} ms   ║`);
    console.log(`║ Max           ║ ${sabResults.max.padStart(10)} ms   ║ ${pmResults.max.padStart(13)} ms   ║`);
    console.log(`║ Mean          ║ ${sabResults.mean.padStart(10)} ms   ║ ${pmResults.mean.padStart(13)} ms   ║`);
    console.log(`║ Median        ║ ${sabResults.median.padStart(10)} ms   ║ ${pmResults.median.padStart(13)} ms   ║`);
    console.log(`║ P95           ║ ${sabResults.p95.padStart(10)} ms   ║ ${pmResults.p95.padStart(13)} ms   ║`);
    console.log(`║ P99           ║ ${sabResults.p99.padStart(10)} ms   ║ ${pmResults.p99.padStart(13)} ms   ║`);
    console.log(`║ StdDev        ║ ${sabResults.stdDev.padStart(10)} ms   ║ ${pmResults.stdDev.padStart(13)} ms   ║`);
    console.log("╚═══════════════╩═══════════════════╩══════════════════════╝");

    const diff = parseFloat(pmResults.mean) - parseFloat(sabResults.mean);
    const ratio = parseFloat(pmResults.mean) / parseFloat(sabResults.mean);
    console.log(`\n  Difference: postMessage is ${diff.toFixed(2)}ms slower (${ratio.toFixed(2)}x)`);

    // Both should be reasonably fast
    expect(parseFloat(sabResults.mean)).toBeLessThan(50);
    expect(parseFloat(pmResults.mean)).toBeLessThan(50);
  });
});

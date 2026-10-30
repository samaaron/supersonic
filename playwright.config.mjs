import { defineConfig } from "@playwright/test";

export default defineConfig({
  testDir: "./test",
  testMatch: "**/*.spec.mjs",
  // Benchmarks measure the machine, which a run with every worker busy is
  // not a measure of: they run alone, under playwright.benchmark.mjs
  // (npm run test:benchmark), as the native suite keeps [benchmark] apart.
  testIgnore: "**/*benchmark*.spec.mjs",
  timeout: 30000, // 30s default - reduced for faster feedback
  retries: 0,
  workers: '100%', // Use all available CPUs

  use: {
    baseURL: "http://localhost:8003",
    headless: true,
    // Chrome flags for audio in headless mode
    launchOptions: {
      args: [
        "--use-fake-ui-for-media-stream",
        "--use-fake-device-for-media-stream",
        // No audio hardware at all: Chromium renders every output stream
        // into a fake sink driven by a timer, so a test page never opens the
        // machine's sound card (one run opened a developer's 198 times and
        // clicked it on every start, 2026-10-16). Timing is the real
        // clock's, as a device's would be.
        "--disable-audio-output",
        "--autoplay-policy=no-user-gesture-required",
      ],
    },
  },

  // Run tests in both SAB and postMessage modes
  projects: [
    {
      name: "SAB",
      use: {
        browserName: "chromium",
        supersonicMode: "sab",
      },
    },
    {
      name: "postMessage",
      use: {
        browserName: "chromium",
        supersonicMode: "postMessage",
      },
    },
  ],

  // The exported demo (build/site) is booted by the suite; produce it first.
  globalSetup: "./test/global-setup.mjs",

  // Start the test server before running tests
  webServer: {
    command: "node test/server.mjs",
    port: 8003,
    reuseExistingServer: false,
  },
});

import { defineConfig } from "@playwright/test";

export default defineConfig({
  testDir: "./test",
  testMatch: "**/*.spec.mjs",
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

  // Start the test server before running tests
  webServer: {
    command: "node test/server.mjs",
    port: 8003,
    reuseExistingServer: !process.env.CI,
  },
});

import { defineConfig } from "@playwright/test";

export default defineConfig({
  testDir: "./test",
  testMatch: "**/*.spec.mjs",
  timeout: 60000, // 60s - WASM loading can be slow
  retries: 0,
  workers: 1, // Run tests serially - they share the audio context

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

  projects: [
    {
      name: "chromium",
      use: {
        browserName: "chromium",
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

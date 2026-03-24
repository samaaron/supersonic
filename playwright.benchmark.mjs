import { defineConfig } from "@playwright/test";

export default defineConfig({
  testDir: "./test",
  testMatch: "**/*.spec.mjs",
  timeout: 300000,
  retries: 0,
  workers: 1,

  use: {
    baseURL: "http://localhost:8003",
    headless: false,
    launchOptions: {
      args: [
        "--autoplay-policy=no-user-gesture-required",
      ],
    },
  },

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

  webServer: {
    command: "node test/server.mjs",
    port: 8003,
    reuseExistingServer: true,
  },
});

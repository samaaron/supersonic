// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Sam Aaron
//
// getInfo().version: the engine's version, as the worklet reports it from the
// module. It was always null — the client waited for a "version" message no
// worklet ever sent.
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { test, expect } from "./fixtures.mjs";

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const { version } = JSON.parse(fs.readFileSync(path.join(ROOT, "package.json"), "utf8"));

test("getInfo().version is the engine's version, and stays so across a reset", async ({ page, sonicConfig }) => {
  await page.goto("/test/harness.html");
  await page.waitForFunction(() => window.supersonicReady === true, { timeout: 10000 });
  const seen = await page.evaluate(async (config) => {
    const sonic = new window.SuperSonic(config);
    await sonic.init();
    const before = sonic.getInfo().version;
    await sonic.reset();
    const after = sonic.getInfo().version;
    await sonic.destroy();
    return { before, after };
  }, sonicConfig);
  expect(seen.before).toBe(version);
  expect(seen.after).toBe(version);
});

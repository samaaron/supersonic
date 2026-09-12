// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Sam Aaron
//
// The engine's wasm comes from the core package (coreBaseURL), not from
// beside the client (baseURL): the client package ships no wasm, so a CDN
// boot configured the documented way — baseURL for the client and workers,
// coreBaseURL for the engine — has exactly one place the wasm can be.
// 0.81.0 derived the wasm URL from baseURL and ignored coreBaseURL, and
// every CDN boot 404'd at init().
import { test, expect } from "./fixtures.mjs";

test("the engine wasm is fetched from coreBaseURL, not from beside the client", async ({ sonicPage, sonicConfig }) => {
  const wasmRequests = [];
  sonicPage.on("request", (r) => { if (/scsynth-nrt\.wasm/.test(r.url())) wasmRequests.push(new URL(r.url()).pathname); });
  const config = { ...sonicConfig, coreBaseURL: "/packages/supersonic-scsynth-core/" };
  delete config.wasmBaseURL;   // the fixture names it explicitly; here the core directory must be enough
  delete config.wasmUrl;
  await sonicPage.evaluate(async (config) => {
    const sonic = new window.SuperSonic(config);
    await sonic.init();
    await sonic.destroy();
  }, config);
  expect(wasmRequests.length, "the wasm was fetched").toBeGreaterThan(0);
  for (const p of wasmRequests)
    expect(p, "wasm fetched from the core package's directory").toBe("/packages/supersonic-scsynth-core/wasm/scsynth-nrt.wasm");
});

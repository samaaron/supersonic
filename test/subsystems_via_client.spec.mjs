// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Sam Aaron
//
// MIDI and gamepad reached the way a page reaches them: through the
// SuperSonic client with `midi: true` / `gamepad: true`, which fetches the
// subsystems' wasm from wasmBaseURL — the core package's wasm/ directory on
// a CDN, /dist/wasm/ here. The other MIDI and gamepad specs drive the managers
// from clockwork's own dist and never touch that path; 0.80.0 shipped with
// the two wasm files missing from the core package and nothing noticed.
import { test, expect } from "./fixtures.mjs";

test("midi: true and gamepad: true boot through the client, fetching their wasm from wasmBaseURL", async ({ sonicPage, sonicConfig, context }) => {
  // Headless Chromium refuses Web MIDI unless the context grants it; the
  // permission is the page's business, the fetch of the wasm is ours.
  await context.grantPermissions(["midi", "midi-sysex"]);
  const errors = [];
  sonicPage.on("pageerror", (e) => errors.push(String(e)));
  sonicPage.on("response", (r) => { if (r.status() >= 400 && /_bg\.wasm/.test(r.url())) errors.push(`${r.status()} ${r.url()}`); });
  const r = await sonicPage.evaluate(async (config) => {
    const sonic = new window.SuperSonic({ ...config, midi: true, gamepad: true });
    await sonic.init();
    const out = { midi: !!sonic.midi, gamepad: !!sonic.gamepad };
    await sonic.destroy();
    return out;
  }, sonicConfig);
  expect(errors, "subsystem wasm missing or page errors").toEqual([]);
  expect(r.midi, "the MIDI manager came up").toBe(true);
  expect(r.gamepad, "the gamepad manager came up").toBe(true);
});

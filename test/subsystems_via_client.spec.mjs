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

// Boot with both subsystems on; collect any 4xx on a subsystem wasm and any
// page error; report what came up and why anything did not.
async function bootWith(sonicPage, sonicConfig, subsystems) {
  const errors = [];
  sonicPage.on("pageerror", (e) => errors.push(String(e)));
  sonicPage.on("response", (r) => { if (r.status() >= 400 && /_bg\.wasm/.test(r.url())) errors.push(`${r.status()} ${r.url()}`); });
  const r = await sonicPage.evaluate(async ([config, subsystems]) => {
    const sonic = new window.SuperSonic({ ...config, ...subsystems });
    await sonic.init();
    const out = {
      midi: !!sonic.midi, midiError: sonic.midiError ? String(sonic.midiError) : null,
      gamepad: !!sonic.gamepad, gamepadError: sonic.gamepadError ? String(sonic.gamepadError) : null,
    };
    await sonic.destroy();
    return out;
  }, [sonicConfig, subsystems]);
  return { errors, ...r };
}

test("midi: true and gamepad: true boot through the client, fetching their wasm from wasmBaseURL", async ({ sonicPage, sonicConfig, context }) => {
  // Headless Chromium refuses Web MIDI unless the context grants it; the
  // permission is the page's business, the fetch of the wasm is ours.
  await context.grantPermissions(["midi", "midi-sysex"]);
  const r = await bootWith(sonicPage, sonicConfig, { midi: true, gamepad: true });
  expect(r.errors, "subsystem wasm missing or page errors").toEqual([]);
  expect(r.gamepad, "the gamepad manager came up").toBe(true);
  // Web MIDI needs a platform backend, and a CI runner has none: Chromium
  // refuses with InvalidStateError after the wasm has already been fetched,
  // and the client boots without MIDI. Anything else is a real failure.
  if (!r.midi) expect(r.midiError, "the MIDI manager came up").toMatch(/Platform dependent initialization failed/);
});

test("a platform with no Web MIDI backend still boots, with the gamepad and the wasm fetch intact", async ({ sonicPage, sonicConfig }) => {
  // Deterministic on every OS: the refusal headless Linux gives for real.
  await sonicPage.evaluate(() => {
    window.__noMidi = { requestAccess: () => Promise.reject(new DOMException("Platform dependent initialization failed", "InvalidStateError")) };
  });
  const r = await sonicPage.evaluate(async (config) => {
    const errors = [];
    const sonic = new window.SuperSonic({ ...config, midi: window.__noMidi, gamepad: true });
    sonic.on("error", (e) => errors.push(String(e)));
    await sonic.init();
    const out = { midi: !!sonic.midi, midiError: String(sonic.midiError), gamepad: !!sonic.gamepad, errors };
    await sonic.destroy();
    return out;
  }, sonicConfig);
  expect(r.midi).toBe(false);
  expect(r.midiError).toContain("Platform dependent initialization failed");
  expect(r.errors, "the refusal is reported on the error event").toEqual(
    expect.arrayContaining([expect.stringContaining("midi unavailable")]));
  expect(r.gamepad, "the gamepad manager came up").toBe(true);
});

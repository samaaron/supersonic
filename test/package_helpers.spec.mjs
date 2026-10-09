// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Sam Aaron
//
// The asset packages' index.js: what a page imports to find the packages on a
// CDN. It has to load in a browser, which is where a CDN URL is wanted: the
// synthdefs and samples helpers once imported Node's `url` and `path` at the
// top, so no browser could import them (nor the bundle, which re-exports
// both). The filesystem paths are Node's, from node.js, chosen by the
// packages' "node" export condition.
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { test, expect } from "./fixtures.mjs";

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const pkgDir = (name) => path.join(ROOT, "packages", name);
const pkgJson = (name) => JSON.parse(fs.readFileSync(path.join(pkgDir(name), "package.json"), "utf8"));

test("a browser imports the synthdefs helper, and its CDN base ends in a slash", async ({ sonicPage }) => {
  const { version } = pkgJson("supersonic-scsynth-synthdefs");
  const m = await sonicPage.evaluate(async () => {
    const h = await import("/packages/supersonic-scsynth-synthdefs/index.js");
    return { cdn: h.SYNTHDEFS_CDN, names: h.SYNTHDEF_NAMES.length };
  });
  expect(m.cdn).toBe(`https://unpkg.com/supersonic-scsynth-synthdefs@${version}/synthdefs/`);
  expect(m.names).toBeGreaterThan(100);
});

test("a browser imports the samples helper", async ({ sonicPage }) => {
  const { version } = pkgJson("supersonic-scsynth-samples");
  const url = await sonicPage.evaluate(async () => {
    const h = await import("/packages/supersonic-scsynth-samples/index.js");
    return h.getSampleURL("bd_haus.flac", "jsdelivr");
  });
  expect(url).toBe(`https://cdn.jsdelivr.net/npm/supersonic-scsynth-samples@${version}/samples/bd_haus.flac`);
});

test("the core helper names this release, not @latest, and has no deprecated alias", async ({ sonicPage }) => {
  const { version } = pkgJson("supersonic-scsynth-core");
  const h = await sonicPage.evaluate(async () => {
    const m = await import("/packages/supersonic-scsynth-core/index.js");
    return { ...m };
  });
  const base = `https://unpkg.com/supersonic-scsynth-core@${version}/`;
  expect(h.CORE_CDN).toBe(base);
  expect(h.WASM_CDN).toBe(`${base}wasm/`);
  expect(h.WORKLET_CDN).toBe(`${base}workers/clockwork_audio_worklet.js`);
  expect(Object.keys(h).sort()).toEqual(["CORE_CDN", "WASM_CDN", "WORKLET_CDN"]);
});

test("under Node, the synthdefs and samples helpers also give the files' paths", async () => {
  for (const [name, dirKey, pathOf, sample] of [
    ["supersonic-scsynth-synthdefs", "SYNTHDEFS_DIR", "getSynthDefPath", "sonic-pi-beep"],
    ["supersonic-scsynth-samples", "SAMPLES_DIR", "getSamplePath", "bd_haus.flac"],
  ]) {
    const exp = pkgJson(name).exports?.["."];
    expect(exp, `${name}: the "node" condition picks node.js`).toEqual({ node: "./node.js", default: "./index.js" });
    const h = await import(path.join(pkgDir(name), "node.js"));
    expect(fs.statSync(h[dirKey]).isDirectory(), `${name}: ${dirKey}`).toBe(true);
    expect(fs.existsSync(h[pathOf](sample)), `${name}: ${pathOf}`).toBe(true);
    // Everything the browser helper has, Node has too.
    const browser = await import(path.join(pkgDir(name), "index.js"));
    for (const k of Object.keys(browser)) {
      if (k === "default") for (const d of Object.keys(browser.default)) expect(h.default[d], `${name}: default.${d}`).toEqual(browser.default[d]);
      else expect(h[k], `${name}: ${k}`).toEqual(browser[k]);
    }
  }
});

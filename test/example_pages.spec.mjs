// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * The example pages boot. Nothing else exercised them, and a page can be
 * wrong in ways the library's suite never sees: demo.html carried a
 * <tau-metrics> element for a component that had been registered as
 * <clockwork-metrics> for weeks, so the page never finished booting.
 * These drive each page the way a person does — the click that unlocks audio,
 * then something that makes sound — and require that no error reaches the
 * page and that the pieces the page composes are actually there.
 *
 * Each page is driven twice: from example/, where dist is a symlink into the
 * repository, and from build/site, the flat copy that scripts/export-site.sh
 * produces for publishing (test/global-setup.mjs exports it before the run).
 * A page that works only through the symlink is a page that breaks on upload.
 */
import { test, expect } from "@playwright/test";

const ROOTS = [
  { name: "repository", base: "/example" },
  { name: "exported site", base: "/build/site" },
];

function watch(page) {
  const errors = [];
  page.on("pageerror", (e) => errors.push(`pageerror: ${e.message}`));
  page.on("console", (m) => {
    if (m.type() === "error") errors.push(`console: ${m.text()}`);
  });
  return errors;
}

// The pad retargets the filter cutoff and the reverb mix on every pointer
// move, and Autoplay's first move jumps the cutoff from wide open to wherever
// the pointer sits. A filter whose cutoff moves in one block clicks — it was
// visible on the scope and very audible — so the fx nodes are created with a
// slide, which every later /n_set inherits. Asked of the running nodes.
test("the fx chain's cutoff and mix glide rather than snap", async ({ page }) => {
  const errors = watch(page);
  await page.goto("/example/demo.html");
  await page.click("#init-button");
  await page.waitForFunction(
    () => getComputedStyle(document.getElementById("init-button-container")).display === "none",
    null, { timeout: 30000 });

  const slides = await page.evaluate(async () => {
    const o = window.orchestrator;
    const get = (node, control) => new Promise((resolve, reject) => {
      const timer = setTimeout(() => { o.off("in", handler); reject(new Error(`no reply for ${node} ${control}`)); }, 5000);
      const handler = (msg) => {
        if (msg[0] === "/n_set" && msg[1] === node && msg[2] === control) {
          clearTimeout(timer); o.off("in", handler); resolve(msg[3]);
        }
      };
      o.on("in", handler);
      o.send("/s_get", node, control);
    });
    // startAll() builds the fx chain; the level ramp it schedules is not
    // waited for here — the nodes exist once startAll resolves.
    await window.startAll();
    const out = { cutoff: await get(2000, "cutoff_slide"), mix: await get(2001, "mix_slide") };
    window.stopAll();
    return out;
  });

  expect(slides.cutoff, "fx_lpf cutoff has no slide").toBeGreaterThan(0);
  expect(slides.mix, "fx_reverb mix has no slide").toBeGreaterThan(0);
  expect(errors, "the page reported errors").toEqual([]);
});

// The blob moves the way the sound does: a move sets the fx target at once,
// and the blob takes the slide time to get there, laying its trail along the
// path. Autoplay used to animate the pointer over 1.2 s while the sound had
// long since arrived; a press used to jump the blob and leave the trail with
// gaps.
//
// Two things this pins were flakes on a loaded CI runner. startAll() used to
// resolve before the scheduler worker had its channel, with the Autoplay
// button still disabled, so a press right after it was dropped: the blob
// stayed put and the cutoff stayed wide open. And the trail was drawn a
// frame behind the blob (two rAF loops, the scope's — which drew the trail —
// registered first), so at the moment the blob arrived the trail was short
// of it by a frame's travel, half the glide on a slow frame; a wall-clock
// wait for the next frame is not a guarantee. The page now steps the blob
// and lays its trail in one frame, and that is what is checked, every
// frame: the path behind the blob has no gap, however fast the frames come.
test("the blob arrives with the sound and the trail follows its path", async ({ page }) => {
  const errors = watch(page);
  await page.goto("/example/demo.html");
  await page.click("#init-button");
  await page.waitForFunction(
    () => getComputedStyle(document.getElementById("init-button-container")).display === "none",
    null, { timeout: 30000 });

  const r = await page.evaluate(async () => {
    const o = window.orchestrator;
    const ui = window.uiState;
    const get = (node, control) => new Promise((resolve, reject) => {
      const timer = setTimeout(() => { o.off("in", handler); reject(new Error(`no reply for ${node} ${control}`)); }, 5000);
      const handler = (msg) => {
        if (msg[0] === "/n_set" && msg[1] === node && msg[2] === control) {
          clearTimeout(timer); o.off("in", handler); resolve(msg[3]);
        }
      };
      o.on("in", handler);
      o.send("/s_get", node, control);
    });
    await window.startAll();
    const button = document.getElementById("play-toggle");
    const disabledAtPress = button.classList.contains("disabled");

    const rect = document.getElementById("synth-pad").getBoundingClientRect();
    const target = { x: 60 / rect.width, y: 1 - 60 / rect.height }; // where Autoplay parks
    const start = { x: ui.padX, y: ui.padY };
    // The trail canvas, read whole once per frame; the path is then walked
    // pixel by pixel in it. Lit at a point means a particle was drawn over
    // a 5x5 patch there. The trail is laid as clusters 6 px apart along the
    // path, so up to a few pixels short of the blob nothing longer than
    // two spacings may be dark: that is a gap, not a scatter miss.
    const c = document.getElementById("trail-canvas");
    const ctx = c.getContext("2d");
    const sx = start.x * c.width, sy = (1 - start.y) * c.height;
    const tx = target.x * c.width, ty = (1 - target.y) * c.height;
    const pathPx = Math.hypot(tx - sx, ty - sy);
    const litAt = (img, d) => {
      const px = Math.round(sx + ((tx - sx) * d) / pathPx);
      const py = Math.round(sy + ((ty - sy) * d) / pathPx);
      let m = 0;
      for (let y = py - 2; y <= py + 2; y++) {
        for (let x = px - 2; x <= px + 2; x++) {
          const k = (y * img.width + x) * 4;
          m = Math.max(m, img.data[k], img.data[k + 1], img.data[k + 2]);
        }
      }
      return m;
    };
    // Longest dark run, in px, from just past the start to `upto` px along,
    // and the dimmest point on the way.
    const longestGap = (img, upto) => {
      let gap = 0, run = 0, dimmest = 255;
      for (let d = 6; d <= upto; d++) {
        const v = litAt(img, d);
        dimmest = Math.min(dimmest, v);
        run = v > 30 ? 0 : run + 1;
        gap = Math.max(gap, run);
      }
      return { gap, dimmest };
    };
    const along = () => {
      const bx = ui.padX * c.width, by = (1 - ui.padY) * c.height;
      return ((bx - sx) * (tx - sx) + (by - sy) * (ty - sy)) / pathPx;
    };
    // The last cluster sits under a spacing (6 px) short of the blob, and
    // its glow reaches a few pixels past it, so this close to the blob the
    // trail is lit. Tighter than a frame's travel at 120 Hz (23 px): a
    // trail laid a frame behind the blob shows as a gap here.
    const BEHIND_PX = 8;

    const t0 = performance.now();
    button.click();
    const atOnce = { x: ui.padX, y: ui.padY };
    const cutoffAtOnce = await get(2000, "cutoff");

    let arrivedAt = null;
    const frames = [];
    while (performance.now() - t0 < 1500) {
      await new Promise((r) => requestAnimationFrame(r));
      const img = ctx.getImageData(0, 0, c.width, c.height);
      const at = along();
      frames.push({ t: Math.round(performance.now() - t0), at: Math.round(at), ...longestGap(img, at - BEHIND_PX) });
      if (Math.abs(ui.padX - target.x) < 0.005 && Math.abs(ui.padY - target.y) < 0.005) {
        arrivedAt = performance.now() - t0;
        break;
      }
    }
    const final = longestGap(ctx.getImageData(0, 0, c.width, c.height), pathPx - BEHIND_PX);
    window.stopAll();
    return { start, target, atOnce, cutoffAtOnce, expectedCutoff: 30 + target.y * 100, arrivedAt, pathPx, frames, final, disabledAtPress };
  });

  // A started page takes the press: the button is live once startAll() is.
  expect(r.disabledAtPress, "Autoplay button still disabled after startAll()").toBe(false);
  // The sound has its target the moment the button is pressed...
  expect(r.cutoffAtOnce).toBeCloseTo(r.expectedCutoff, 1);
  // ...and the blob has not jumped there: it travels.
  expect(Math.abs(r.atOnce.x - r.target.x) + Math.abs(r.atOnce.y - r.target.y)).toBeGreaterThan(0.05);
  // It arrives on the slide's timescale — not a jump, not a second later.
  expect(r.arrivedAt, "the blob never arrived").not.toBeNull();
  expect(r.arrivedAt).toBeGreaterThan(40);
  expect(r.arrivedAt).toBeLessThan(400);
  // And the trail is a line along the way it came, not a few dots — laid
  // up to the blob in every frame, not a frame behind it.
  // Clusters are 6 px apart: a dark run longer than that is a gap.
  const describe = (f) => `${f.t}ms at ${f.at}px gap ${f.gap}px dimmest ${f.dimmest}`;
  expect(r.frames.length, "no frame was observed").toBeGreaterThan(0);
  for (const f of r.frames) {
    expect(f.gap, `trail short of the blob (${describe(f)}); frames: ${r.frames.map(describe).join(", ")}`).toBeLessThanOrEqual(6);
  }
  expect(r.final.gap, `gap in the trail along the ${Math.round(r.pathPx)}px path (dimmest ${r.final.dimmest})`).toBeLessThanOrEqual(6);
  expect(errors, "the page reported errors").toEqual([]);
});

// Dragging retargets the blob on every pointer event. It must trail the
// pointer by the slide time and no more, however fast the pointer moves: a
// tween that eased in from rest on each retarget never got going, and the
// lag grew with speed.
test("a dragged blob trails the pointer by the slide time, not more", async ({ page }) => {
  const errors = watch(page);
  await page.goto("/example/demo.html");
  await page.click("#init-button");
  await page.waitForFunction(
    () => getComputedStyle(document.getElementById("init-button-container")).display === "none",
    null, { timeout: 30000 });

  // The pad is below the fold at the test viewport's height; a press that
  // lands on the page body activates nothing.
  await page.locator("#synth-pad").scrollIntoViewIfNeeded();
  const rect = await page.evaluate(() => {
    const r = document.getElementById("synth-pad").getBoundingClientRect();
    window.__drag = { pointer: [], blob: [], on: true };
    document.addEventListener("mousemove", (e) =>
      window.__drag.pointer.push([performance.now(), (e.clientX - r.left) / r.width]));
    const tick = () => {
      window.__drag.blob.push([performance.now(), window.uiState.padX]);
      if (window.__drag.on) requestAnimationFrame(tick);
    };
    requestAnimationFrame(tick);
    return { left: r.left, top: r.top, w: r.width, h: r.height };
  });

  // A press at the left edge, then a steady sweep to the right edge over
  // about a third of a second, then a pause with the button still held.
  const y = rect.top + rect.h / 2;
  await page.mouse.move(rect.left + 4, y);
  await page.mouse.down();
  const steps = 40;
  for (let i = 1; i <= steps; i++) {
    await page.mouse.move(rect.left + 4 + (rect.w - 8) * (i / steps), y);
    await page.waitForTimeout(8);
  }
  await page.waitForTimeout(300);
  await page.mouse.up();

  const d = await page.evaluate(() => { window.__drag.on = false; return window.__drag; });
  // For each blob position, how long ago was the pointer there? That is the
  // blob's lag behind the pointer at that moment.
  const finalPointer = d.pointer[d.pointer.length - 1][1];
  const finalBlob = d.blob[d.blob.length - 1][1];
  let maxLag = 0;
  for (const [tb, xb] of d.blob) {
    if (xb < 0.05) continue;
    if (Math.abs(xb - finalPointer) < 0.005) continue; // arrived; the hold is not lag
    const reached = d.pointer.find(([, xp]) => xp >= xb - 0.005);
    if (!reached) continue;
    maxLag = Math.max(maxLag, tb - reached[0]);
  }

  expect(d.pointer.length).toBeGreaterThan(20);
  // The slide is 100 ms; a frame or two of rAF on top — on an idle machine.
  // The blob moves on requestAnimationFrame, and a CI runner with the whole
  // suite on every core stalls frames by a hundred milliseconds and more
  // (249 ms was measured on the publish job). The bound is set to catch a
  // blob that has stopped following, not one whose frames came late.
  expect(maxLag, "the blob fell behind the pointer").toBeLessThan(400);
  expect(Math.abs(finalBlob - finalPointer), "the blob did not settle where the pointer stopped").toBeLessThan(0.01);
  expect(errors, "the page reported errors").toEqual([]);
});

for (const root of ROOTS) {
  test.describe(root.name, () => {
    test("demo.html boots, upgrades its metrics panel, and plays a loop", async ({ page }) => {
      const errors = watch(page);
      await page.goto(`${root.base}/demo.html`);

      // The gesture that unlocks the AudioContext, then the orchestrator's ready
      // path hides the overlay and marks the loading log complete.
      await page.click("#init-button");
      await page.waitForFunction(
        () => document.getElementById("init-button-container")?.classList.contains("hidden")
           || getComputedStyle(document.getElementById("init-button-container")).display === "none",
        null, { timeout: 30000 });

      // The metrics panel is a custom element: if the tag in the page and the
      // name the component registers ever disagree again, this is what fails.
      const metrics = await page.evaluate(async () => {
        await customElements.whenDefined("clockwork-metrics");
        const el = document.querySelector("clockwork-metrics");
        // It renders into light DOM — panels appended as children — so an
        // upgraded element has children and an un-upgraded one has none.
        return { present: !!el, upgraded: !!el && el.childElementCount > 0 };
      });
      expect(metrics.present, "no <clockwork-metrics> in the page").toBe(true);
      expect(metrics.upgraded, "the metrics element never rendered").toBe(true);

      // Make sound through the page's own entry points, then stop.
      await page.evaluate(() => window.startKickLoop());
      await page.waitForTimeout(1500);
      await page.evaluate(() => window.stopAll());

      expect(errors, "the page reported errors").toEqual([]);
    });

    test("simple.html boots, loads a synthdef and a sample, and plays", async ({ page }) => {
      const errors = watch(page);
      await page.goto(`${root.base}/simple.html`);

      // First click boots and loads. The button's text is the page's own
      // signal, and its resting text is also its starting text, so the boot
      // has to be seen to begin — a page whose module never loaded leaves the
      // button reading "Play Amen Break" and doing nothing.
      await page.click("#play");
      await expect(page.locator("#play")).toHaveText("Booting scsynth...");
      await expect(page.locator("#play")).toHaveText("Play Amen Break", { timeout: 30000 });
      await page.click("#play");
      await page.waitForTimeout(800);

      expect(errors, "the page reported errors").toEqual([]);
    });
  });
}

/**
 * UGen Unit Test: Median length below 1 (upstream #7672, commit 8a483574d)
 *
 * Median reads its length once, in its constructor. A length below 1 left
 * the insertion position at -1, which indexed the median window out of
 * bounds and could crash the server. The length is now clipped to 1, where
 * Median passes its input through.
 *
 * Probe (compiled by test/synthdefs/compile_median_synthdefs.scd):
 *   median_length_probe: Out.kr(bus, Median.kr(\length.ir(3), DC.kr(0.5)))
 * The median of a constant is that constant, so every length must read 0.5.
 */

import { test, expect } from "./fixtures.mjs";

const LENGTHS = [3, 1, 0, -3];

test.describe("Median length below 1", () => {
  for (const length of LENGTHS) {
    test(`length=${length} → median of a constant 0.5 is 0.5`, async ({ page, sonicConfig }) => {
      await page.goto("/test/harness.html");
      const value = await page.evaluate(async ({ config, length }) => {
        const sonic = new window.SuperSonic(config);
        const messages = [];
        sonic.on("in", (m) => messages.push(Array.from(m)));
        await sonic.init();
        const r = await fetch("/test/synthdefs/median_length_probe.scsyndef");
        await sonic.loadSynthDef(new Uint8Array(await r.arrayBuffer()));

        const BUS = 200;
        await sonic.send("/s_new", "median_length_probe", 9400, 0, 0, "bus", BUS, "length", length);
        await new Promise((res) => setTimeout(res, 60));
        messages.length = 0;
        await sonic.send("/c_get", BUS);
        await sonic.sync(98);
        await new Promise((res) => setTimeout(res, 30));
        await sonic.send("/n_free", 9400);

        const reply = messages.find((m) => m[0] === "/c_set" && m[1] === BUS);
        await sonic.destroy();
        return reply ? reply[2] : undefined;
      }, { config: sonicConfig, length });

      expect(value).toBeCloseTo(0.5, 6);
    });
  }
});

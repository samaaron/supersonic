/**
 * WASM gamepad: the shared Rust core (state diffing/schema) and the JS
 * GamepadManager running in a real browser. The Gamepad API is mocked so the
 * round-trip is deterministic in headless Chromium (a real pad needs hardware
 * + a user gesture).
 */
import { test, expect } from "@playwright/test";

test.describe("WASM gamepad", () => {
  test("Rust core diffing + schema run in the browser", async ({ page }) => {
    await page.goto("/test/gamepad_wasm.html");
    await page.waitForFunction(() => window.gamepadWasmReady === true);

    const r = await page.evaluate(async () => {
      const w = window.wasmGamepad;
      await w.initWasm();

      const norm = w.normalize_name("Pro Controller: USB");
      // Handle assignment dedups against the taken set with the shared rule.
      const handle2 = w.assign_handle("Pro Controller: USB", ["pro_controller__usb"]);

      // Standard-mapping diff state: a south press emits once, repeats are
      // silent (no-change ticks return undefined — no array materialised),
      // and the browser's down-positive Y flips to canonical up.
      const st = new w.WasmPadState(true);
      const pressed = new Uint8Array(17);
      const values = new Float64Array(17);
      const axes = new Float64Array([0, 0, 0, 0]);
      pressed[0] = 1;
      values[0] = 1;
      axes[1] = -1; // stick pushed fully up (browser convention)
      const first = st.update(pressed, values, axes);
      const repeat = st.update(pressed, values, axes) ?? null;

      // Axis rest drift inside the deadzone stays silent.
      const drift = st.update(pressed, values, new Float64Array([0.03, -1, 0, 0])) ?? null;

      // Extension axes beyond the W3C standard four are NOT d-pad axes: they
      // must surface under generic names, not dpad_x/dpad_y.
      const extra = st.update(pressed, values, new Float64Array([0.03, -1, 0, 0, 1, 0]));

      // inbound event → /gamepad/in/* OSC
      const inOsc = w.gamepad_button_osc("pad", "south", 1, 1.0);
      const inAddr = new TextDecoder().decode(inOsc.subarray(0, inOsc.indexOf(0)));

      // outbound: /gamepad/out/rumble decodes to a flat command
      const rumbleOsc = w.encodeOsc("/gamepad/out/rumble", [
        { t: "s", v: "*" }, { t: "f", v: 1.0 }, { t: "f", v: 0.5 }, { t: "i", v: 200 },
      ]);
      const rumble = w.gamepad_out_decode(rumbleOsc);

      // foreign addresses are ignored (wasm None → undefined; normalise for
      // the evaluate() serialisation boundary)
      const foreign =
        w.gamepad_out_decode(w.encodeOsc("/midi/out/clock", [{ t: "s", v: "x" }])) ?? null;

      return { norm, handle2, first, repeat, drift, extra, inAddr, rumble, foreign };
    });

    expect(r.norm).toBe("pro_controller__usb");
    expect(r.handle2).toBe("pro_controller__usb_2");
    // One button event + one axis event, four slots each.
    expect(r.first.length).toBe(8);
    expect(r.first.slice(0, 4)).toEqual(["button", "south", 1, 1]);
    expect(r.first[4]).toBe("axis");
    expect(r.first[5]).toBe("left_y");
    expect(r.first[6]).toBe(1); // flipped to up-positive
    expect(r.repeat).toBeNull();
    expect(r.drift).toBeNull();
    // axes[4] on a standard pad is a vendor extension, not dpad_x.
    expect(r.extra.length).toBe(4);
    expect(r.extra[0]).toBe("axis");
    expect(r.extra[1]).toBe("axis_4");
    expect(r.inAddr).toBe("/gamepad/in/button");
    expect(r.rumble).toEqual(["rumble", "*", 1, 0.5, 200]);
    expect(r.foreign).toBeNull();
  });

  test("GamepadManager round-trips through a (mocked) Gamepad API", async ({ page }) => {
    await page.goto("/test/gamepad_wasm.html");
    await page.waitForFunction(() => window.gamepadWasmReady === true);

    const r = await page.evaluate(async () => {
      const effects = [];
      const pad = {
        id: "Test Pad",
        index: 0,
        connected: true,
        mapping: "standard",
        buttons: Array.from({ length: 17 }, () => ({ pressed: false, value: 0 })),
        axes: [0, 0, 0, 0],
        vibrationActuator: {
          playEffect: (type, params) => { effects.push({ type, ...params }); return Promise.resolve("complete"); },
          reset: () => { effects.push({ type: "reset" }); return Promise.resolve("complete"); },
        },
      };
      Object.defineProperty(navigator, "getGamepads", {
        value: () => [pad, null, null, null],
        configurable: true,
      });

      const tick = (ms) => new Promise((res) => setTimeout(res, ms));
      const m = new window.GamepadManager({ pollIntervalMs: 2 });
      const events = [];
      const messages = [];
      const devices = [];
      m.onEvent((osc) => events.push(new TextDecoder().decode(osc.subarray(0, osc.indexOf(0)))));
      m.onDevices((d) => devices.push(d.pads));
      await m.init();
      await tick(30);

      // device → engine (OSC path): a south press becomes /gamepad/in/button
      pad.buttons[0] = { pressed: true, value: 1 };
      await tick(30);

      // structured path takes precedence once registered
      m.onMessage((fields) => messages.push(fields));
      pad.axes[1] = -1; // full up
      await tick(30);

      // engine → device: /gamepad/out/rumble reaches the actuator
      m.sendOut(window.wasmGamepad.encodeOsc("/gamepad/out/rumble", [
        { t: "s", v: "test_pad" }, { t: "f", v: 1.0 }, { t: "f", v: 0.25 }, { t: "i", v: 150 },
      ]));
      m.sendOut(window.wasmGamepad.encodeOsc("/gamepad/out/rumble_stop", [{ t: "s", v: "*" }]));

      m.dispose();
      return { events, messages, devices, effects };
    });

    expect(r.devices[0]).toEqual(["test_pad"]);
    expect(r.events).toContain("/gamepad/in/button");
    expect(r.messages).toContainEqual(["axis", "test_pad", "left_y", 1]);
    expect(r.effects[0].type).toBe("dual-rumble");
    expect(r.effects[0].strongMagnitude).toBe(1);
    expect(r.effects[0].weakMagnitude).toBe(0.25);
    expect(r.effects[0].duration).toBe(150);
    expect(r.effects[1].type).toBe("reset");
  });

  test("rumble is not delivered to a different pad that reused the slot", async ({ page }) => {
    await page.goto("/test/gamepad_wasm.html");
    await page.waitForFunction(() => window.gamepadWasmReady === true);

    const r = await page.evaluate(async () => {
      const effects = [];
      const makePad = (id) => ({
        id,
        index: 0,
        connected: true,
        mapping: "standard",
        buttons: Array.from({ length: 17 }, () => ({ pressed: false, value: 0 })),
        axes: [0, 0, 0, 0],
        vibrationActuator: {
          playEffect: (type, params) => { effects.push({ id, type, ...params }); return Promise.resolve("complete"); },
          reset: () => { effects.push({ id, type: "reset" }); return Promise.resolve("complete"); },
        },
      });
      let current = makePad("Pad A");
      Object.defineProperty(navigator, "getGamepads", {
        value: () => [current, null, null, null],
        configurable: true,
      });

      // Huge poll interval: nothing refreshes the registry between the swap
      // and the send, exposing the stale-slot window.
      const m = new window.GamepadManager({ pollIntervalMs: 60000 });
      await m.init(); // registers Pad A as "pad_a"

      current = makePad("Pad B"); // slot 0 reused by different hardware
      m.sendOut(window.wasmGamepad.encodeOsc("/gamepad/out/rumble", [
        { t: "s", v: "pad_a" }, { t: "f", v: 1.0 }, { t: "f", v: 1.0 }, { t: "i", v: 100 },
      ]));

      m.dispose();
      return { effects };
    });

    // The rumble was addressed to Pad A; Pad B must not vibrate.
    expect(r.effects).toEqual([]);
  });

  test("a same-id pad growing its element count is re-registered, not truncated", async ({ page }) => {
    await page.goto("/test/gamepad_wasm.html");
    await page.waitForFunction(() => window.gamepadWasmReady === true);

    const r = await page.evaluate(async () => {
      const tick = (ms) => new Promise((res) => setTimeout(res, ms));
      const pad = {
        id: "Test Pad",
        index: 0,
        connected: true,
        mapping: "standard",
        buttons: Array.from({ length: 17 }, () => ({ pressed: false, value: 0 })),
        axes: [0, 0, 0, 0],
      };
      Object.defineProperty(navigator, "getGamepads", {
        value: () => [pad, null, null, null],
        configurable: true,
      });

      const m = new window.GamepadManager({ pollIntervalMs: 2 });
      const messages = [];
      m.onMessage((fields) => messages.push(fields));
      await m.init();
      await tick(30);

      // The pad sprouts an 18th button (out-of-spec, but must not become a
      // permanently dead input) and presses it.
      pad.buttons = Array.from({ length: 18 }, () => ({ pressed: false, value: 0 }));
      pad.buttons[17] = { pressed: true, value: 1 };
      await tick(50);

      m.dispose();
      return { messages };
    });

    expect(r.messages).toContainEqual(["button", "test_pad", "button_17", 1, 1]);
  });

  test("until-stop rumble is retriggered past the 5s effect cap until stopped", async ({ page }) => {
    await page.goto("/test/gamepad_wasm.html");
    await page.waitForFunction(() => window.gamepadWasmReady === true);

    const r = await page.evaluate(async () => {
      const tick = (ms) => new Promise((res) => setTimeout(res, ms));
      const effects = [];
      const pad = {
        id: "Test Pad",
        index: 0,
        connected: true,
        mapping: "standard",
        buttons: Array.from({ length: 17 }, () => ({ pressed: false, value: 0 })),
        axes: [0, 0, 0, 0],
        vibrationActuator: {
          playEffect: (type, params) => { effects.push({ type, ...params }); return Promise.resolve("complete"); },
          reset: () => { effects.push({ type: "reset" }); return Promise.resolve("complete"); },
        },
      };
      Object.defineProperty(navigator, "getGamepads", {
        value: () => [pad, null, null, null],
        configurable: true,
      });

      // Short refresh period so the test sees several renewals quickly; in
      // production this is ~4.5s against the API's 5s effect ceiling.
      const m = new window.GamepadManager({ pollIntervalMs: 2, rumbleRefreshMs: 20 });
      await m.init();

      m.sendOut(window.wasmGamepad.encodeOsc("/gamepad/out/rumble", [
        { t: "s", v: "test_pad" }, { t: "f", v: 0.8 }, { t: "f", v: 0.2 }, { t: "i", v: 0 },
      ]));
      await tick(100); // several refresh periods

      const playsBeforeStop = effects.filter((e) => e.type === "dual-rumble").length;
      m.sendOut(window.wasmGamepad.encodeOsc("/gamepad/out/rumble_stop", [{ t: "s", v: "*" }]));
      await tick(60);
      const playsAfterStop = effects.filter((e) => e.type === "dual-rumble").length;

      m.dispose();
      return { effects, playsBeforeStop, playsAfterStop };
    });

    // "Until stopped" must keep renewing the capped effect…
    expect(r.playsBeforeStop).toBeGreaterThanOrEqual(3);
    for (const e of r.effects.filter((e) => e.type === "dual-rumble")) {
      expect(e.strongMagnitude).toBeCloseTo(0.8, 5); // f32 on the OSC wire
      expect(e.duration).toBeLessThanOrEqual(5000);
    }
    // …and stop renewing once stopped.
    expect(r.effects.some((e) => e.type === "reset")).toBe(true);
    expect(r.playsAfterStop).toBe(r.playsBeforeStop);
  });
});

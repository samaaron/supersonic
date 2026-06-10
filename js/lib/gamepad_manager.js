// GamepadManager — the web gamepad seam (main thread). Owns Gamepad API I/O
// (polling navigator.getGamepads()) and drives the shared Rust core (compiled
// to wasm) for all protocol logic: per-pad diffing with the same deadzone,
// quantisation and press hysteresis as native, canonical button/axis names,
// /gamepad/in/* OSC encoding, /gamepad/out/* decoding, and name
// normalisation. This is the web counterpart of the native GamepadControl +
// Rust subsystem — the /gamepad/* OSC contract is identical.
//
// The Gamepad API is poll-only and main-thread only, so this runs alongside
// the SuperSonic JS, not in the AudioWorklet. Polling uses setInterval, not
// requestAnimationFrame: rAF throttles in background tabs, which would mute a
// controller the moment the tab loses focus — wrong for a music engine. The
// wasm diffing means the poll cadence never shows up on the wire; only real
// state changes do.

import init, {
  assign_handle,
  WasmPadState,
  gamepad_axis_osc,
  gamepad_button_osc,
  gamepad_out_decode,
} from "../../dist/gamepad/supersonic_gamepad.js";

export class GamepadManager {
  // rumbleRefreshMs: how often an active "until stopped" (or > 5 s) rumble is
  // re-issued — the Gamepad API caps a single effect at 5 s, so the poll loop
  // renews it just before expiry to match the native until-stop semantics.
  constructor({ pollIntervalMs = 8, rumbleRefreshMs = 4500 } = {}) {
    this._pollIntervalMs = pollIntervalMs;
    this._rumbleRefreshMs = rumbleRefreshMs;
    this._timer = null;
    this._pads = new Map(); // Gamepad.index -> { id, handle, state: WasmPadState, ... }
    this._onEvent = null; // (Uint8Array osc) => void  — /gamepad/in/* OSC packet
    this._onMessage = null; // (Array [kind, pad, name, ...]) => void  — structured
    this._onDevices = null; // ({pads}) => void
    this._lastDevicesKey = null; // last emitted pad list, to suppress no-op pushes
    this._onChange = () => this._refresh();
  }

  // Load the wasm core and start polling. Browsers only surface a pad after a
  // user gesture (typically the first button press), so an empty initial list
  // is normal; connect/disconnect events + polling pick pads up as they appear.
  async init() {
    await init();
    if (!navigator.getGamepads) throw new Error("Gamepad API unavailable");
    window.addEventListener("gamepadconnected", this._onChange);
    window.addEventListener("gamepaddisconnected", this._onChange);
    this._refresh();
    this._timer = setInterval(() => this._poll(), this._pollIntervalMs);
    return this;
  }

  dispose() {
    if (this._timer) clearInterval(this._timer);
    this._timer = null;
    window.removeEventListener("gamepadconnected", this._onChange);
    window.removeEventListener("gamepaddisconnected", this._onChange);
    this._pads.clear();
  }

  onEvent(cb) { this._onEvent = cb; }
  // Structured inbound events: cb receives ["button", pad, name, pressed01,
  // value] or ["axis", pad, name, value] with no OSC encode/decode round-trip.
  // Takes precedence over onEvent when both are set.
  onMessage(cb) { this._onMessage = cb; }
  onDevices(cb) { this._onDevices = cb; }

  _refresh() {
    const pads = navigator.getGamepads();
    // Drop vanished/replaced pads first so their handles free up…
    for (const [index, entry] of [...this._pads]) {
      const pad = pads[index];
      if (!pad || pad.id !== entry.id) this._pads.delete(index);
    }
    // …then register new arrivals. Handle assignment (normalise + _2/_3
    // dedup) is the shared Rust rule, so a handle means the same device on
    // web and native.
    const taken = [...this._pads.values()].map((e) => e.handle);
    for (const pad of pads) {
      if (!pad || this._pads.has(pad.index)) continue;
      const handle = assign_handle(pad.id, taken);
      taken.push(handle);
      this._pads.set(pad.index, {
        id: pad.id,
        handle,
        state: new WasmPadState(pad.mapping === "standard"),
        // Poll-snapshot buffers, reused every tick (element counts are fixed
        // for the life of a pad) so polling doesn't churn the GC.
        pressed: new Uint8Array(pad.buttons.length),
        values: new Float64Array(pad.buttons.length),
        axes: new Float64Array(pad.axes.length),
      });
    }
    // Browsers can fire connect/disconnect for transient transitions; only
    // notify when the pad list actually changed, mirroring the native
    // "broadcast only on change" behaviour.
    const names = [...this._pads.values()].map((e) => e.handle);
    const key = JSON.stringify(names);
    if (key === this._lastDevicesKey) return;
    this._lastDevicesKey = key;
    if (this._onDevices) this._onDevices({ pads: names });
  }

  _poll() {
    const pads = navigator.getGamepads();
    for (const pad of pads) {
      if (!pad) continue;
      let entry = this._pads.get(pad.index);
      const buttons = pad.buttons;
      const axes = pad.axes;
      if (
        !entry ||
        entry.id !== pad.id ||
        // Element counts are fixed for a connection, so a mismatch means the
        // entry is stale; re-register rather than silently truncating (which
        // would leave the extra inputs permanently dead).
        buttons.length !== entry.pressed.length ||
        axes.length !== entry.axes.length
      ) {
        this._pads.delete(pad.index);
        this._refresh(); // pad appeared (or slot/shape changed) between events
        entry = this._pads.get(pad.index);
        if (!entry) continue;
      }
      for (let i = 0; i < buttons.length; i++) {
        const b = buttons[i];
        entry.pressed[i] = b.pressed ? 1 : 0;
        entry.values[i] = b.value;
      }
      for (let i = 0; i < axes.length; i++) entry.axes[i] = axes[i];
      // No-change ticks (the overwhelmingly common case) return undefined —
      // no per-tick array materialised.
      const events = entry.state.update(entry.pressed, entry.values, entry.axes);
      if (events) {
        for (let i = 0; i < events.length; i += 4) {
          this._emit(entry.handle, events[i], events[i + 1], events[i + 2], events[i + 3]);
        }
      }
      this._refreshRumble(entry, pad);
    }
    // A pad unplugged mid-poll surfaces as a null slot before the
    // gamepaddisconnected event lands; sweep so the devices push is prompt.
    for (const index of this._pads.keys()) {
      if (!pads[index]) {
        this._refresh();
        break;
      }
    }
  }

  _emit(pad, kind, name, a, b) {
    // Prefer the structured fast path; fall back to OSC bytes for consumers
    // (e.g. the native-shaped engine ingress) that want the wire form.
    if (this._onMessage) {
      this._onMessage(kind === "button" ? [kind, pad, name, a, b] : [kind, pad, name, a]);
      return;
    }
    if (!this._onEvent) return;
    const osc =
      kind === "button"
        ? gamepad_button_osc(pad, name, a, b)
        : gamepad_axis_osc(pad, name, a);
    this._onEvent(osc);
  }

  // Drive rumble from a /gamepad/out/* OSC packet (rumble / rumble_stop).
  // Best-effort: pads without a vibrationActuator are skipped. The spec caps a
  // single effect at 5 s, so a rumble outliving the cap (durationMs <= 0 =
  // "until stop", or any longer duration) is renewed from the poll loop —
  // matching the native until-stop semantics.
  sendOut(oscBytes) {
    const cmd = gamepad_out_decode(oscBytes);
    if (!cmd) return;
    const [verb, target] = cmd;
    const pads = navigator.getGamepads();
    for (const [index, entry] of this._pads) {
      if (target !== "*" && entry.handle !== target) continue;
      const pad = pads[index];
      // The registry can be a poll-tick stale: a different pad may have
      // reused the slot — never rumble hardware the handle doesn't name.
      if (!pad || pad.id !== entry.id) continue;
      const actuator = pad.vibrationActuator;
      if (!actuator) continue;
      if (verb === "rumble") {
        const [, , strong, weak, durationMs] = cmd;
        const now = performance.now();
        actuator.playEffect("dual-rumble", {
          strongMagnitude: strong,
          weakMagnitude: weak,
          duration: durationMs > 0 ? Math.min(durationMs, 5000) : 5000,
        });
        entry.rumble = {
          strong,
          weak,
          until: durationMs > 0 ? now + durationMs : Infinity,
          nextPlay: now + this._rumbleRefreshMs,
        };
      } else if (verb === "rumble_stop") {
        if (actuator.reset) actuator.reset();
        delete entry.rumble;
      }
    }
  }

  // Renew an active long-running rumble before the API's 5 s effect ceiling
  // cuts it off. Called every poll tick for each live pad.
  _refreshRumble(entry, pad) {
    const r = entry.rumble;
    if (!r) return;
    const now = performance.now();
    if (now >= r.until) {
      delete entry.rumble; // the final (remaining-duration) effect ends itself
      return;
    }
    if (now < r.nextPlay) return;
    pad.vibrationActuator?.playEffect("dual-rumble", {
      strongMagnitude: r.strong,
      weakMagnitude: r.weak,
      duration: Math.min(r.until - now, 5000),
    });
    r.nextPlay = now + this._rumbleRefreshMs;
  }
}

//! wasm-bindgen surface for the web gamepad seam: the same shared Rust core
//! (schema/state/normalisation) exposed to the main-thread JS `GamepadManager`,
//! which owns the Gamepad API I/O (polling `navigator.getGamepads()`). Native
//! I/O lives in `device`/`ffi` instead.
#![cfg(target_arch = "wasm32")]

use wasm_bindgen::prelude::*;

use crate::schema::{decode_out, encode_axis, encode_button, OutCommand};
use crate::state::{axis_name, button_name, wire_name, PadState};

/// The W3C "standard" gamepad mapping defines exactly four axes (two sticks).
/// Anything beyond is a vendor extension and must not take a canonical name —
/// in particular not `dpad_x`/`dpad_y` (table indices 4/5), which only exist
/// for native hat-axis devices.
const STANDARD_AXES: usize = 4;

/// Normalise a raw device name to its OSC-safe handle — identical to the
/// native path, so a pad is addressed the same way on web and native.
#[wasm_bindgen]
pub fn normalize_name(raw: &str) -> String {
    crate::normalize::safe_osc_name(raw)
}

/// Assign a stable handle for a newly-connected pad against the handles
/// already in use — the same normalise + `_2`/`_3` dedup rule as the native
/// registry, so a handle means the same device on web and native.
#[wasm_bindgen]
pub fn assign_handle(raw: &str, taken: Vec<String>) -> String {
    crate::normalize::assign_handle(raw, |h| taken.iter().any(|t| t == h))
}

/// Per-pad diff state. The JS manager polls `navigator.getGamepads()` and
/// feeds each pad's full snapshot in; only actual state changes come back, so
/// the poll cadence never shows up on the wire.
#[wasm_bindgen]
pub struct WasmPadState {
    inner: PadState,
    standard: bool,
}

#[wasm_bindgen]
impl WasmPadState {
    /// `standard` is `Gamepad.mapping === "standard"`: canonical W3C-indexed
    /// names with the browser's down-positive stick Y flipped to the shared
    /// up-positive convention. Non-standard pads pass through raw with
    /// generic `button_<i>` / `axis_<i>` names.
    #[wasm_bindgen(constructor)]
    pub fn new(standard: bool) -> WasmPadState {
        WasmPadState { inner: PadState::new(), standard }
    }

    /// One poll snapshot: `pressed`/`values` per button index (from
    /// `Gamepad.buttons[i].pressed/.value`), `axes` from `Gamepad.axes`.
    /// Returns the changes as a flat array, four slots per event:
    /// `["button", name, pressed01, value]` / `["axis", name, value, 0]` —
    /// or `None` (no JS array materialised) on the common no-change tick.
    pub fn update(&mut self, pressed: &[u8], values: &[f64], axes: &[f64]) -> Option<Vec<JsValue>> {
        let mut out = Vec::new();
        for (i, (&p, &v)) in pressed.iter().zip(values.iter()).enumerate() {
            if let Some(crate::state::PadEvent::Button { idx, pressed, value }) =
                self.inner.set_button(i, p != 0, v as f32)
            {
                let canonical = button_name(idx).filter(|_| self.standard);
                out.push(JsValue::from_str("button"));
                out.push(JsValue::from_str(&wire_name(canonical, "button", idx)));
                out.push(JsValue::from_f64(pressed as i32 as f64));
                out.push(JsValue::from_f64(value as f64));
            }
        }
        for (i, &v) in axes.iter().enumerate() {
            let mut value = v as f32;
            if self.standard && (i == 1 || i == 3) {
                value = -value; // browser Y is down-positive; canonical is up
            }
            if let Some(crate::state::PadEvent::Axis { idx, value }) =
                self.inner.set_axis(i, value)
            {
                // Canonical names only for the four standard-mapping axes;
                // extension axes are generic (see STANDARD_AXES).
                let canonical =
                    axis_name(idx).filter(|_| self.standard && idx < STANDARD_AXES);
                out.push(JsValue::from_str("axis"));
                out.push(JsValue::from_str(&wire_name(canonical, "axis", idx)));
                out.push(JsValue::from_f64(value as f64));
                out.push(JsValue::from_f64(0.0));
            }
        }
        if out.is_empty() {
            None
        } else {
            Some(out)
        }
    }
}

/// One button event (as produced by [`WasmPadState::update`]) → the
/// `/gamepad/in/button` OSC packet for the engine ingress / wire-form
/// consumers.
#[wasm_bindgen]
pub fn gamepad_button_osc(pad: &str, name: &str, pressed: i32, value: f64) -> Vec<u8> {
    encode_button(pad, name, pressed != 0, value as f32)
}

/// One axis event → the `/gamepad/in/axis` OSC packet.
#[wasm_bindgen]
pub fn gamepad_axis_osc(pad: &str, name: &str, value: f64) -> Vec<u8> {
    encode_axis(pad, name, value as f32)
}

/// A `/gamepad/out/*` OSC packet → a flat command array for the JS layer to
/// hand to `Gamepad.vibrationActuator`:
/// `["rumble", pad, strong, weak, durationMs]` or `["rumble_stop", pad]`.
/// `None` for non-output verbs (device management is handled JS-side).
#[wasm_bindgen]
pub fn gamepad_out_decode(osc: &[u8]) -> Option<Vec<JsValue>> {
    match decode_out(osc)? {
        OutCommand::Rumble { pad, strong, weak, duration_ms } => Some(vec![
            JsValue::from_str("rumble"),
            JsValue::from_str(&pad),
            JsValue::from_f64(strong as f64),
            JsValue::from_f64(weak as f64),
            JsValue::from_f64(duration_ms as f64),
        ]),
        OutCommand::RumbleStop { pad } => Some(vec![
            JsValue::from_str("rumble_stop"),
            JsValue::from_str(&pad),
        ]),
        _ => None,
    }
}

//! Canonical controller vocabulary + per-pad diff state.
//!
//! Both IO shells feed raw readings in here and emit only what this module
//! hands back, so the `/gamepad/in/*` stream is identical on web and native:
//! same names, same value quantisation, same deadzone, same press hysteresis.
//!
//! Conventions (documented in docs/OSC_API.md):
//! * Button values are 0..=1 (analog triggers sweep; digital buttons are 0/1).
//! * Axis values are -1..=1 with **up/right positive** (the gilrs convention;
//!   the web shell flips the browser's down-positive Y before feeding us).
//! * Values are quantised to 1/127 steps and events fire only on change, so a
//!   drifting stick at rest is silent and a sweep can't flood the OSC ring.

/// Canonical button names, ordered by the W3C "standard" gamepad mapping index
/// (https://w3c.github.io/gamepad/#remapping) so the web shell can index this
/// table directly with `Gamepad.buttons[i]`. Native (gilrs) buttons are mapped
/// onto the same indices in `device.rs`. Buttons beyond the table (extra
/// hardware buttons) fall back to a generic `button_<i>` name.
pub const BUTTONS: [&str; 17] = [
    "south",          //  0  bottom action (A / cross)
    "east",           //  1  right action  (B / circle)
    "west",           //  2  left action   (X / square)
    "north",          //  3  top action    (Y / triangle)
    "left_shoulder",  //  4  L1
    "right_shoulder", //  5  R1
    "left_trigger",   //  6  L2 (analog 0..=1)
    "right_trigger",  //  7  R2 (analog 0..=1)
    "select",         //  8  back / share
    "start",          //  9  start / options
    "left_thumb",     // 10  left-stick click
    "right_thumb",    // 11  right-stick click
    "dpad_up",        // 12
    "dpad_down",      // 13
    "dpad_left",      // 14
    "dpad_right",     // 15
    "mode",           // 16  guide / home / PS
];

/// Canonical axis names. The first four match W3C standard-mapping `axes[0..4]`
/// (after the web shell's Y flip); `dpad_x`/`dpad_y` only occur natively, on
/// the rare devices whose d-pad reports as a hat axis instead of four buttons.
/// Axes beyond the table fall back to a generic `axis_<i>` name.
pub const AXES: [&str; 6] = [
    "left_x",  // 0
    "left_y",  // 1
    "right_x", // 2
    "right_y", // 3
    "dpad_x",  // 4
    "dpad_y",  // 5
];

/// Hard caps on tracked elements per pad: bounds memory against a hostile or
/// broken source claiming absurd element counts (browser `Gamepad` objects are
/// attacker-influenced in the same sense as any other web input).
pub const MAX_BUTTONS: usize = 64;
pub const MAX_AXES: usize = 16;

pub fn button_name(idx: usize) -> Option<&'static str> {
    BUTTONS.get(idx).copied()
}

pub fn axis_name(idx: usize) -> Option<&'static str> {
    AXES.get(idx).copied()
}

/// The wire name for an emitted element: the canonical name when one applies,
/// `<kind>_<idx>` beyond the tables. The single home of the fallback rule —
/// every shell (native backends via `Out::from_pad_event`, the wasm seam)
/// names elements through this, so the `/gamepad/in/*` vocabulary cannot
/// diverge across targets.
pub fn wire_name(canonical: Option<&'static str>, kind: &str, idx: usize) -> String {
    canonical.map(str::to_string).unwrap_or_else(|| format!("{kind}_{idx}"))
}

/// Stick readings inside the deadzone are snapped to 0 (and the live range
/// rescaled to keep the full -1..=1 sweep), so a worn stick at rest is silent.
const AXIS_DEADZONE: f32 = 0.08;
/// Press hysteresis for analog buttons (value-only sources): press at ≥ 0.6,
/// release below 0.3, so a trigger hovering near the threshold can't chatter.
const PRESS_ON: f32 = 0.6;
const PRESS_OFF: f32 = 0.3;

/// Quantise to 1/127 steps: the diffing resolution (and a familiar MIDI-ish
/// granularity for clients mapping values to controls).
fn quantize(v: f32) -> f32 {
    (v * 127.0).round() / 127.0
}

fn deadzone(v: f32) -> f32 {
    let a = v.abs();
    if a < AXIS_DEADZONE {
        0.0
    } else {
        v.signum() * ((a - AXIS_DEADZONE) / (1.0 - AXIS_DEADZONE)).min(1.0)
    }
}

/// One emitted state change. `idx` keys [`BUTTONS`] / [`AXES`] (generic
/// `button_<idx>` / `axis_<idx>` beyond the tables).
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum PadEvent {
    Button { idx: usize, pressed: bool, value: f32 },
    Axis { idx: usize, value: f32 },
}

/// Last-emitted state for one pad. Feed raw readings in; an `Option<PadEvent>`
/// comes back only when the quantised state actually changed. Storage grows on
/// first touch up to [`MAX_BUTTONS`]/[`MAX_AXES`]; indices beyond the caps are
/// ignored.
#[derive(Clone, Debug, Default)]
pub struct PadState {
    buttons: Vec<(bool, f32)>,
    axes: Vec<f32>,
}

impl PadState {
    pub fn new() -> Self {
        Self::default()
    }

    /// Button reading with an authoritative pressed flag (web: the browser's
    /// `GamepadButton.pressed`; native: gilrs ButtonPressed/Released edges).
    /// Non-finite values are rejected: NaN would propagate through
    /// clamp/quantise and never compare equal to the stored state, turning the
    /// change-only dedupe into a per-poll-tick event flood.
    pub fn set_button(&mut self, idx: usize, pressed: bool, value: f32) -> Option<PadEvent> {
        if idx >= MAX_BUTTONS || !value.is_finite() {
            return None;
        }
        if self.buttons.len() <= idx {
            self.buttons.resize(idx + 1, (false, 0.0));
        }
        let v = quantize(value.clamp(0.0, 1.0));
        if self.buttons[idx] == (pressed, v) {
            return None;
        }
        self.buttons[idx] = (pressed, v);
        Some(PadEvent::Button { idx, pressed, value: v })
    }

    /// Button reading from a value-only source (gilrs ButtonChanged): the
    /// pressed flag is derived here with hysteresis.
    pub fn set_button_value(&mut self, idx: usize, value: f32) -> Option<PadEvent> {
        let was_pressed = self.buttons.get(idx).map(|b| b.0).unwrap_or(false);
        let pressed = if was_pressed { value > PRESS_OFF } else { value >= PRESS_ON };
        self.set_button(idx, pressed, value)
    }

    /// Last value-state of a button (used by the native shell to keep analog
    /// values across gilrs's separate pressed/released edge events).
    pub fn button_value(&self, idx: usize) -> f32 {
        self.buttons.get(idx).map(|b| b.1).unwrap_or(0.0)
    }

    pub fn set_axis(&mut self, idx: usize, value: f32) -> Option<PadEvent> {
        if idx >= MAX_AXES || !value.is_finite() {
            return None; // non-finite: see set_button
        }
        if self.axes.len() <= idx {
            self.axes.resize(idx + 1, 0.0);
        }
        let v = quantize(deadzone(value.clamp(-1.0, 1.0)));
        if self.axes[idx] == v {
            return None;
        }
        self.axes[idx] = v;
        Some(PadEvent::Axis { idx, value: v })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn digital_button_edges_dedupe() {
        let mut s = PadState::new();
        assert_eq!(
            s.set_button(0, true, 1.0),
            Some(PadEvent::Button { idx: 0, pressed: true, value: 1.0 })
        );
        assert_eq!(s.set_button(0, true, 1.0), None); // repeat → silent
        assert_eq!(
            s.set_button(0, false, 0.0),
            Some(PadEvent::Button { idx: 0, pressed: false, value: 0.0 })
        );
    }

    #[test]
    fn value_only_buttons_use_hysteresis() {
        let mut s = PadState::new();
        // Below the press-on threshold: value events, not pressed.
        match s.set_button_value(7, 0.4) {
            Some(PadEvent::Button { pressed: false, .. }) => {}
            other => panic!("expected unpressed value event, got {other:?}"),
        }
        // Crossing 0.6 presses…
        match s.set_button_value(7, 0.7) {
            Some(PadEvent::Button { pressed: true, .. }) => {}
            other => panic!("expected press, got {other:?}"),
        }
        // …and 0.5 (inside the hysteresis band) stays pressed.
        match s.set_button_value(7, 0.5) {
            Some(PadEvent::Button { pressed: true, .. }) => {}
            other => panic!("expected still-pressed value event, got {other:?}"),
        }
        // Dropping below 0.3 releases.
        match s.set_button_value(7, 0.1) {
            Some(PadEvent::Button { pressed: false, .. }) => {}
            other => panic!("expected release, got {other:?}"),
        }
    }

    #[test]
    fn axis_deadzone_and_quantised_dedupe() {
        let mut s = PadState::new();
        assert_eq!(s.set_axis(0, 0.03), None); // rest drift inside deadzone
        assert_eq!(s.set_axis(0, -0.05), None);
        let ev = s.set_axis(0, 0.5).expect("real deflection emits");
        match ev {
            PadEvent::Axis { idx: 0, value } => assert!(value > 0.4 && value < 0.5),
            other => panic!("unexpected {other:?}"),
        }
        // A sub-quantum wiggle is silent; a real move emits again.
        assert_eq!(s.set_axis(0, 0.501), None);
        assert!(s.set_axis(0, 0.6).is_some());
        // Full deflection survives the deadzone rescale.
        assert_eq!(s.set_axis(1, 1.0), Some(PadEvent::Axis { idx: 1, value: 1.0 }));
    }

    #[test]
    fn non_finite_values_are_rejected() {
        let mut s = PadState::new();
        // NaN never compares equal, so without the guard each call would emit.
        assert_eq!(s.set_axis(0, f32::NAN), None);
        assert_eq!(s.set_axis(0, f32::NAN), None);
        assert_eq!(s.set_button(0, true, f32::INFINITY), None);
        assert_eq!(s.set_button_value(7, f32::NAN), None);
        // …and the stored state is untouched (a real value still diffs cleanly).
        assert!(s.set_axis(0, 0.5).is_some());
    }

    #[test]
    fn wire_names_canonical_and_generic() {
        assert_eq!(wire_name(button_name(0), "button", 0), "south");
        assert_eq!(wire_name(None, "button", 20), "button_20");
        assert_eq!(wire_name(axis_name(9), "axis", 9), "axis_9");
    }

    #[test]
    fn indices_beyond_caps_are_ignored() {
        let mut s = PadState::new();
        assert_eq!(s.set_button(MAX_BUTTONS, true, 1.0), None);
        assert_eq!(s.set_button_value(MAX_BUTTONS, 1.0), None);
        assert_eq!(s.set_axis(MAX_AXES, 1.0), None);
        // …while generic indices beyond the name tables still work.
        assert!(s.set_button(BUTTONS.len() + 3, true, 1.0).is_some());
        assert!(s.set_axis(AXES.len() + 2, 1.0).is_some());
    }
}

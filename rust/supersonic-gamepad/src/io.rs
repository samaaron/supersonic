//! Backend-shared native IO types: the device registry and the translated
//! event stream. Two platform backends produce these — gilrs (`device.rs`:
//! evdev / XInput) and Apple's GameController framework (`gc.rs`: macOS) —
//! behind one `GamepadIo` interface consumed by `ffi.rs`.

use crate::state::{axis_name, button_name, wire_name, PadEvent};

// Stable handle assignment is shared protocol logic — the same rule names
// MIDI ports and web-side pads. See supersonic_osc::normalize.
pub use crate::normalize::assign_handle;

/// The (handle, enabled) registry shared with the FFI shell, which reads it to
/// answer `/gamepad/devices/list` synchronously on the caller's thread and
/// writes it for `/gamepad/enable`. Rows are in connect order; the backends
/// register/remove rows through [`connect`](Registry::connect) /
/// [`disconnect`](Registry::disconnect) so the row bookkeeping (and the
/// default-enabled rule) lives here, not in each backend.
#[derive(Debug)]
pub struct Registry {
    rows: Vec<(String, bool)>,
    /// Applied to pads that connect later (set by `/gamepad/enable * …`).
    default_enabled: bool,
}

impl Default for Registry {
    fn default() -> Self {
        Registry { rows: Vec::new(), default_enabled: true }
    }
}

impl Registry {
    /// Add the row for a newly-connected pad, enabled per the current default.
    pub fn connect(&mut self, handle: &str) {
        let enabled = self.default_enabled;
        self.rows.push((handle.to_string(), enabled));
    }

    /// Drop the row for a disconnected pad.
    pub fn disconnect(&mut self, handle: &str) {
        self.rows.retain(|r| r.0 != handle);
    }

    /// Toggle a pad (or `"*"` = all + the default for future pads). Returns
    /// true if anything actually changed, so no-op toggles don't broadcast.
    pub fn set_enabled(&mut self, pad: &str, enabled: bool) -> bool {
        let mut changed = false;
        if pad == "*" {
            changed |= self.default_enabled != enabled;
            self.default_enabled = enabled;
            for row in &mut self.rows {
                changed |= row.1 != enabled;
                row.1 = enabled;
            }
        } else if let Some(row) = self.rows.iter_mut().find(|r| r.0 == pad) {
            changed = row.1 != enabled;
            row.1 = enabled;
        }
        changed
    }

    pub fn enabled(&self, handle: &str) -> bool {
        self.rows.iter().any(|r| r.0 == handle && r.1)
    }

    pub fn snapshot(&self) -> Vec<(String, bool)> {
        self.rows.clone()
    }
}

/// One translated, deduplicated state change (or a registry change) out of
/// `GamepadIo::poll`. Names are the canonical [`crate::state`] vocabulary.
#[derive(Debug)]
pub enum Out {
    Button { handle: String, name: String, pressed: bool, value: f32 },
    Axis { handle: String, name: String, value: f32 },
    DevicesChanged,
}

impl Out {
    /// Both native backends emit through this (and the wasm seam shares
    /// [`wire_name`]), so the `/gamepad/in/*` vocabulary cannot diverge.
    pub fn from_pad_event(handle: &str, ev: PadEvent) -> Out {
        match ev {
            PadEvent::Button { idx, pressed, value } => Out::Button {
                handle: handle.to_string(),
                name: wire_name(button_name(idx), "button", idx),
                pressed,
                value,
            },
            PadEvent::Axis { idx, value } => Out::Axis {
                handle: handle.to_string(),
                name: wire_name(axis_name(idx), "axis", idx),
                value,
            },
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn registry_enable_semantics() {
        let mut r = Registry::default();
        r.connect("a");
        r.connect("b");

        assert!(r.set_enabled("a", false));
        assert!(!r.set_enabled("a", false)); // no-op → no broadcast
        assert!(!r.enabled("a"));
        assert!(r.enabled("b"));
        assert!(!r.enabled("ghost"));

        // "*" toggles everything and the default for future pads.
        assert!(r.set_enabled("*", false));
        assert!(!r.enabled("b"));
        r.connect("c");
        assert!(!r.enabled("c")); // inherits the disabled default
        assert!(r.set_enabled("*", true));
        assert!(r.enabled("a") && r.enabled("b") && r.enabled("c"));

        r.disconnect("b");
        assert_eq!(
            r.snapshot(),
            vec![("a".to_string(), true), ("c".to_string(), true)]
        );
    }

    #[test]
    fn from_pad_event_names_canonical_and_generic() {
        match Out::from_pad_event("p", PadEvent::Button { idx: 0, pressed: true, value: 1.0 }) {
            Out::Button { name, .. } => assert_eq!(name, "south"),
            other => panic!("unexpected {other:?}"),
        }
        match Out::from_pad_event("p", PadEvent::Axis { idx: 9, value: 0.5 }) {
            Out::Axis { name, .. } => assert_eq!(name, "axis_9"),
            other => panic!("unexpected {other:?}"),
        }
    }
}

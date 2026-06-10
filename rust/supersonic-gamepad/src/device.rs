//! gilrs-backed device IO: hotplug-tracked pad registry, event translation
//! into the shared [`crate::state`] diffing, and best-effort rumble.
//!
//! Pads are addressed by their normalised handle (see
//! `supersonic_osc::normalize`), assigned at connect time and stable for the
//! life of the connection (a later disconnect of a duplicate-named pad never
//! renames a live one). All of this runs on the FFI shell's poll thread —
//! gilrs itself never touches the audio thread.

use std::collections::HashMap;
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use gilrs::ff::{BaseEffect, BaseEffectType, Effect, EffectBuilder, Replay, Ticks};
use gilrs::{Axis, Button, EventType, GamepadId, Gilrs};

use crate::io::{assign_handle, Out, Registry};
use crate::state::{PadEvent, PadState};

struct Pad {
    handle: String,
    state: PadState,
}

struct RumbleSlot {
    handle: String,
    effect: Effect,
    deadline: Option<Instant>,
}

/// Owns the gilrs context, the connected-pad map and any live rumble effects.
pub struct GamepadIo {
    gilrs: Gilrs,
    registry: Arc<Mutex<Registry>>,
    pads: HashMap<GamepadId, Pad>,
    rumble: Vec<RumbleSlot>,
}

impl GamepadIo {
    pub fn new(registry: Arc<Mutex<Registry>>) -> Result<Self, gilrs::Error> {
        let gilrs = Gilrs::new()?;
        let mut io = GamepadIo { gilrs, registry, pads: HashMap::new(), rumble: Vec::new() };
        // Seed pads that were already connected at startup (gilrs also queues
        // Connected events for these on some platforms; connect_pad is
        // idempotent so both paths are safe).
        let ids: Vec<GamepadId> = io.gilrs.gamepads().map(|(id, _)| id).collect();
        for id in ids {
            io.connect_pad(id);
        }
        Ok(io)
    }

    /// Drain pending gilrs events into translated [`Out`]s, blocking up to
    /// `timeout` for the first one so the poll thread idles in the OS instead
    /// of spinning. Events from pads muted via `/gamepad/enable … 0` still
    /// update the diff state (so re-enabling doesn't replay a stale edge) but
    /// are not emitted.
    pub fn poll(&mut self, timeout: Duration) -> Vec<Out> {
        let mut out = Vec::new();
        let mut next = self.gilrs.next_event_blocking(Some(timeout));
        while let Some(ev) = next {
            self.translate(ev.id, ev.event, &mut out);
            next = self.gilrs.next_event();
        }
        out
    }

    fn translate(&mut self, id: GamepadId, ev: EventType, out: &mut Vec<Out>) {
        match ev {
            EventType::Connected => {
                if self.connect_pad(id) {
                    out.push(Out::DevicesChanged);
                }
            }
            EventType::Disconnected => {
                if let Some(pad) = self.pads.remove(&id) {
                    self.rumble.retain(|s| s.handle != pad.handle);
                    self.registry.lock().unwrap().disconnect(&pad.handle);
                    out.push(Out::DevicesChanged);
                }
            }
            // Edge events carry no value: presses land at the held analog
            // value (1.0 for a digital button that never saw a value event),
            // releases at the swept-down value (0.0 from full).
            EventType::ButtonPressed(b, _) => {
                if let Some(idx) = button_index(b) {
                    self.state_event(id, out, |s| {
                        let v = s.button_value(idx);
                        s.set_button(idx, true, if v > 0.0 { v } else { 1.0 })
                    });
                }
            }
            EventType::ButtonReleased(b, _) => {
                if let Some(idx) = button_index(b) {
                    self.state_event(id, out, |s| {
                        let v = s.button_value(idx);
                        s.set_button(idx, false, if v < 1.0 { v } else { 0.0 })
                    });
                }
            }
            EventType::ButtonChanged(b, value, _) => {
                if let Some(idx) = button_index(b) {
                    self.state_event(id, out, |s| s.set_button_value(idx, value));
                }
            }
            EventType::AxisChanged(a, value, _) => {
                if let Some(idx) = axis_index(a) {
                    self.state_event(id, out, |s| s.set_axis(idx, value));
                }
            }
            // Repeats are filter-generated (we install none); Dropped is
            // explicitly "ignore me"; FF completion needs no action.
            EventType::ButtonRepeated(..)
            | EventType::Dropped
            | EventType::ForceFeedbackEffectCompleted => {}
            // EventType is non_exhaustive.
            _ => {}
        }
    }

    /// Run one PadState update for `id` and emit the resulting event if the
    /// pad is connected, changed, and not muted.
    fn state_event(
        &mut self,
        id: GamepadId,
        out: &mut Vec<Out>,
        f: impl FnOnce(&mut PadState) -> Option<PadEvent>,
    ) {
        let Some(pad) = self.pads.get_mut(&id) else { return };
        let Some(ev) = f(&mut pad.state) else { return };
        if !self.registry.lock().unwrap().enabled(&pad.handle) {
            return; // muted: state updated, event suppressed
        }
        out.push(Out::from_pad_event(&pad.handle, ev));
    }

    /// Register a newly-connected pad: assign a stable deduped handle and add
    /// a registry row. Returns false if the pad was already known.
    fn connect_pad(&mut self, id: GamepadId) -> bool {
        if self.pads.contains_key(&id) {
            return false;
        }
        let Some(gamepad) = self.gilrs.connected_gamepad(id) else { return false };
        let handle =
            assign_handle(gamepad.name(), |h| self.pads.values().any(|p| p.handle == h));
        self.registry.lock().unwrap().connect(&handle);
        self.pads.insert(id, Pad { handle, state: PadState::new() });
        true
    }

    /// Start (or retrigger) rumble. Best-effort: pads without force feedback
    /// are skipped. `duration_ms <= 0` plays until `rumble_stop`.
    pub fn rumble(&mut self, pad: &str, strong: f32, weak: f32, duration_ms: i32) {
        let targets: Vec<(GamepadId, String)> = self
            .pads
            .iter()
            .filter(|(_, p)| pad == "*" || p.handle == pad)
            .map(|(id, p)| (*id, p.handle.clone()))
            .collect();
        for (id, handle) in targets {
            self.rumble.retain(|s| s.handle != handle); // drop = stop previous
            if !self.gilrs.connected_gamepad(id).map(|g| g.is_ff_supported()).unwrap_or(false) {
                continue;
            }
            let magnitude = |m: f32| (m.clamp(0.0, 1.0) * f32::from(u16::MAX)) as u16;
            // A short Replay repeated infinitely is continuous output; the
            // duration is enforced by `expire_rumble` dropping the effect.
            let scheduling = Replay { play_for: Ticks::from_ms(50), ..Default::default() };
            let built = EffectBuilder::new()
                .add_effect(BaseEffect {
                    kind: BaseEffectType::Strong { magnitude: magnitude(strong) },
                    scheduling,
                    ..Default::default()
                })
                .add_effect(BaseEffect {
                    kind: BaseEffectType::Weak { magnitude: magnitude(weak) },
                    scheduling,
                    ..Default::default()
                })
                .gamepads(&[id])
                .finish(&mut self.gilrs);
            if let Ok(effect) = built {
                if effect.play().is_ok() {
                    let deadline = (duration_ms > 0)
                        .then(|| Instant::now() + Duration::from_millis(duration_ms as u64));
                    self.rumble.push(RumbleSlot { handle, effect, deadline });
                }
            }
        }
    }

    pub fn rumble_stop(&mut self, pad: &str) {
        self.rumble.retain(|s| {
            let stop = pad == "*" || s.handle == pad;
            if stop {
                let _ = s.effect.stop();
            }
            !stop
        });
    }

    /// Stop rumble effects whose duration has elapsed. Called every poll tick,
    /// so bail before the clock read in the (usual) no-rumble case.
    pub fn expire_rumble(&mut self) {
        if self.rumble.is_empty() {
            return;
        }
        let now = Instant::now();
        self.rumble.retain(|s| {
            let expired = s.deadline.is_some_and(|d| now >= d);
            if expired {
                let _ = s.effect.stop();
            }
            !expired
        });
    }
}

/// gilrs Button → canonical [`crate::state::BUTTONS`] index. gilrs's
/// `LeftTrigger`/`RightTrigger` are the *shoulder bumpers* (L1/R1) and
/// `*Trigger2` the analog triggers (L2/R2) — mapped here onto the W3C names so
/// web and native agree. `C`/`Z`/`Unknown` have no canonical slot.
fn button_index(b: Button) -> Option<usize> {
    Some(match b {
        Button::South => 0,
        Button::East => 1,
        Button::West => 2,
        Button::North => 3,
        Button::LeftTrigger => 4,   // L1
        Button::RightTrigger => 5,  // R1
        Button::LeftTrigger2 => 6,  // L2
        Button::RightTrigger2 => 7, // R2
        Button::Select => 8,
        Button::Start => 9,
        Button::LeftThumb => 10,
        Button::RightThumb => 11,
        Button::DPadUp => 12,
        Button::DPadDown => 13,
        Button::DPadLeft => 14,
        Button::DPadRight => 15,
        Button::Mode => 16,
        _ => return None,
    })
}

fn axis_index(a: Axis) -> Option<usize> {
    Some(match a {
        Axis::LeftStickX => 0,
        Axis::LeftStickY => 1,
        Axis::RightStickX => 2,
        Axis::RightStickY => 3,
        Axis::DPadX => 4,
        Axis::DPadY => 5,
        _ => return None, // LeftZ/RightZ/Unknown: no stable cross-platform meaning
    })
}


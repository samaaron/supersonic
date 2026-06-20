//! macOS backend: Apple's GameController framework, polled.
//!
//! gilrs is unusable here — modern controllers (Xbox/PlayStation) are claimed
//! by Apple's own drivers (e.g. `XboxSeriesXGamepad`), which deliver input
//! exclusively through GameController.framework; an IOHIDManager client can
//! enumerate them but never receives input reports. So on macOS we poll each
//! pad's `extendedGamepad` profile and feed the readings through the same
//! shared [`crate::state`] diffing as the other backends — the
//! `/gamepad/in/*` stream is identical.
//!
//! Two platform requirements, both satisfied by the supersonic standalone:
//! * the **main CFRunLoop must be pumped** for controller discovery to
//!   deliver (src/native/Main.cpp does). In hosts that never pump it — test
//!   fixtures, and notably a BEAM/NIF embedding, where the runtime owns the
//!   main thread — the controller list simply stays empty: /gamepad/* stays
//!   serviceable but reports no devices. There is no way to detect the
//!   missing pump from here (it is indistinguishable from "no pads plugged
//!   in"), hence no diagnostic.
//! * `shouldMonitorBackgroundEvents` must be set, since the engine is never
//!   the focused application.
//!
//! Everything runs on the FFI shell's poll thread. Rumble is not implemented
//! on this backend (GCController exposes haptics via GCDeviceHaptics /
//! Core Haptics).

use std::ptr::NonNull;
use std::sync::{Arc, Mutex};
use std::time::Duration;

use block2::RcBlock;
use objc2::rc::{autoreleasepool, Retained};
use objc2_game_controller::{
    GCController, GCControllerAxisInput, GCControllerButtonInput, GCControllerElement, GCDevice,
    GCExtendedGamepad,
};

use crate::io::{assign_handle, Out, Registry};
use crate::state::PadState;

/// Input reads happen every poll tick; hotplug reconciliation (an ObjC
/// round-trip + allocations) only needs ~100ms-class latency, so it runs
/// every Nth tick (N × the ~4ms poll pace).
const SYNC_EVERY: u32 = 25;

struct Pad {
    ctrl: Retained<GCController>,
    handle: String,
    state: PadState,
    // Element handles cached at connect (they are immutable for the life of
    // the connection), keyed by canonical state-table index — the per-tick
    // work is then just the isPressed()/value() reads, not re-resolving ~20
    // property chains through objc_msgSend.
    buttons: Vec<(usize, Retained<GCControllerButtonInput>)>,
    axes: Vec<(usize, Retained<GCControllerAxisInput>)>,
}

impl Pad {
    fn ptr(&self) -> usize {
        Retained::as_ptr(&self.ctrl) as usize
    }
}

/// Owns the polled controller set. Same interface as the gilrs `GamepadIo`.
pub struct GamepadIo {
    registry: Arc<Mutex<Registry>>,
    pads: Vec<Pad>,
    tick: u32,
}

impl GamepadIo {
    pub fn new(registry: Arc<Mutex<Registry>>) -> Result<Self, ()> {
        // The engine process is never the focused app, so without this the
        // framework reports controllers but withholds their input.
        unsafe { GCController::setShouldMonitorBackgroundEvents(true) };
        Ok(GamepadIo { registry, pads: Vec::new(), tick: 0 })
    }

    /// Pace at `timeout`, then re-read every pad's cached elements through
    /// the shared state diffing (with a periodic hotplug reconciliation).
    /// Muted pads still update their diff state (so re-enabling doesn't
    /// replay a stale edge) but emit nothing.
    pub fn poll(&mut self, timeout: Duration) -> Vec<Out> {
        std::thread::sleep(timeout);
        // `GCController::controllers()` (called from `sync_controllers`) returns
        // an autoreleased NSArray. This thread has no run loop to drain the
        // autorelease pool, so without an explicit one it leaks. Drain per tick;
        // the Retained<_> handles we keep hold their own retain and survive it.
        autoreleasepool(|_| {
            let mut out = Vec::new();
            if self.tick % SYNC_EVERY == 0 {
                self.sync_controllers(&mut out);
            }
            self.tick = self.tick.wrapping_add(1);
            for i in 0..self.pads.len() {
                self.read_pad(i, &mut out);
            }
            out
        })
    }

    /// Reconcile our pad list against `GCController.controllers()`. The
    /// registry is only locked when something actually changed.
    fn sync_controllers(&mut self, out: &mut Vec<Out>) {
        let current: Vec<Retained<GCController>> =
            unsafe { GCController::controllers() }.to_vec();
        let live: Vec<usize> = current.iter().map(|c| Retained::as_ptr(c) as usize).collect();

        let removed: Vec<String> = self
            .pads
            .iter()
            .filter(|p| !live.contains(&p.ptr()))
            .map(|p| p.handle.clone())
            .collect();
        let added: Vec<Retained<GCController>> = current
            .into_iter()
            .filter(|c| {
                let ptr = Retained::as_ptr(c) as usize;
                !self.pads.iter().any(|p| p.ptr() == ptr)
            })
            .collect();
        if removed.is_empty() && added.is_empty() {
            return;
        }

        // Disconnects first, so a re-plugged duplicate can reclaim its name.
        let mut registry = self.registry.lock().unwrap();
        self.pads.retain(|p| !removed.contains(&p.handle));
        for handle in &removed {
            registry.disconnect(handle);
        }

        for ctrl in added {
            let raw = unsafe { ctrl.vendorName() }
                .map(|n| n.to_string())
                .unwrap_or_else(|| "controller".to_string());
            let handle = assign_handle(&raw, |h| self.pads.iter().any(|p| p.handle == h));
            let (buttons, axes) = cache_elements(&ctrl);
            registry.connect(&handle);
            self.pads.push(Pad { ctrl, handle, state: PadState::new(), buttons, axes });
        }
        drop(registry);

        out.push(Out::DevicesChanged);
    }

    /// Read one pad's cached elements into its diff state, emitting changes.
    fn read_pad(&mut self, idx: usize, out: &mut Vec<Out>) {
        let Pad { handle, state, buttons, axes, .. } = &mut self.pads[idx];

        let mut events = Vec::new();
        for (i, b) in buttons.iter() {
            events.extend(state.set_button(*i, unsafe { b.isPressed() }, unsafe { b.value() }));
        }
        for (i, a) in axes.iter() {
            events.extend(state.set_axis(*i, unsafe { a.value() }));
        }

        if events.is_empty() || !self.registry.lock().unwrap().enabled(handle) {
            return; // muted: state updated, events suppressed
        }
        for ev in events {
            out.push(Out::from_pad_event(handle, ev));
        }
    }

    /// Rumble is not implemented on this backend (GCDeviceHaptics /
    /// Core Haptics would carry it).
    pub fn rumble(&mut self, _pad: &str, _strong: f32, _weak: f32, _duration_ms: i32) {}
    pub fn rumble_stop(&mut self, _pad: &str) {}
    pub fn expire_rumble(&mut self) {}
}

/// Resolve and retain a pad's input elements, keyed by their canonical
/// [`crate::state`] indices, and install the no-op value-changed handler: the
/// framework only streams input into a profile once a handler is installed —
/// even a no-op one. We keep reading by polling; the handler (main-queue,
/// no-op) just turns delivery on. The setter copies the block, so the
/// temporary is safe to drop. A pad without an extended profile yields no
/// elements (listed, but silent).
#[allow(clippy::type_complexity)]
fn cache_elements(
    ctrl: &GCController,
) -> (
    Vec<(usize, Retained<GCControllerButtonInput>)>,
    Vec<(usize, Retained<GCControllerAxisInput>)>,
) {
    let mut buttons = Vec::new();
    let mut axes = Vec::new();
    let Some(ext) = (unsafe { ctrl.extendedGamepad() }) else { return (buttons, axes) };

    let noop = RcBlock::new(|_: NonNull<GCExtendedGamepad>, _: NonNull<GCControllerElement>| {});
    unsafe { ext.setValueChangedHandler(RcBlock::as_ptr(&noop)) };

    // Canonical indices — see crate::state::BUTTONS.
    unsafe {
        buttons.push((0, ext.buttonA())); //  south
        buttons.push((1, ext.buttonB())); //  east
        buttons.push((2, ext.buttonX())); //  west
        buttons.push((3, ext.buttonY())); //  north
        buttons.push((4, ext.leftShoulder()));
        buttons.push((5, ext.rightShoulder()));
        buttons.push((6, ext.leftTrigger()));
        buttons.push((7, ext.rightTrigger()));
        if let Some(b) = ext.buttonOptions() {
            buttons.push((8, b)); // select / view / share
        }
        buttons.push((9, ext.buttonMenu())); // start
        if let Some(b) = ext.leftThumbstickButton() {
            buttons.push((10, b));
        }
        if let Some(b) = ext.rightThumbstickButton() {
            buttons.push((11, b));
        }
        let dpad = ext.dpad();
        buttons.push((12, dpad.up()));
        buttons.push((13, dpad.down()));
        buttons.push((14, dpad.left()));
        buttons.push((15, dpad.right()));
        if let Some(b) = ext.buttonHome() {
            buttons.push((16, b)); // mode / guide / PS
        }
        // GCControllerAxisInput is already up/right-positive — the shared
        // canonical convention, no flip needed.
        let left = ext.leftThumbstick();
        axes.push((0, left.xAxis()));
        axes.push((1, left.yAxis()));
        let right = ext.rightThumbstick();
        axes.push((2, right.xAxis()));
        axes.push((3, right.yAxis()));
    }
    (buttons, axes)
}

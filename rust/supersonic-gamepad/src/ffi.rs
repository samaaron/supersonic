//! Native C ABI for the gamepad subsystem — the seam the C++ engine links
//! against (via the `supersonic-native` umbrella staticlib).
//!
//! The engine creates one instance, supplying one host callback, and feeds it
//! decoded-from-the-wire `/gamepad/*` OSC via [`ss_gamepad_handle_osc`]. The
//! subsystem owns its device IO (see [`crate::backend`]) on a dedicated poll
//! thread; it never touches the audio thread. Translated `/gamepad/in/*` events and
//! `/gamepad/devices` pushes flow back through the `emit` callback, which may
//! fire on the poll thread — the engine's implementation must be thread-safe
//! (it is: the egress ring).
//!
//! Replies (`/gamepad/devices.reply`) are emitted synchronously inside
//! [`ss_gamepad_handle_osc`] / [`ss_gamepad_emit_devices`] on the caller's
//! thread, while the engine's origin token still identifies the caller.

use std::ffi::c_void;
use std::slice;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::mpsc::{channel, Sender};
use std::sync::{Arc, Mutex};
use std::thread::JoinHandle;
use std::time::Duration;

use crate::backend::GamepadIo;
use crate::io::{Out, Registry};
use crate::schema::{
    decode_out, encode_axis, encode_button, encode_devices, encode_devices_reply, OutCommand,
};
// Emit-callback shape (here delivering `/gamepad/in/*` + `/gamepad/devices*`
// packets), kind codes and panic fence: shared across the subsystem C ABIs —
// see supersonic_osc::ffi.
pub use supersonic_osc::ffi::{no_unwind, EmitFn, EMIT_BROADCAST, EMIT_REPLY};

/// The host callback + opaque context, bundled and made Send/Sync so the poll
/// thread can hold a copy. Safety: the C++ engine guarantees `ctx` outlives
/// the instance and the callback is thread-safe.
#[derive(Clone, Copy)]
struct Host {
    ctx: *mut c_void,
    emit: EmitFn,
}
unsafe impl Send for Host {}
unsafe impl Sync for Host {}

impl Host {
    fn emit(&self, kind: i32, osc: &[u8]) {
        (self.emit)(self.ctx, kind, osc.as_ptr(), osc.len() as u32);
    }
}

/// The opaque handle the C++ side owns. Rumble commands need the backend's
/// device context, so they hop to the poll thread over `tx` as-is.
pub struct SsGamepad {
    host: Host,
    registry: Arc<Mutex<Registry>>,
    tx: Sender<OutCommand>,
    stop: Arc<AtomicBool>,
    join: Option<JoinHandle<()>>,
}

impl SsGamepad {
    fn handle(&self, cmd: OutCommand) {
        match cmd {
            cmd @ (OutCommand::Rumble { .. } | OutCommand::RumbleStop { .. }) => {
                let _ = self.tx.send(cmd);
            }

            // Enable toggles the shared registry directly (the poll thread
            // reads it per event), so the devices push reflects it immediately.
            OutCommand::Enable { pad, enabled } => {
                let changed = self.registry.lock().unwrap().set_enabled(&pad, enabled);
                if changed {
                    self.push_devices();
                }
            }
            OutCommand::DevicesList => self.reply_devices(),
            // The backends track hotplug themselves; refresh just re-broadcasts
            // the current snapshot for clients that want to resync.
            OutCommand::Refresh => self.push_devices(),

            // Subscription is an egress-audience concern owned by the C++ seam.
            OutCommand::Subscribe | OutCommand::Unsubscribe => {}
        }
    }

    fn reply_devices(&self) {
        let rows = self.registry.lock().unwrap().snapshot();
        self.host.emit(EMIT_REPLY, &encode_devices_reply(&rows));
    }

    fn push_devices(&self) {
        let rows = self.registry.lock().unwrap().snapshot();
        self.host.emit(EMIT_BROADCAST, &encode_devices(&rows));
    }
}

/// The poll-thread body: drain backend events → emit, run rumble commands,
/// expire rumble deadlines. The ~4 ms poll pace keeps worst-case added input
/// latency well under an audio buffer.
fn poll_loop(
    host: Host,
    registry: Arc<Mutex<Registry>>,
    rx: std::sync::mpsc::Receiver<OutCommand>,
    stop: Arc<AtomicBool>,
) {
    let mut io = match GamepadIo::new(registry.clone()) {
        Ok(io) => io,
        Err(_) => return, // no backend (rare): devices list stays empty
    };
    while !stop.load(Ordering::Acquire) {
        for out in io.poll(Duration::from_millis(4)) {
            match out {
                Out::Button { handle, name, pressed, value } => {
                    host.emit(EMIT_BROADCAST, &encode_button(&handle, &name, pressed, value));
                }
                Out::Axis { handle, name, value } => {
                    host.emit(EMIT_BROADCAST, &encode_axis(&handle, &name, value));
                }
                Out::DevicesChanged => {
                    let rows = registry.lock().unwrap().snapshot();
                    host.emit(EMIT_BROADCAST, &encode_devices(&rows));
                }
            }
        }
        for cmd in rx.try_iter() {
            match cmd {
                OutCommand::Rumble { pad, strong, weak, duration_ms } => {
                    io.rumble(&pad, strong, weak, duration_ms);
                }
                OutCommand::RumbleStop { pad } => io.rumble_stop(&pad),
                _ => {} // only rumble verbs are forwarded here
            }
        }
        io.expire_rumble();
    }
}

// ── C ABI ────────────────────────────────────────────────────────────────────

/// Create the gamepad subsystem. Returns an owning pointer (null on failure);
/// free with [`ss_gamepad_destroy`]. `ctx` and the callback must remain valid
/// until then.
#[no_mangle]
pub extern "C" fn ss_gamepad_create(ctx: *mut c_void, emit: EmitFn) -> *mut SsGamepad {
    no_unwind(std::ptr::null_mut(), || {
        let host = Host { ctx, emit };
        let registry = Arc::new(Mutex::new(Registry::default()));
        let stop = Arc::new(AtomicBool::new(false));
        let (tx, rx) = channel();

        let thread_host = host;
        let thread_registry = registry.clone();
        let thread_stop = stop.clone();
        let join = std::thread::Builder::new()
            .name("supersonic-gamepad".into())
            .spawn(move || {
                // The loop body must never unwind into the OS thread shim.
                no_unwind((), || poll_loop(thread_host, thread_registry, rx, thread_stop));
            })
            .ok();

        Box::into_raw(Box::new(SsGamepad { host, registry, tx, stop, join }))
    })
}

/// Destroy the subsystem: stops the poll thread (≤ ~4 ms), dropping all rumble
/// effects and the backend's device context with it.
#[no_mangle]
pub unsafe extern "C" fn ss_gamepad_destroy(handle: *mut SsGamepad) {
    if handle.is_null() {
        return;
    }
    no_unwind((), || {
        let mut me = Box::from_raw(handle);
        me.stop.store(true, Ordering::Release);
        if let Some(join) = me.join.take() {
            let _ = join.join();
        }
    });
}

/// Feed one decoded `/gamepad/*` OSC packet (the C++ seam forwards these off
/// the audio thread). Unknown/foreign addresses are ignored.
#[no_mangle]
pub unsafe extern "C" fn ss_gamepad_handle_osc(handle: *mut SsGamepad, data: *const u8, len: u32) {
    if handle.is_null() || data.is_null() {
        return;
    }
    let me = &*handle;
    let bytes = slice::from_raw_parts(data, len as usize);
    no_unwind((), || {
        if let Some(cmd) = decode_out(bytes) {
            me.handle(cmd);
        }
    });
}

/// Emit a fresh `/gamepad/devices.reply` to the caller — used by the C++ seam
/// to send a device snapshot to a newly-subscribed client.
#[no_mangle]
pub unsafe extern "C" fn ss_gamepad_emit_devices(handle: *mut SsGamepad) {
    if handle.is_null() {
        return;
    }
    let me = &*handle;
    no_unwind((), || me.reply_devices());
}

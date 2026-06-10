//! Native C ABI for the gamepad subsystem — the seam the C++ engine links
//! against (via the `supersonic-native` umbrella staticlib).
//!
//! The engine creates one instance, supplying one host callback, and feeds it
//! decoded-from-the-wire `/gamepad/*` OSC via [`ss_gamepad_handle_osc`].
//! Translated `/gamepad/in/*` events and `/gamepad/devices` pushes flow back
//! through the `emit` callback, which may fire on the poll thread — the
//! engine's implementation must be thread-safe (it is: the egress ring).
//!
//! The device IO (see [`crate::backend`]) is **process-global**: one poll
//! thread and one device context, created on first use and kept for the life
//! of the process. gilrs is built as a per-process singleton — its Linux
//! backend spawns a hotplug thread with no shutdown path, so tearing down and
//! recreating the context leaks a thread + fds each time. Instances are
//! host registrations: create installs the engine's callback, destroy removes
//! it (the poll thread then parks, keeping the device registry warm for the
//! next instance). Destroy synchronises on the host lock, so no callback
//! fires after it returns.
//!
//! Replies (`/gamepad/devices.reply`) are emitted synchronously inside
//! [`ss_gamepad_handle_osc`] / [`ss_gamepad_emit_devices`] on the caller's
//! thread, while the engine's origin token still identifies the caller.

use std::ffi::c_void;
use std::slice;
use std::sync::mpsc::{channel, Sender};
use std::sync::{Arc, Mutex, OnceLock};
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

/// The process-global subsystem: poll thread, device registry, rumble channel
/// and the currently-registered host (None = parked, events discarded).
struct Global {
    registry: Arc<Mutex<Registry>>,
    active: Arc<Mutex<Option<Host>>>,
    tx: Sender<OutCommand>,
}

static GLOBAL: OnceLock<Global> = OnceLock::new();

fn global() -> &'static Global {
    GLOBAL.get_or_init(|| {
        let registry = Arc::new(Mutex::new(Registry::default()));
        let active: Arc<Mutex<Option<Host>>> = Arc::new(Mutex::new(None));
        let (tx, rx) = channel();

        let thread_registry = registry.clone();
        let thread_active = active.clone();
        // The poll thread runs for the life of the process (matching the
        // device backends' own lifetime expectations). Errors are best-effort:
        // if the backend can't start, the registry simply stays empty.
        let _ = std::thread::Builder::new().name("supersonic-gamepad".into()).spawn(move || {
            // The loop body must never unwind into the OS thread shim.
            no_unwind((), || poll_loop(thread_active, thread_registry, rx));
        });

        Global { registry, active, tx }
    })
}

/// The opaque handle the C++ side owns: a registration against the global
/// subsystem carrying the engine's callback (also used directly for the
/// synchronous replies).
pub struct SsGamepad {
    host: Host,
}

impl SsGamepad {
    fn handle(&self, cmd: OutCommand) {
        match cmd {
            // Rumble needs the backend's device context: hop to the poll thread.
            cmd @ (OutCommand::Rumble { .. } | OutCommand::RumbleStop { .. }) => {
                let _ = global().tx.send(cmd);
            }

            // Enable toggles the shared registry directly (the poll thread
            // reads it per event), so the devices push reflects it immediately.
            OutCommand::Enable { pad, enabled } => {
                let changed = global().registry.lock().unwrap().set_enabled(&pad, enabled);
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
        let rows = global().registry.lock().unwrap().snapshot();
        self.host.emit(EMIT_REPLY, &encode_devices_reply(&rows));
    }

    fn push_devices(&self) {
        let rows = global().registry.lock().unwrap().snapshot();
        self.host.emit(EMIT_BROADCAST, &encode_devices(&rows));
    }
}

/// The poll-thread body: drain backend events → emit to the registered host,
/// run rumble commands, expire rumble deadlines. The ~4 ms poll pace keeps
/// worst-case added input latency well under an audio buffer; with no host
/// registered the thread parks at a lazy tick, keeping the registry warm.
/// Emission holds the host lock, so `ss_gamepad_destroy`'s host removal
/// strictly orders against any in-flight callback.
fn poll_loop(
    active: Arc<Mutex<Option<Host>>>,
    registry: Arc<Mutex<Registry>>,
    rx: std::sync::mpsc::Receiver<OutCommand>,
) {
    let mut io = match GamepadIo::new(registry.clone()) {
        Ok(io) => io,
        Err(_) => return, // no backend (rare): devices list stays empty
    };
    loop {
        let parked = active.lock().unwrap().is_none();
        let pace = if parked { Duration::from_millis(250) } else { Duration::from_millis(4) };
        for out in io.poll(pace) {
            let guard = active.lock().unwrap();
            let Some(host) = *guard else { continue }; // parked: discard
            match out {
                Out::Button { ref handle, ref name, pressed, value } => {
                    host.emit(EMIT_BROADCAST, &encode_button(handle, name, pressed, value));
                }
                Out::Axis { ref handle, ref name, value } => {
                    host.emit(EMIT_BROADCAST, &encode_axis(handle, name, value));
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

/// Create a gamepad-subsystem registration: installs `emit` as the active
/// host (starting the process-global device IO on first use). Returns an
/// owning pointer (null on failure); free with [`ss_gamepad_destroy`]. `ctx`
/// and the callback must remain valid until then. One registration at a time:
/// a second create supersedes the first (the engine is single-per-process).
#[no_mangle]
pub extern "C" fn ss_gamepad_create(ctx: *mut c_void, emit: EmitFn) -> *mut SsGamepad {
    no_unwind(std::ptr::null_mut(), || {
        let host = Host { ctx, emit };
        *global().active.lock().unwrap() = Some(host);
        Box::into_raw(Box::new(SsGamepad { host }))
    })
}

/// Remove the registration and stop all rumble. The poll thread and device
/// context persist (parked) for the life of the process; once this returns no
/// further callbacks fire (host removal synchronises on the emission lock).
#[no_mangle]
pub unsafe extern "C" fn ss_gamepad_destroy(handle: *mut SsGamepad) {
    if handle.is_null() {
        return;
    }
    no_unwind((), || {
        let me = Box::from_raw(handle);
        let g = global();
        let mut active = g.active.lock().unwrap();
        // Only deregister if we are still the active host (a newer instance
        // may have superseded this one).
        if active.map(|h| h.ctx) == Some(me.host.ctx) {
            *active = None;
        }
        drop(active);
        let _ = g.tx.send(OutCommand::RumbleStop { pad: "*".into() });
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

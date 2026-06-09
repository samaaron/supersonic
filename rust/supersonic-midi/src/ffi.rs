//! Native C ABI for the MIDI subsystem — the seam the C++ engine links against.
//!
//! The engine creates one instance, supplying three host callbacks, and feeds it
//! decoded-from-the-wire `/midi/*` OSC via [`ss_midi_handle_osc`]. The subsystem
//! owns its midir device IO (and midir's own input thread); it never touches the
//! audio thread. Clock-OUT generation lives engine-side in MidiClockOut.
//!
//! Data flows back to the engine through the callbacks:
//! * `emit`      — a `/midi/in/*` (or `/midi/ports*`) OSC packet to inject into
//!                 the RT IN ring / broadcast to subscribers.
//! * `clock`     — one 0xF8 pulse → SuperClock (pulse-count beat + estimate).
//! * `transport` — Start/Continue/Stop/SongPosition → SuperClock transport.
//!
//! Callbacks may fire on the midir input thread, so the engine's implementations
//! must be thread-safe.

use std::collections::HashSet;
use std::ffi::c_void;
use std::slice;
use std::sync::{Arc, Mutex};

use crate::device::{InputCallback, MidiIo};
use crate::message::MidiMessage;
use crate::schema::{decode_out, encode_in, encode_ports, encode_ports_reply, OutCommand};
use crate::sync::{transport_event, TransportEvent};

/// Emit an OSC packet to the engine. `kind` is [`EMIT_BROADCAST`] (fan out to
/// the /midi/notify audience — `/midi/in/*`, `/midi/ports`) or [`EMIT_REPLY`]
/// (reply to the current caller — `/midi/ports.reply`).
pub type EmitFn = extern "C" fn(ctx: *mut c_void, kind: i32, osc: *const u8, len: u32);

/// `kind` codes for [`EmitFn`].
pub const EMIT_BROADCAST: i32 = 0;
pub const EMIT_REPLY: i32 = 1;
/// One MIDI clock pulse (0xF8) for an input port → the engine, which anchors the
/// timeline beat on the pulse count and estimates tempo engine-side (not here).
/// `norm` is the normalised handle the engine keys the timeline on; `raw` is the
/// friendly OS device name for display; `ts_us` is the pulse's OS timestamp (µs).
/// Strings are not NUL-terminated; all args valid only during the call.
pub type ClockFn = extern "C" fn(
    ctx: *mut c_void,
    norm: *const u8, norm_len: u32,
    raw: *const u8, raw_len: u32,
    ts_us: u64,
);
/// Transport intent for one input port: kind 0=Start 1=Continue 2=Stop
/// 3=Position; `beat` is the target beat for Start/Position, `-1` otherwise.
/// `norm`/`raw` as in [`ClockFn`].
pub type TransportFn = extern "C" fn(
    ctx: *mut c_void,
    norm: *const u8, norm_len: u32,
    raw: *const u8, raw_len: u32,
    kind: i32, beat: f64,
);

/// Transport `kind` codes shared with the C++ side.
pub const TRANSPORT_START: i32 = 0;
pub const TRANSPORT_CONTINUE: i32 = 1;
pub const TRANSPORT_STOP: i32 = 2;
pub const TRANSPORT_POSITION: i32 = 3;

/// The host callbacks + opaque context, bundled and made Send/Sync so the helper
/// threads can hold a copy. Safety: the C++ engine guarantees `ctx` outlives the
/// instance and the callbacks are thread-safe.
#[derive(Clone, Copy)]
struct Host {
    ctx: *mut c_void,
    emit: EmitFn,
    clock: ClockFn,
    transport: TransportFn,
}
unsafe impl Send for Host {}
unsafe impl Sync for Host {}

impl Host {
    fn emit(&self, kind: i32, osc: &[u8]) {
        (self.emit)(self.ctx, kind, osc.as_ptr(), osc.len() as u32);
    }
    fn clock(&self, norm: &str, raw: &str, ts_us: u64) {
        (self.clock)(
            self.ctx,
            norm.as_ptr(), norm.len() as u32,
            raw.as_ptr(), raw.len() as u32,
            ts_us,
        );
    }
    fn transport(&self, norm: &str, raw: &str, kind: i32, beat: f64) {
        (self.transport)(
            self.ctx,
            norm.as_ptr(), norm.len() as u32,
            raw.as_ptr(), raw.len() as u32,
            kind, beat,
        );
    }
}

/// Shared with the midir input thread. `muted` holds ports the host has
/// explicitly opted out of clock-following via `/midi/clock/sync … 0`; every
/// other enabled input that sends 0xF8 drives its own engine-side timeline.
#[derive(Default)]
struct InputState {
    muted: HashSet<String>,
}

/// The opaque handle the C++ side owns.
pub struct SsMidi {
    // Declared first so it drops first: stopping the watcher's thread before the
    // rest tears down guarantees no refresh runs against half-dropped state.
    _watcher: crate::watcher::Watcher,
    host: Host,
    io: Arc<Mutex<MidiIo>>,
    input: Arc<Mutex<InputState>>,
}

impl SsMidi {
    fn handle(&self, cmd: OutCommand) {
        match cmd {
            OutCommand::Send { port, msg } => {
                let mut io = self.io.lock().unwrap();
                // channel 0 is the "all channels" sentinel (wire channel -1):
                // fan a channel-voice message out to all 16 channels.
                if msg.channel() == Some(0) {
                    for ch in 1..=16 {
                        io.send(&port, &msg.with_channel(ch).encode());
                    }
                } else {
                    io.send(&port, &msg.encode());
                }
            }
            OutCommand::SendRaw { port, bytes } => self.io.lock().unwrap().send(&port, &bytes),

            // One immediate clock pulse. Continuous-clock generation + transport
            // + beat-bursts are owned by the engine's SuperClock-timed
            // MidiClockOut (C++); its generated pulses arrive here as ClockTick.
            OutCommand::ClockTick { port } => self.io.lock().unwrap().send(&port, &[0xF8]),
            // Per-port clock-follow toggle. Every enabled input is tracked by
            // default; this opts a port out (mute) or back in. Muting stops the
            // pulse feed so its engine-side timeline goes stale and is reclaimed.
            OutCommand::ClockSync { port, enabled } => {
                let mut is = self.input.lock().unwrap();
                if enabled {
                    is.muted.remove(&port);
                } else {
                    is.muted.insert(port);
                }
            }

            OutCommand::Enable { port, input, enabled } => {
                {
                    let mut io = self.io.lock().unwrap();
                    if port == "*" {
                        io.enable_all(input, enabled);
                    } else if input {
                        io.enable_input(&port, enabled);
                    } else {
                        io.enable_output(&port, enabled);
                    }
                }
                self.push_ports();
            }
            OutCommand::Refresh => {
                self.io.lock().unwrap().refresh();
                self.push_ports();
            }
            OutCommand::PortsList => self.reply_ports(),

            // Subscription is an egress-audience concern owned by the C++ seam.
            OutCommand::Subscribe | OutCommand::Unsubscribe => {}
        }
    }

    fn reply_ports(&self) {
        let (ins, outs) = self.io.lock().unwrap().port_lists();
        self.host.emit(EMIT_REPLY, &encode_ports_reply(&ins, &outs));
    }

    fn push_ports(&self) {
        let (ins, outs) = self.io.lock().unwrap().port_lists();
        self.host.emit(EMIT_BROADCAST, &encode_ports(&ins, &outs));
    }
}

/// Handle one inbound message (runs on midir's input thread). `norm` is the
/// normalised port handle (estimator key + /midi/in address + timeline key);
/// `raw` is the friendly OS name passed through for timeline labelling.
fn handle_input(host: &Host, input: &Arc<Mutex<InputState>>,
                norm: &str, raw: &str, ts_us: u64, bytes: &[u8]) {
    let msg = match MidiMessage::parse(bytes) {
        Some(m) => m,
        None => return,
    };

    // Clock pulses feed each port's engine-side timeline (beat + tempo are
    // computed in SuperClock), so we just forward every 0xF8 with its OS
    // timestamp unless the port is muted. They never enter the /midi/in ring.
    if msg.is_clock_pulse() {
        if input.lock().unwrap().muted.contains(norm) {
            return; // opted out via /midi/clock/sync — don't drive a timeline
        }
        host.clock(norm, raw, ts_us);
        return;
    }

    // Transport/position drives the originating port's timeline (unless muted).
    if let Some(ev) = transport_event(&msg) {
        let muted = input.lock().unwrap().muted.contains(norm);
        if !muted {
            let (kind, beat) = transport_code(ev);
            host.transport(norm, raw, kind, beat);
        }
    }

    // Everything else surfaces as a /midi/in/* event for the engine/clients.
    if let Some(osc) = encode_in(norm, &msg) {
        host.emit(EMIT_BROADCAST, &osc);
    }
}

fn transport_code(ev: TransportEvent) -> (i32, f64) {
    match ev {
        TransportEvent::Start => (TRANSPORT_START, 0.0),
        TransportEvent::Continue => (TRANSPORT_CONTINUE, -1.0),
        TransportEvent::Stop => (TRANSPORT_STOP, -1.0),
        TransportEvent::Position { .. } => (TRANSPORT_POSITION, ev.beat().unwrap_or(-1.0)),
    }
}

// ── C ABI ────────────────────────────────────────────────────────────────────

/// Create the MIDI subsystem. Returns an owning pointer; free with
/// [`ss_midi_destroy`]. The callbacks and `ctx` must remain valid until then.
#[no_mangle]
pub extern "C" fn ss_midi_create(
    ctx: *mut c_void,
    emit: EmitFn,
    clock: ClockFn,
    transport: TransportFn,
) -> *mut SsMidi {
    let host = Host {
        ctx,
        emit,
        clock,
        transport,
    };

    let input = Arc::new(Mutex::new(InputState::default()));

    let in_state = input.clone();
    let on_input: InputCallback = Arc::new(move |norm: &str, raw: &str, ts: u64, bytes: &[u8]| {
        handle_input(&host, &in_state, norm, raw, ts, bytes);
    });
    let io = Arc::new(Mutex::new(MidiIo::new("SuperSonic", on_input)));

    // Native hot-swap: the OS device-change notification (WinRT DeviceWatcher /
    // CoreMIDI notify / ALSA announce) re-enumerates and diff-broadcasts
    // `/midi/ports` directly — no JUCE, no audio-thread, no polling.
    let watch_host = host;
    let watch_io = io.clone();
    let on_change: crate::watcher::OnChange =
        Arc::new(move || refresh_and_broadcast(&watch_host, &watch_io));
    let watcher = crate::watcher::Watcher::new(on_change);

    Box::into_raw(Box::new(SsMidi {
        _watcher: watcher,
        host,
        io,
        input,
    }))
}

/// Destroy the subsystem: closes all ports.
#[no_mangle]
pub unsafe extern "C" fn ss_midi_destroy(handle: *mut SsMidi) {
    if handle.is_null() {
        return;
    }
    // Box dropped here → `io` drops → all midir connections close.
    drop(Box::from_raw(handle));
}

/// Feed one decoded `/midi/*` OSC packet (the C++ seam forwards these off the
/// audio thread). Unknown/foreign addresses are ignored.
#[no_mangle]
pub unsafe extern "C" fn ss_midi_handle_osc(handle: *mut SsMidi, data: *const u8, len: u32) {
    if handle.is_null() || data.is_null() {
        return;
    }
    let me = &*handle;
    let bytes = slice::from_raw_parts(data, len as usize);
    if let Some(cmd) = decode_out(bytes) {
        me.handle(cmd);
    }
}

/// Emit a fresh `/midi/ports.reply` to the caller — used by the C++ seam to send
/// a device snapshot to a newly-subscribed client.
#[no_mangle]
pub unsafe extern "C" fn ss_midi_emit_ports(handle: *mut SsMidi) {
    if handle.is_null() {
        return;
    }
    (*handle).reply_ports();
}

/// Re-enumerate devices and, only if the port list actually changed, broadcast
/// the updated `/midi/ports` to subscribers. Shared by the C ABI [`ss_midi_refresh`]
/// and the native hot-swap [`crate::watcher::Watcher`]; the change check keeps a
/// device-event storm from spamming `/midi/ports`.
fn refresh_and_broadcast(host: &Host, io: &Mutex<MidiIo>) {
    let updated = {
        let mut io = io.lock().unwrap();
        let before = io.port_lists();
        io.refresh();
        let after = io.port_lists();
        if after != before { Some(after) } else { None }
    };
    if let Some((ins, outs)) = updated {
        host.emit(EMIT_BROADCAST, &encode_ports(&ins, &outs));
    }
}

/// Re-enumerate devices and, only if the port list actually changed, broadcast
/// the updated `/midi/ports` to subscribers. Retained for the manual
/// `/midi/refresh` seam; native hot-swap is driven by the watcher (see
/// [`crate::watcher`]).
#[no_mangle]
pub unsafe extern "C" fn ss_midi_refresh(handle: *mut SsMidi) {
    if handle.is_null() {
        return;
    }
    let me = &*handle;
    refresh_and_broadcast(&me.host, &me.io);
}

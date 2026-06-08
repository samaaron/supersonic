//! End-to-end accuracy of the MIDI subsystem over *real* system MIDI (CoreMIDI/
//! ALSA virtual ports), driving the C ABI exactly as the engine does.
//!
//!   * incoming clock — a precise virtual clock master, BPM estimate checked
//!   * note round-trip — /midi/out → device and device → /midi/in
//!
//! (Outgoing clock generation lives engine-side in C++ MidiClockOut, covered by
//! the native MidiClockGen / MidiClockOut tests.)
//!
//! Ignored by default (needs virtual MIDI + real time). Run on a dev box with:
//!     cargo test --test clock_accuracy -- --ignored --nocapture
#![cfg(not(target_arch = "wasm32"))]

use std::ffi::c_void;
use std::sync::Mutex;
use std::time::{Duration, Instant};

use midir::os::unix::{VirtualInput, VirtualOutput};
use midir::{MidiInput, MidiOutput};

use supersonic_midi::ffi::{ss_midi_create, ss_midi_destroy, ss_midi_handle_osc, SsMidi};
use supersonic_midi::osc::{encode, OscArg};

/// Host context shared with the C ABI callbacks: captures the BPM estimates and
/// emitted OSC packets the subsystem produces.
struct Ctx {
    tempo: Mutex<Vec<f64>>,
    events: Mutex<Vec<Vec<u8>>>,
}

extern "C" fn emit_cb(ctx: *mut c_void, _kind: i32, osc: *const u8, len: u32) {
    let c = unsafe { &*(ctx as *const Ctx) };
    c.events
        .lock()
        .unwrap()
        .push(unsafe { std::slice::from_raw_parts(osc, len as usize) }.to_vec());
}
extern "C" fn tempo_cb(ctx: *mut c_void, bpm: f64) {
    let c = unsafe { &*(ctx as *const Ctx) };
    c.tempo.lock().unwrap().push(bpm);
}
extern "C" fn transport_cb(_ctx: *mut c_void, _kind: i32, _beat: f64) {}

fn new_ctx() -> *mut Ctx {
    Box::into_raw(Box::new(Ctx {
        tempo: Mutex::new(Vec::new()),
        events: Mutex::new(Vec::new()),
    }))
}

fn create(ctx: *mut Ctx) -> *mut SsMidi {
    ss_midi_create(ctx as *mut c_void, emit_cb, tempo_cb, transport_cb)
}

fn send(h: *mut SsMidi, addr: &str, args: &[OscArg]) {
    let bytes = encode(addr, args);
    unsafe { ss_midi_handle_osc(h, bytes.as_ptr(), bytes.len() as u32) };
}

fn mean_std(xs: &[f64]) -> (f64, f64) {
    let mean = xs.iter().sum::<f64>() / xs.len() as f64;
    let var = xs.iter().map(|x| (x - mean).powi(2)).sum::<f64>() / xs.len() as f64;
    (mean, var.sqrt())
}

#[test]
#[ignore = "needs virtual MIDI + real time; run with --ignored --nocapture"]
fn incoming_clock_bpm_accuracy() {
    let bpm = 120.0;
    let interval = Duration::from_secs_f64(60.0 / bpm / 24.0);

    // A virtual source feeding the engine a precise clock.
    let vout = MidiOutput::new("ss-test").unwrap();
    let mut conn = vout.create_virtual("ss-clk-in").unwrap();

    let ctx = new_ctx();
    let h = create(ctx);
    send(h, "/midi/in/enable", &[OscArg::Str("ss-clk-in".into()), OscArg::Int(1)]);
    send(h, "/midi/clock/sync", &[OscArg::Str("ss-clk-in".into()), OscArg::Int(1)]);

    // Busy-wait to each target instant for precise inter-pulse timing.
    let mut next = Instant::now();
    let mut sent = Vec::new();
    for _ in 0..240 {
        next += interval;
        while Instant::now() < next {}
        conn.send(&[0xF8]).unwrap();
        sent.push(Instant::now());
    }
    std::thread::sleep(Duration::from_millis(50));
    unsafe { ss_midi_destroy(h) };

    // True rate of our (jittery) sends, for a fair accuracy comparison.
    let send_intervals: Vec<f64> = sent.windows(2).map(|w| (w[1] - w[0]).as_secs_f64()).collect();
    let (mean_iv, _) = mean_std(&send_intervals);
    let actual_bpm = 60.0 / (mean_iv * 24.0);

    let tempo = unsafe { &*(ctx) }.tempo.lock().unwrap().clone();
    assert!(!tempo.is_empty(), "no BPM estimate produced");
    let est = *tempo.last().unwrap();
    println!(
        "clock IN: target {bpm} bpm, actual-sent {actual_bpm:.3} bpm, estimate {est:.3} bpm (updates={})",
        tempo.len()
    );
    assert!((est - actual_bpm).abs() < 2.0, "estimate {est:.3} vs actual {actual_bpm:.3}");
}

#[test]
#[ignore = "needs virtual MIDI; run with --ignored --nocapture"]
fn note_round_trip_through_ffi() {
    // device → engine: a virtual source the engine reads as input.
    let vout = MidiOutput::new("ss-test").unwrap();
    let mut src = vout.create_virtual("ss-rt-in").unwrap();
    // engine → device: a virtual destination the engine writes as output.
    let got: std::sync::Arc<Mutex<Vec<Vec<u8>>>> = std::sync::Arc::new(Mutex::new(Vec::new()));
    let sink = got.clone();
    let vin = MidiInput::new("ss-test").unwrap();
    let _dst = vin
        .create_virtual("ss-rt-out", move |_t, b, _| sink.lock().unwrap().push(b.to_vec()), ())
        .unwrap();

    let ctx = new_ctx();
    let h = create(ctx);
    send(h, "/midi/in/enable", &[OscArg::Str("ss-rt-in".into()), OscArg::Int(1)]);
    send(h, "/midi/out/enable", &[OscArg::Str("ss-rt-out".into()), OscArg::Int(1)]);

    // IN: device sends note-on → engine emits /midi/in/note_on.
    src.send(&[0x90, 60, 100]).unwrap();
    // OUT: engine sends note-on → device receives the bytes.
    send(
        h,
        "/midi/out/note_on",
        &[OscArg::Str("ss-rt-out".into()), OscArg::Int(1), OscArg::Int(64), OscArg::Int(99)],
    );
    std::thread::sleep(Duration::from_millis(200));
    unsafe { ss_midi_destroy(h) };

    let events = unsafe { &*(ctx) }.events.lock().unwrap().clone();
    assert!(
        events.iter().any(|e| {
            supersonic_midi::osc::decode(e).map(|m| m.addr) == Some("/midi/in/note_on".into())
        }),
        "expected /midi/in/note_on, got {} events",
        events.len()
    );
    let got = got.lock().unwrap();
    assert!(
        got.iter().any(|b| b == &[0x90, 64, 99]),
        "expected note-on at device, got {got:?}"
    );
}

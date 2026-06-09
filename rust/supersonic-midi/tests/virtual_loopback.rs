//! In-process MIDI loopback over CoreMIDI/ALSA virtual ports, exercising the
//! real `device::MidiIo` byte path in both directions.
//!
//! Ignored by default: virtual ports need CoreMIDI/ALSA (not Windows) and the
//! delivery is asynchronous, so this is timing-dependent and unsuitable for CI.
//! Run on a dev machine with:
//!     cargo test --test virtual_loopback -- --ignored
#![cfg(not(target_arch = "wasm32"))]

use std::sync::{Arc, Mutex};
use std::time::Duration;

use midir::os::unix::{VirtualInput, VirtualOutput};
use midir::{MidiInput, MidiOutput};

use supersonic_midi::device::MidiIo;
use supersonic_midi::message::MidiMessage;

#[test]
#[ignore = "needs CoreMIDI/ALSA virtual ports; run with --ignored on a dev box"]
fn input_loopback_via_virtual_port() {
    // A virtual *source* the subsystem will enumerate as an input port.
    let vout = MidiOutput::new("ss-test-src").unwrap();
    let mut vconn = vout.create_virtual("ss-loop-in").unwrap();

    let got: Arc<Mutex<Vec<(String, Vec<u8>)>>> = Arc::new(Mutex::new(Vec::new()));
    let sink = got.clone();
    let mut io = MidiIo::new(
        "SuperSonic-test",
        Arc::new(move |port: &str, _raw: &str, _ts, bytes: &[u8]| {
            sink.lock().unwrap().push((port.to_string(), bytes.to_vec()));
        }),
    );
    io.refresh();
    assert!(io.enable_input("ss-loop-in", true), "open virtual input port");

    let expected = MidiMessage::NoteOn { channel: 1, note: 60, velocity: 100 };
    vconn.send(&expected.encode()).unwrap();
    std::thread::sleep(Duration::from_millis(200));

    let got = got.lock().unwrap();
    assert!(
        got.iter()
            .any(|(p, b)| p == "ss-loop-in" && MidiMessage::parse(b) == Some(expected.clone())),
        "expected note-on loopback, got {got:?}"
    );
}

#[test]
#[ignore = "needs CoreMIDI/ALSA virtual ports; run with --ignored on a dev box"]
fn output_loopback_via_virtual_port() {
    // A virtual *destination* the subsystem will enumerate as an output port.
    let got: Arc<Mutex<Vec<Vec<u8>>>> = Arc::new(Mutex::new(Vec::new()));
    let sink = got.clone();
    let vin = MidiInput::new("ss-test-dst").unwrap();
    let _vconn = vin
        .create_virtual(
            "ss-loop-out",
            move |_ts, bytes, _| sink.lock().unwrap().push(bytes.to_vec()),
            (),
        )
        .unwrap();

    let mut io = MidiIo::new("SuperSonic-test", Arc::new(|_, _, _, _: &[u8]| {}));
    io.refresh();
    assert!(io.enable_output("ss-loop-out", true), "open virtual output port");

    let expected = MidiMessage::ControlChange { channel: 1, controller: 7, value: 42 };
    io.send("ss-loop-out", &expected.encode());
    std::thread::sleep(Duration::from_millis(200));

    let got = got.lock().unwrap();
    assert!(
        got.iter().any(|b| MidiMessage::parse(b) == Some(expected.clone())),
        "expected control-change loopback, got {got:?}"
    );
}

//! Virtual MIDI clock CAPTURE — the counterpart to clock_gen, for testing
//! `midi_clock_out`. Creates a virtual MIDI *destination* ("SP Clock Capture")
//! that other apps (Sonic Pi / SuperSonic) see as a MIDI OUTPUT port and can
//! send clock to. Counts incoming 0xF8 timing-clock bytes and prints the derived
//! BPM once a second, plus any transport (Start/Stop/Continue) bytes.
//!
//! Run (macOS / Linux):
//!     cargo run -p supersonic-midi --example clock_capture
//! then point a clock-out at "SP Clock Capture".
#![cfg(not(target_arch = "wasm32"))]

use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::Arc;
use std::time::Duration;

#[cfg(unix)]
use midir::os::unix::VirtualInput;
use midir::MidiInput;

const PORT_NAME: &str = "SP Clock Capture";

fn main() {
    #[cfg(not(unix))]
    {
        eprintln!("clock_capture needs virtual MIDI ports (macOS/Linux only).");
        std::process::exit(1);
    }

    #[cfg(unix)]
    {
        let pulses = Arc::new(AtomicU64::new(0)); // 0xF8 count
        let starts = Arc::new(AtomicU64::new(0)); // 0xFA
        let stops  = Arc::new(AtomicU64::new(0)); // 0xFC
        let conts  = Arc::new(AtomicU64::new(0)); // 0xFB

        let (p, sa, so, co) = (pulses.clone(), starts.clone(), stops.clone(), conts.clone());
        let mut input = MidiInput::new("SP Clock Capture In").expect("create MidiInput");
        input.ignore(midir::Ignore::None); // do NOT filter out timing/clock bytes
        let _conn = input
            .create_virtual(
                PORT_NAME,
                move |_ts, bytes, _| {
                    for &b in bytes {
                        match b {
                            0xF8 => { p.fetch_add(1, Ordering::Relaxed); }
                            0xFA => { sa.fetch_add(1, Ordering::Relaxed); }
                            0xFB => { co.fetch_add(1, Ordering::Relaxed); }
                            0xFC => { so.fetch_add(1, Ordering::Relaxed); }
                            _ => {}
                        }
                    }
                },
                (),
            )
            .expect("create virtual MIDI destination");

        println!("Capturing on virtual MIDI destination '{PORT_NAME}' — point a clock-out at it.");
        let mut last = 0u64;
        loop {
            std::thread::sleep(Duration::from_secs(1));
            let now = pulses.load(Ordering::Relaxed);
            let per_sec = now - last;
            last = now;
            let bpm = per_sec as f64 / 24.0 * 60.0;
            println!(
                "{:>3} pulses/s  -> {:6.1} BPM   [start={} cont={} stop={}]",
                per_sec,
                bpm,
                starts.load(Ordering::Relaxed),
                conts.load(Ordering::Relaxed),
                stops.load(Ordering::Relaxed),
            );
        }
    }
}

//! Interactive virtual MIDI clock generator — a controllable external clock for
//! testing `use_bpm :midi`.
//!
//! Creates a virtual MIDI output port ("SP Test Clock") and streams 24-PPQN
//! `0xF8` timing-clock bytes at a BPM you change live from the keyboard. Any app
//! (Sonic Pi / SuperSonic) that enables this port as MIDI input sees a
//! `midi:sp_test_clock` timeline whose tempo follows what you type here.
//!
//! Run (macOS / Linux — virtual ports aren't supported on Windows midir):
//!     cargo run -p supersonic-midi --example clock_gen
//!
//! Then at the prompt:
//!     120        set tempo to 120 BPM (accepts fractions, e.g. 128.5)
//!     start / s  send Start  (0xFA)
//!     stop  / x  send Stop   (0xFC)
//!     cont  / c  send Continue (0xFB)
//!     q          quit
#![cfg(not(target_arch = "wasm32"))]

use std::io::BufRead;
use std::sync::atomic::{AtomicBool, AtomicI32, AtomicU64, Ordering};
use std::sync::Arc;
use std::time::{Duration, Instant};

#[cfg(unix)]
use midir::os::unix::VirtualOutput;
use midir::MidiOutput;

const PORT_NAME: &str = "SP Test Clock";

fn main() {
    #[cfg(not(unix))]
    {
        eprintln!("clock_gen needs virtual MIDI ports (macOS/Linux only).");
        std::process::exit(1);
    }

    #[cfg(unix)]
    {
        let out = MidiOutput::new("SP Clock Gen").expect("create MidiOutput");
        let mut conn = out
            .create_virtual(PORT_NAME)
            .expect("create virtual MIDI port");

        // milli-BPM so fractional tempos survive the atomic.
        let mbpm = Arc::new(AtomicU64::new(120_000));
        let transport = Arc::new(AtomicI32::new(-1)); // -1 none, else a status byte
        let quit = Arc::new(AtomicBool::new(false));

        let (mb, tp, qt) = (mbpm.clone(), transport.clone(), quit.clone());
        let sender = std::thread::spawn(move || {
            let mut next = Instant::now();
            while !qt.load(Ordering::Relaxed) {
                // Emit any pending transport byte first (Start/Stop/Continue).
                let t = tp.swap(-1, Ordering::Relaxed);
                if t >= 0 {
                    let _ = conn.send(&[t as u8]);
                }

                let bpm = mb.load(Ordering::Relaxed) as f64 / 1000.0;
                let interval = Duration::from_secs_f64(60.0 / bpm / 24.0);

                // Wait precisely to the next pulse instant: coarse-sleep to ~1ms
                // before, then spin. thread::sleep alone overshoots by several ms
                // (OS timer granularity), which at 24 PPQN biases the real rate
                // below nominal and makes followers phase against a fixed bpm.
                let now = Instant::now();
                if next > now {
                    let dt = next - now;
                    if dt > Duration::from_millis(2) {
                        std::thread::sleep(dt - Duration::from_millis(1));
                    }
                    while Instant::now() < next {
                        std::hint::spin_loop();
                    }
                }
                let _ = conn.send(&[0xF8]);
                // Absolute schedule (next += interval) is drift-free; only resync
                // after a real stall (machine sleep / huge tempo drop) so we don't
                // burst-emit to "catch up".
                next += interval;
                let after = Instant::now();
                if next + Duration::from_millis(250) < after {
                    next = after;
                }
            }
        });

        println!("Virtual MIDI clock port '{PORT_NAME}' is live at 120 BPM.");
        println!("Enable it as MIDI input in Sonic Pi, then: use_bpm :midi");
        println!("Commands: <number>=set BPM, start/s, stop/x, cont/c, q=quit");
        print!("bpm> ");
        use std::io::Write;
        let _ = std::io::stdout().flush();

        let stdin = std::io::stdin();
        for line in stdin.lock().lines() {
            let line = line.unwrap_or_default();
            match line.trim() {
                "q" | "quit" => break,
                "start" | "s" => { transport.store(0xFA, Ordering::Relaxed); println!("-> Start"); }
                "stop" | "x" => { transport.store(0xFC, Ordering::Relaxed); println!("-> Stop"); }
                "cont" | "c" => { transport.store(0xFB, Ordering::Relaxed); println!("-> Continue"); }
                "" => {}
                other => match other.parse::<f64>() {
                    Ok(v) if v >= 1.0 && v <= 1000.0 => {
                        mbpm.store((v * 1000.0) as u64, Ordering::Relaxed);
                        println!("-> {v} BPM");
                    }
                    _ => println!("? expected a BPM number (1-1000) or start/stop/cont/q"),
                },
            }
            print!("bpm> ");
            let _ = std::io::stdout().flush();
        }

        quit.store(true, Ordering::Relaxed);
        let _ = sender.join();
    }
}

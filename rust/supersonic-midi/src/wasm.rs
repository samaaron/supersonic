//! wasm-bindgen surface for the web MIDI seam: the same shared Rust core
//! (parse/encode/schema/estimator) exposed to the main-thread JS `MidiManager`,
//! which owns the Web MIDI I/O. Native I/O lives in `device`/`ffi` instead.
#![cfg(target_arch = "wasm32")]

use wasm_bindgen::prelude::*;

use crate::message::MidiMessage;
use crate::schema::{decode_out, encode_in, OutCommand};
use crate::ClockEstimator;

/// Raw inbound bytes from a device → a `/midi/in/*` OSC packet for the engine /
/// app. Returns `None` for clock pulses (handled by the estimator instead).
#[wasm_bindgen]
pub fn midi_in_osc(port: &str, bytes: &[u8]) -> Option<Vec<u8>> {
    encode_in(port, &MidiMessage::parse(bytes)?)
}

/// Raw inbound bytes → a flat structured event `[kind, port, ...ints]` as a JS
/// array, e.g. `["note_on", "kbd", 1, 60, 100]`. This is the main-thread fast
/// path: a web consumer that just wants the fields skips the OSC encode here and
/// the matching decode in JS that [`midi_in_osc`] would force. Returns `None`
/// for clock pulses (the estimator handles those) and for unparseable bytes.
#[wasm_bindgen]
pub fn midi_in_fields(port: &str, bytes: &[u8]) -> Option<Vec<JsValue>> {
    let msg = MidiMessage::parse(bytes)?;
    if msg.is_clock_pulse() {
        return None;
    }
    let mut nums = [0i32; 3];
    let n = msg.data_args(&mut nums);
    let mut out = Vec::with_capacity(2 + n);
    out.push(JsValue::from_str(msg.kind()));
    out.push(JsValue::from_str(port));
    for &v in &nums[..n] {
        out.push(JsValue::from_f64(v as f64));
    }
    Some(out)
}

/// A `/midi/out/*` OSC packet → `[port_len:u8][port][raw midi bytes]` for the
/// JS layer to hand to `MIDIOutput.send`. `None` for non-send verbs.
#[wasm_bindgen]
pub fn midi_out_decode(osc: &[u8]) -> Option<Vec<u8>> {
    let (port, bytes) = match decode_out(osc)? {
        OutCommand::Send { port, msg } => {
            // channel 0 = "all channels" (wire channel -1): concatenate the 16
            // channel-voice messages — MIDIOutput.send accepts a multi-message
            // buffer, so one send() covers all channels.
            if msg.channel() == Some(0) {
                let mut b = Vec::new();
                for ch in 1..=16 {
                    b.extend_from_slice(&msg.with_channel(ch).encode());
                }
                (port, b)
            } else {
                (port, msg.encode())
            }
        }
        OutCommand::SendRaw { port, bytes } => (port, bytes),
        _ => return None,
    };
    let mut out = Vec::with_capacity(1 + port.len() + bytes.len());
    out.push(port.len() as u8);
    out.extend_from_slice(port.as_bytes());
    out.extend_from_slice(&bytes);
    Some(out)
}

/// Normalise a raw device name to its OSC-safe handle — identical to the native
/// path, so a port is addressed the same way on web and native.
#[wasm_bindgen]
pub fn normalize_name(raw: &str) -> String {
    crate::normalize::safe_osc_name(raw)
}

/// Median-filtered tempo estimator for an incoming MIDI clock (the same one the
/// native side uses), fed clock-pulse arrival timestamps in microseconds.
#[wasm_bindgen]
pub struct WasmClockEstimator {
    inner: ClockEstimator,
}

#[wasm_bindgen]
impl WasmClockEstimator {
    #[wasm_bindgen(constructor)]
    pub fn new() -> WasmClockEstimator {
        WasmClockEstimator { inner: ClockEstimator::new() }
    }

    /// Feed a pulse timestamp (µs); returns the BPM estimate once available.
    pub fn update(&mut self, ts_us: f64) -> Option<f64> {
        self.inner.update(ts_us as u64)
    }

    pub fn reset(&mut self) {
        self.inner.reset();
    }
}

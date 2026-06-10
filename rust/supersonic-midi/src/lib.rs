//! SuperSonic MIDI subsystem.
//!
//! A midir-based MIDI subsystem within SuperSonic. It compiles to two shapes
//! from one codebase:
//!
//! * a **native rlib** folded into the `supersonic-native` staticlib linked
//!   into the C++ engine (CoreMIDI/ALSA/WinMM), and
//! * a **wasm-bindgen module** loaded by the main-thread SuperSonic JS, where Web
//!   MIDI lives.
//!
//! The pure logic here — message [`message`] parse/encode, device-name
//! [`normalize`]ation, and [`clock`] generation/estimation — is shared
//! across both targets. Only the IO/FFI shells are platform-specific.

pub mod clock;
pub mod message;
pub mod schema;
pub mod sync;

// The OSC codec + name normalisation live in the shared supersonic-osc crate
// (also used by supersonic-gamepad), exposed under this crate's module paths.
pub use supersonic_osc::{normalize, osc};

pub use clock::{ClockEstimator, EstimatorParams, PPQN};
pub use message::MidiMessage;
pub use normalize::{normalize_ports, safe_osc_name, PortInfo};
pub use osc::{OscArg, OscMessage};
pub use schema::{
    decode_out, encode_clock_bpm, encode_in, encode_ports, encode_ports_reply, OutCommand,
};
pub use sync::{transport_event, TransportEvent};

// Device IO + FFI use midir and OS threads (native). The web seam exposes the
// same shared core to JS via wasm-bindgen; Web MIDI I/O is done in JS.
#[cfg(not(target_arch = "wasm32"))]
pub mod device;
#[cfg(not(target_arch = "wasm32"))]
pub mod ffi;
#[cfg(not(target_arch = "wasm32"))]
pub mod watcher;
#[cfg(target_arch = "wasm32")]
pub mod wasm;

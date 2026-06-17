//! Shared, std-only helpers for SuperSonic's Rust subsystems (MIDI, gamepad, OSC):
//! a complete OSC 1.0 [`osc`] codec (messages + bundles), OSC-safe device-name
//! [`normalize`]ation,
//! and the native subsystems' common C-ABI scaffolding ([`ffi`]). Kept
//! dependency-free so it builds identically for the native staticlibs and the
//! wasm-bindgen modules.

#[cfg(not(target_arch = "wasm32"))]
pub mod ffi;
pub mod normalize;
pub mod osc;

pub use normalize::{assign_handle, normalize_ports, safe_osc_name, PortInfo};
pub use osc::{decode, decode_packet, encode, encode_packet, OscArg, OscBundle, OscMessage, OscPacket};

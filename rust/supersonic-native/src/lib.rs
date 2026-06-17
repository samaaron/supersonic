//! Umbrella staticlib for SuperSonic's native Rust subsystems.
//!
//! A binary may link exactly one Rust staticlib (each staticlib bundles its own
//! copy of std, so two collide at final link). This crate is that staticlib:
//! the subsystems are plain rlib dependencies, and the `pub use` below keeps
//! their `#[no_mangle]` C ABI symbols (`ss_midi_*`, `ss_gamepad_*`, `ss_osc_*`)
//! anchored in the archive CMake links into the engine. The C headers live with
//! the subsystems: `supersonic-midi/cpp/ss_midi.h`,
//! `supersonic-gamepad/cpp/ss_gamepad.h`, `supersonic-osc-net/cpp/ss_osc.h`.

#[cfg(feature = "gamepad")]
pub use supersonic_gamepad;
#[cfg(feature = "midi")]
pub use supersonic_midi;
#[cfg(feature = "osc")]
pub use supersonic_osc_net;

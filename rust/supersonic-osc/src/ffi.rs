//! Shared scaffolding for the subsystem C ABIs (`ss_midi_*`, `ss_gamepad_*`):
//! the host emit-callback shape, its `kind` codes, and the panic fence. One
//! definition keeps the cross-subsystem ABI invariants (the C headers'
//! `SS_*_EMIT_*` values, the never-unwind guarantee) pinned in a single place.

use std::ffi::c_void;

/// Emit an OSC packet to the engine. `kind` is [`EMIT_BROADCAST`] (fan out to
/// the subsystem's notify audience) or [`EMIT_REPLY`] (reply to the current
/// caller). `osc`/`len` are only valid for the duration of the call; the
/// callback may fire on a subsystem-owned thread, so the host implementation
/// must be thread-safe.
pub type EmitFn = extern "C" fn(ctx: *mut c_void, kind: i32, osc: *const u8, len: u32);

/// `kind` codes for [`EmitFn`]. Must match the `SS_MIDI_EMIT_*` /
/// `SS_GAMEPAD_EMIT_*` defines in the subsystems' C headers.
pub const EMIT_BROADCAST: i32 = 0;
pub const EMIT_REPLY: i32 = 1;

/// Run `f`, catching any panic so it cannot unwind across a C ABI or OS
/// callback boundary (which aborts the whole engine process). The subsystems
/// are best-effort peripherals: on a panic the operation is dropped and the
/// subsystem stays alive (a poisoned mutex then disables it rather than
/// killing audio).
pub fn no_unwind<T>(default: T, f: impl FnOnce() -> T) -> T {
    std::panic::catch_unwind(std::panic::AssertUnwindSafe(f)).unwrap_or(default)
}

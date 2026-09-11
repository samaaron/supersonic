// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2025-2026 Sam Aaron
//! SuperSonic's Rust umbrella.
//!
//! Nothing is implemented here. Every crate below exports `#[no_mangle]` C
//! symbols, and an rlib's symbols only reach the final staticlib if something
//! references the crate — so this file exists to reference them.
//!
//! The two halves it joins are the point: tau's subsystems, from the
//! submodule under a licence of their own, and the engine layer that drives
//! scsynth. Only one Rust staticlib may be linked into a binary, so they have
//! to arrive as one, and this is where that happens.

pub use clockwork_clock;
pub use clockwork_heap;
pub use supersonic_node_mirror;
pub use clockwork_ports;
pub use clockwork_ports_disk;
pub use clockwork_schedule;
pub use clockwork_scope;
pub use clockwork_sinks;

#[cfg(feature = "gamepad")]
pub use clockwork_gamepad;
#[cfg(feature = "midi")]
pub use clockwork_midi;
#[cfg(feature = "osc")]
pub use clockwork_osc_net;
#[cfg(feature = "comms")]
pub use clockwork_comms;

// NOT supersonic-engine. That crate is the OTHER implementation of
// engine_api.h — it defines World_Cleanup and World_CopySndBuf itself, and
// upstream's SUPERSONIC_RUST_ENGINE option swaps it in PLACE OF synth/server.
// scsynth-nrt is the C++ server, so linking both is a duplicate-symbol error,
// which is exactly how this was found.
//
// What remains here is what the C++ ugens call into regardless of which engine
// runs: the buffer table, the limiter, the node-tree mirror.
pub use supersonic_buffers;
pub use supersonic_limiter;
pub use supersonic_nodetree;

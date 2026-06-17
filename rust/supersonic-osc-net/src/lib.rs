//! SuperSonic's OSC networking subsystem (native-only).
//!
//! Owns the external-facing OSC sockets — the cue server (inbound external OSC,
//! re-framed to `/external-osc-cue` and emitted to the engine) and the outbound
//! user-OSC sender (`osc_send` / scheduled `osc`). Uses `std::net`, which
//! resolves a hostname in the socket's own
//! address family, so `localhost` reaches the cue server on whichever family it
//! resolves to. The C ABI the engine links against is in `cpp/ss_osc.h`; the
//! engine integration + Ruby-facing transport stay in C++.

#[cfg(not(target_arch = "wasm32"))]
pub mod ffi;

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

// Kernel-ACL'd local transports (the UDP control port's owner-only siblings):
// UDS datagram, length-prefix-framed stream servers (UDS + TCP), and the
// Windows named-pipe analogue behind the same stream ABI.
#[cfg(not(target_arch = "wasm32"))]
pub mod pipe;
#[cfg(not(target_arch = "wasm32"))]
pub mod stream;
#[cfg(not(target_arch = "wasm32"))]
pub mod uds;

// Cross-process SHM peer client (the peer side of the SHM command plane) — used
// by the transport harness to drive --shm-commands end-to-end. Not a C ABI.
#[cfg(not(target_arch = "wasm32"))]
pub mod shm;

/// Does this receive error leave the socket usable, so the loop should poll
/// again rather than tear the connection down?
///
/// `WouldBlock`/`TimedOut` are the read-timeout tick every receive loop polls
/// on. `Interrupted` is EINTR: std does not retry `read`/`recv_from` for us
/// (unlike `write_all`/`read_exact`, which do), so it surfaces here on any
/// signal delivered to that thread — a live client, not a dead one.
#[cfg(not(target_arch = "wasm32"))]
pub(crate) fn recv_retryable(e: &std::io::Error) -> bool {
    use std::io::ErrorKind;
    matches!(e.kind(), ErrorKind::WouldBlock | ErrorKind::TimedOut | ErrorKind::Interrupted)
}

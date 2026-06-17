/*
 * ss_osc.h — C ABI for the SuperSonic OSC networking subsystem (Rust / std::net).
 *
 * The C++ engine seam (src/native/OscControl) creates one instance with an emit
 * callback, then drives it:
 *   - ss_osc_configure(): (re)bind the external cue server.
 *   - ss_osc_send():      send a now-due scheduled OSC packet to a host:port.
 * Inbound external OSC is re-framed to /external-osc-cue <ip> <port> <address>
 * <args...> and handed back via the emit callback (which may fire on the cue
 * server's recv thread, so the host impl must be thread-safe). The subsystem
 * owns its UDP sockets + recv threads (one per bound family) and never touches
 * the audio thread.
 *
 * Must match rust/supersonic-osc-net/src/ffi.rs. Dual-licensed
 * MIT OR GPL-3.0-or-later (see repo LICENSE).
 */
#ifndef SUPERSONIC_SS_OSC_H
#define SUPERSONIC_SS_OSC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque handle. */
typedef struct SsOsc SsOsc;

/* `kind` codes for ss_osc_emit_fn (shares supersonic_osc::ffi values). */
#define SS_OSC_EMIT_BROADCAST 0 /* fan out to the /osc/notify audience */
#define SS_OSC_EMIT_REPLY 1     /* reply to the current caller (unused by the cue server) */

/* Emit an OSC packet to the engine — an /external-osc-cue push for inbound
 * external OSC. `osc`/`len` are only valid for the duration of the call. */
typedef void (*ss_osc_emit_fn)(void* ctx, int32_t kind, const uint8_t* osc, uint32_t len);

/* Create the subsystem. `ctx` and `emit` must outlive it. Null on failure. */
SsOsc* ss_osc_create(void* ctx, ss_osc_emit_fn emit);

/* Stop the cue recv threads, close the sockets, free the instance. */
void ss_osc_destroy(SsOsc* handle);

/* (Re)configure the cue server: bind `port` (0 = unbind); `loopback` != 0 binds
 * 127.0.0.1, else all interfaces; `cues_on` != 0 forwards inbound OSC as cues.
 * Off the audio thread. */
void ss_osc_configure(SsOsc* handle, int32_t port, int32_t loopback, int32_t cues_on);

/* Send one OSC packet `data`/`len` to `host`:`port`. `host` is a byte string
 * (ptr + len, not NUL-terminated). Off the audio thread. */
void ss_osc_send(SsOsc* handle, const uint8_t* host, uint32_t host_len,
                 int32_t port, const uint8_t* data, uint32_t len);

/* Raw OSC ingress (separate from the cue server): receive datagrams on `port`
 * and hand the raw OSC bytes to `emit` (kind = broadcast) without re-framing.
 * `loopback` != 0 binds 127.0.0.1 + ::1, else all interfaces. The standalone
 * host uses this for its control port. Null on bind failure. */
typedef struct SsOscIngress SsOscIngress;
SsOscIngress* ss_osc_ingress_start(void* ctx, ss_osc_emit_fn emit,
                                   int32_t port, int32_t loopback);
void ss_osc_ingress_stop(SsOscIngress* handle);

/* Source-bearing OSC ingress: like ss_osc_ingress_start, but delivers each
 * datagram verbatim TOGETHER WITH the sender's (ip, port), so an engine transport
 * can intern an origin token and address a reply back. `bind_addr` is a byte
 * string (ptr + len, not NUL-terminated): empty = all interfaces, else an
 * IPv4/IPv6 literal or hostname (e.g. "127.0.0.1" for loopback). `ip` in the
 * callback is valid only for the call. Null on bind failure; free with
 * ss_osc_ingress_stop. */
typedef void (*ss_osc_emit_src_fn)(void* ctx, const uint8_t* ip, uint32_t ip_len,
                                   int32_t port, const uint8_t* osc, uint32_t len);
SsOscIngress* ss_osc_ingress_start_with_src(void* ctx, ss_osc_emit_src_fn emit,
                                            int32_t port,
                                            const uint8_t* bind_addr, uint32_t bind_addr_len);

#ifdef __cplusplus
}
#endif

#endif /* SUPERSONIC_SS_OSC_H */

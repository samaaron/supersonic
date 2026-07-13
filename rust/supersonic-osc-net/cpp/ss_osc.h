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
 * owns its sockets + recv threads and never touches the audio thread.
 *
 * The standalone server's command transports also live here: the raw UDP
 * ingress (UdpOscTransport), the UDS datagram server (UdsDgramOscTransport),
 * and the framed stream servers — TCP, UDS stream, Windows named pipe — behind
 * one SsOscStream handle (StreamOscTransport).
 *
 * Must match rust/supersonic-osc-net/src/{ffi,uds,stream,pipe}.rs.
 * Dual-licensed MIT OR GPL-3.0-or-later (see repo LICENSE).
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

/* ── UDS datagram ingress (unix only) ─────────────────────────────────────────
 * The kernel-ACL'd sibling of the UDP control port: binds a socket file at
 * `path` (created 0600, replacing a stale file; put it in a 0700 directory to
 * close the bind→chmod window) and delivers each datagram verbatim together
 * with the sender's socket path, so a transport can intern an origin token and
 * reply. A sender that did not bind its own path arrives with an empty peer
 * path and is unaddressable (macOS has no autobind). All byte strings are
 * ptr + len, not NUL-terminated. Start returns null on failure, and always
 * null on Windows (see the named-pipe API below). */
typedef struct SsOscUds SsOscUds;
typedef void (*ss_osc_emit_path_fn)(void* ctx, const uint8_t* peer_path, uint32_t peer_len,
                                    const uint8_t* osc, uint32_t len);
SsOscUds* ss_osc_uds_dgram_start(void* ctx, ss_osc_emit_path_fn emit,
                                 const uint8_t* path, uint32_t path_len);
/* Reply to the peer bound at `path` from the server's own socket. Returns 1 on
 * send, 0 on failure/empty path. Off the audio thread. */
int32_t ss_osc_uds_dgram_send(SsOscUds* handle, const uint8_t* path, uint32_t path_len,
                              const uint8_t* data, uint32_t len);
/* Stop the recv thread, close the socket, unlink the path, free the server. */
void ss_osc_uds_stop(SsOscUds* handle);

/* ── Stream servers: TCP, UDS stream, Windows named pipes ─────────────────────
 * Connection-oriented OSC over one shared wire format: each packet is preceded
 * by a 4-byte big-endian length (the scsynth TCP convention; max payload 64 KiB,
 * violations close the connection). Admission control happens at accept: at
 * most `max_conns` concurrent connections, an over-cap connect is immediately
 * closed (TCP/UDS) or fails busy (pipes). Connection ids are minted from 1 and
 * never reused — a transport uses them directly as origin tokens (0 = the
 * in-process caller). `on_closed` fires once when a connection ends (EOF,
 * error, protocol violation) but not during server stop, so a transport can
 * drop that client's subscriptions: subscription lifetime == connection
 * lifetime. Callbacks may fire on any reader thread. */
typedef struct SsOscStream SsOscStream;
typedef void (*ss_osc_stream_packet_fn)(void* ctx, uint32_t conn_id,
                                        const uint8_t* osc, uint32_t len);
typedef void (*ss_osc_stream_closed_fn)(void* ctx, uint32_t conn_id);

/* TCP on `port` bound to `bind_addr` (empty = all IPv4 interfaces; pass "::"
 * for IPv6). Port 0 binds ephemerally — read it back with ss_osc_stream_port.
 * Null on bind failure. */
SsOscStream* ss_osc_tcp_start(void* ctx, ss_osc_stream_packet_fn on_packet,
                              ss_osc_stream_closed_fn on_closed, int32_t port,
                              const uint8_t* bind_addr, uint32_t bind_addr_len,
                              uint32_t max_conns);
/* UDS stream at `path` (socket file created 0600, stale file replaced). Null
 * on failure, and always null on Windows. */
SsOscStream* ss_osc_uds_stream_start(void* ctx, ss_osc_stream_packet_fn on_packet,
                                     ss_osc_stream_closed_fn on_closed,
                                     const uint8_t* path, uint32_t path_len,
                                     uint32_t max_conns);
/* Windows named pipe: `name` is a bare name (prefixed with \\.\pipe\) or a
 * full pipe path. Instances carry an owner-only DACL (SYSTEM + current user)
 * and PIPE_REJECT_REMOTE_CLIENTS; the first instance is squat-checked
 * (FILE_FLAG_FIRST_PIPE_INSTANCE). Always null on non-Windows platforms. */
SsOscStream* ss_osc_pipe_start(void* ctx, ss_osc_stream_packet_fn on_packet,
                               ss_osc_stream_closed_fn on_closed,
                               const uint8_t* name, uint32_t name_len,
                               uint32_t max_conns);
/* The actual bound TCP port (for port-0 starts); 0 for path/name servers. */
int32_t ss_osc_stream_port(SsOscStream* handle);
/* Frame and send one packet to a live connection. 1 = sent, 0 = unknown/closed
 * connection or write failure. Off the audio thread. */
int32_t ss_osc_stream_send(SsOscStream* handle, uint32_t conn_id,
                           const uint8_t* data, uint32_t len);
/* Stop accepting, close every connection, join the threads, free the server. */
void ss_osc_stream_stop(SsOscStream* handle);

#ifdef __cplusplus
}
#endif

#endif /* SUPERSONIC_SS_OSC_H */

//! Connection-oriented OSC servers — TCP and UDS-stream — sharing one framed
//! protocol and one connection registry.
//!
//! Wire format: each OSC packet is preceded by a 4-byte big-endian length
//! (the scsynth TCP convention). A frame longer than [`MAX_FRAME`] or of zero
//! length is a protocol violation and closes the connection.
//!
//! Admission control happens once, at accept: at most `max_conns` concurrent
//! connections; an accept beyond the cap is immediately closed so the excess
//! client sees a crisp reset instead of a silent hang. Connection ids are
//! minted monotonically from 1 and never reused — the C++ transport uses them
//! directly as origin tokens (0 = in-process caller, so ids start at 1).
//!
//! On disconnect (EOF, error, or protocol violation) the connection leaves
//! the registry and `on_closed` fires, so the transport can drop that
//! client's notify subscriptions — subscription lifetime == connection
//! lifetime. `on_closed` is suppressed during server stop (the host is
//! tearing down and must not be re-entered).

use std::ffi::c_void;
use std::slice;

/// Longest accepted frame payload. Sized to admit /d_recv synthdef blobs
/// (the native IN ring is hundreds of KB, "large for SynthDefs"), matching
/// the SHM command plane's ring so every local command transport agrees on
/// the largest command it will carry; still a hard bound so a stream client
/// can't make a reader accumulate unboundedly. (The UDP transport keeps its
/// own 64 KiB datagram cap — a datagram can't exceed that anyway.)
pub const MAX_FRAME: usize = 256 * 1024;

/// Packet callback: (ctx, conn_id, osc, len). `osc` is one complete de-framed
/// OSC packet, valid only for the duration of the call. May fire on any
/// connection's reader thread.
pub type StreamPacketFn = extern "C" fn(*mut c_void, u32, *const u8, u32);
/// Disconnect callback: (ctx, conn_id). Fires once per connection after it
/// has left the registry; not fired during server stop.
pub type StreamClosedFn = extern "C" fn(*mut c_void, u32);

/// One running stream server behind the C ABI — socket-based (TCP/UDS) here,
/// named-pipe-based (Windows) in `pipe.rs`. The handle owns the accept and
/// reader threads; dropping it stops them all.
pub struct SsOscStream(pub(crate) Box<dyn StreamServerImpl>);

/// What every stream server backend provides to the shared C ABI.
pub(crate) trait StreamServerImpl: Send {
    /// Frame and send one packet to a live connection.
    fn send(&self, conn_id: u32, data: &[u8]) -> bool;
    /// The actual bound TCP port (0 for path/name-addressed servers).
    fn port(&self) -> i32 {
        0
    }
}

/// The host callbacks + opaque context, shared by every reader thread.
/// Safety: the C++ side guarantees `ctx` outlives the server and the
/// callbacks are thread-safe (the same contract as the UDP emit callback).
#[derive(Clone, Copy)]
pub(crate) struct Host {
    pub ctx: *mut c_void,
    pub on_packet: StreamPacketFn,
    pub on_closed: StreamClosedFn,
}
unsafe impl Send for Host {}
unsafe impl Sync for Host {}

impl Host {
    pub(crate) fn packet(&self, conn: u32, osc: &[u8]) {
        no_unwind((), || (self.on_packet)(self.ctx, conn, osc.as_ptr(), osc.len() as u32));
    }
    pub(crate) fn closed(&self, conn: u32) {
        no_unwind((), || (self.on_closed)(self.ctx, conn));
    }
}

/// Frame `data` for the wire: 4-byte big-endian length + payload.
pub(crate) fn frame_packet(data: &[u8]) -> Vec<u8> {
    let mut out = Vec::with_capacity(4 + data.len());
    out.extend_from_slice(&(data.len() as u32).to_be_bytes());
    out.extend_from_slice(data);
    out
}

/// Feed newly-read bytes into the accumulator and deliver every complete
/// frame. Returns false on a protocol violation (zero-length or oversize
/// frame) — the caller closes the connection.
pub(crate) fn drain_frames(acc: &mut Vec<u8>, host: &Host, conn: u32) -> bool {
    loop {
        if acc.len() < 4 {
            return true;
        }
        let len = u32::from_be_bytes([acc[0], acc[1], acc[2], acc[3]]) as usize;
        if len == 0 || len > MAX_FRAME {
            return false;
        }
        if acc.len() < 4 + len {
            return true;
        }
        host.packet(conn, &acc[4..4 + len]);
        acc.drain(..4 + len);
    }
}

// ── Socket backend (TCP + UDS stream) ────────────────────────────────────────

use std::collections::HashMap;
use std::io::{ErrorKind, Read, Write};
use std::net::{Shutdown, TcpListener, TcpStream};
#[cfg(unix)]
use std::os::unix::net::{UnixListener, UnixStream};
use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, AtomicU32, Ordering};
use std::sync::{Arc, Mutex};
use std::thread::JoinHandle;
use std::time::Duration;

use supersonic_osc::ffi::no_unwind;

/// Bound on a single blocking send. A client that hasn't drained its socket
/// within this long is treated as stuck: the write fails and the connection
/// is dropped, so one non-reading client can't wedge the gateway thread (the
/// sole caller of send/broadcast) indefinitely on a full kernel send buffer.
const WRITE_TIMEOUT: Duration = Duration::from_secs(2);

/// How many times one frame may be re-offered to a socket that refused it
/// without taking any bytes. Bounded so a client whose refusal never clears
/// still ends up evicted instead of parking the gateway thread.
const WRITE_RETRIES: usize = 3;

/// Pause between those retries — long enough for a momentary kernel buffer
/// shortage to clear, short enough that the whole bounded sequence stays far
/// inside WRITE_TIMEOUT.
const WRITE_RETRY_PAUSE: Duration = Duration::from_millis(1);

/// A connection's write half: raw writes + a shutdown to unblock its reader.
/// Byte-level rather than write-all so the retry policy can tell "nothing went
/// out" from "half a frame went out", which decides whether framing survived.
trait ConnWriter: Send {
    fn write_some(&mut self, data: &[u8]) -> std::io::Result<usize>;
    fn set_write_timeout(&self, d: Duration);
    fn shutdown_both(&self);
}
impl ConnWriter for TcpStream {
    fn write_some(&mut self, data: &[u8]) -> std::io::Result<usize> {
        self.write(data)
    }
    fn set_write_timeout(&self, d: Duration) {
        let _ = TcpStream::set_write_timeout(self, Some(d));
    }
    fn shutdown_both(&self) {
        let _ = self.shutdown(Shutdown::Both);
    }
}
#[cfg(unix)]
impl ConnWriter for UnixStream {
    fn write_some(&mut self, data: &[u8]) -> std::io::Result<usize> {
        self.write(data)
    }
    fn set_write_timeout(&self, d: Duration) {
        let _ = UnixStream::set_write_timeout(self, Some(d));
    }
    fn shutdown_both(&self) {
        let _ = self.shutdown(Shutdown::Both);
    }
}

/// Is this write error worth re-offering the frame for?
///
/// `Interrupted` is EINTR — no bytes were taken and no state was lost.
/// `Other` is where a transient kernel refusal such as ENOBUFS lands, since
/// std has no ErrorKind for it. Everything else is either terminal (the peer
/// is gone) or WouldBlock/TimedOut, which on a blocking socket means
/// SO_SNDTIMEO expired: a client that stopped reading, and must be dropped
/// rather than retried.
fn write_retryable(e: &std::io::Error) -> bool {
    matches!(e.kind(), ErrorKind::Interrupted | ErrorKind::Other)
}

/// Push one whole frame at a connection, resuming after a retryable refusal.
/// An error means the connection is finished: either the peer is gone, or a
/// refusal outlasted the retry budget with part of a frame already on the
/// wire, which leaves the stream desynced.
fn write_frame(w: &mut dyn ConnWriter, frame: &[u8]) -> std::io::Result<()> {
    let (mut sent, mut retries) = (0usize, 0usize);
    while sent < frame.len() {
        match w.write_some(&frame[sent..]) {
            Ok(0) => return Err(std::io::Error::from(ErrorKind::WriteZero)),
            Ok(n) => sent += n,
            Err(e) if write_retryable(&e) && retries < WRITE_RETRIES => {
                retries += 1;
                // EINTR costs nothing to re-issue; a kernel refusal needs a
                // moment before it can clear.
                if e.kind() != ErrorKind::Interrupted {
                    std::thread::sleep(WRITE_RETRY_PAUSE);
                }
            }
            Err(e) => return Err(e),
        }
    }
    Ok(())
}

/// A connection's read half, abstracted over TCP/UDS.
trait ConnReader: Read + Send + 'static {
    fn set_timeout(&self, d: Duration);
    fn clone_writer(&self) -> std::io::Result<Box<dyn ConnWriter>>;
}
impl ConnReader for TcpStream {
    fn set_timeout(&self, d: Duration) {
        let _ = self.set_read_timeout(Some(d));
    }
    fn clone_writer(&self) -> std::io::Result<Box<dyn ConnWriter>> {
        Ok(Box::new(self.try_clone()?))
    }
}
#[cfg(unix)]
impl ConnReader for UnixStream {
    fn set_timeout(&self, d: Duration) {
        let _ = self.set_read_timeout(Some(d));
    }
    fn clone_writer(&self) -> std::io::Result<Box<dyn ConnWriter>> {
        Ok(Box::new(self.try_clone()?))
    }
}

/// A nonblocking listener, abstracted over TCP/UDS.
trait Acceptor: Send + 'static {
    type Conn: ConnReader;
    fn accept_one(&self) -> std::io::Result<Self::Conn>;
}
impl Acceptor for TcpListener {
    type Conn = TcpStream;
    fn accept_one(&self) -> std::io::Result<TcpStream> {
        // BSD/Windows accepted sockets inherit the listener's non-blocking
        // flag (Linux's don't). A non-blocking conn ignores SO_RCVTIMEO, so
        // the reader's 100ms-timeout read becomes an EAGAIN hot spin — one
        // full core per connection. Force blocking before handing it over.
        self.accept().map(|(s, _)| {
            let _ = s.set_nonblocking(false);
            s
        })
    }
}
#[cfg(unix)]
impl Acceptor for UnixListener {
    type Conn = UnixStream;
    fn accept_one(&self) -> std::io::Result<UnixStream> {
        self.accept().map(|(s, _)| {
            let _ = s.set_nonblocking(false);
            s
        })
    }
}

type Writer = Arc<Mutex<Box<dyn ConnWriter>>>;

/// State shared between the accept loop, reader threads, and the C ABI.
struct Shared {
    stop: AtomicBool,
    conns: Mutex<HashMap<u32, Writer>>,
    next_id: AtomicU32, // conn ids from 1, never reused (0 = in-process)
    readers: Mutex<Vec<JoinHandle<()>>>,
}

struct SocketServer {
    shared: Arc<Shared>,
    accept_join: Option<JoinHandle<()>>,
    port: i32,
    path: Option<PathBuf>, // UDS: unlink on drop
}

impl StreamServerImpl for SocketServer {
    fn send(&self, conn_id: u32, data: &[u8]) -> bool {
        // Clone the writer handle out of the registry, then write outside the
        // registry lock — a slow client must not stall unrelated sends.
        let writer = self.shared.conns.lock().unwrap().get(&conn_id).cloned();
        let Some(writer) = writer else { return false };
        let err = match write_frame(&mut **writer.lock().unwrap(), &frame_packet(data)) {
            Ok(()) => return true,
            Err(e) => e,
        };
        // The frame did not make it and retrying is pointless: the peer is gone,
        // or a refusal outlasted the budget mid-frame and this connection's
        // framing is now suspect. Evict and shut it down; the reader wakes on
        // the shutdown and fires on_closed, pruning the client from every
        // audience. The logged error kind says which: WouldBlock/TimedOut is a
        // client that went WRITE_TIMEOUT without reading, anything else failed
        // outright.
        eprintln!("[osc] stream conn {conn_id} dropped — reply write failed: {err}");
        if let Some(w) = self.shared.conns.lock().unwrap().remove(&conn_id) {
            w.lock().unwrap().shutdown_both();
        }
        false
    }
    fn port(&self) -> i32 {
        self.port
    }
}

impl Drop for SocketServer {
    fn drop(&mut self) {
        self.shared.stop.store(true, Ordering::Relaxed);
        // Shut every connection down so blocked readers return immediately
        // rather than waiting out their read timeout.
        for w in self.shared.conns.lock().unwrap().values() {
            w.lock().unwrap().shutdown_both();
        }
        if let Some(j) = self.accept_join.take() {
            let _ = j.join();
        }
        for j in self.shared.readers.lock().unwrap().drain(..) {
            let _ = j.join();
        }
        if let Some(p) = &self.path {
            let _ = std::fs::remove_file(p);
        }
    }
}

fn run_reader<C: ConnReader>(mut conn: C, id: u32, shared: Arc<Shared>, host: Host) {
    conn.set_timeout(Duration::from_millis(100));
    let mut acc: Vec<u8> = Vec::new();
    let mut tmp = [0u8; 8192];
    // Why this connection ended, when it wasn't the peer's own doing. A clean
    // EOF is the client's decision and stays quiet; anything else is the
    // server hanging up on a live client, so say so.
    let mut why: Option<String> = None;
    while !shared.stop.load(Ordering::Relaxed) {
        match conn.read(&mut tmp) {
            Ok(0) => break, // EOF — peer gone
            Ok(n) => {
                acc.extend_from_slice(&tmp[..n]);
                if !drain_frames(&mut acc, &host, id) {
                    why = Some("protocol violation (bad frame length)".into());
                    break;
                }
            }
            Err(e) if crate::recv_retryable(&e) => {}
            Err(e) => {
                why = Some(format!("read failed: {e}"));
                break;
            }
        }
    }
    // Leave the registry, then tell the host — subscription lifetime ==
    // connection lifetime. Suppressed during stop: the host is tearing down.
    if let Some(w) = shared.conns.lock().unwrap().remove(&id) {
        w.lock().unwrap().shutdown_both();
    }
    if !shared.stop.load(Ordering::Relaxed) {
        if let Some(why) = why {
            eprintln!("[osc] stream conn {id} dropped — {why}");
        }
        host.closed(id);
    }
}

// Join and discard the handles of readers that have already exited, so the
// registry tracks only live readers instead of growing with total-connections
// across a long-running server's reconnect churn. Live readers are left in
// place for the join-on-stop guarantee in SocketServer::drop.
fn reap_finished_readers(shared: &Shared) {
    let mut readers = shared.readers.lock().unwrap();
    let mut i = 0;
    while i < readers.len() {
        if readers[i].is_finished() {
            let _ = readers.swap_remove(i).join(); // already done — returns at once
        } else {
            i += 1;
        }
    }
}

fn run_accept<A: Acceptor>(listener: A, shared: Arc<Shared>, host: Host, max_conns: usize) {
    while !shared.stop.load(Ordering::Relaxed) {
        reap_finished_readers(&shared);
        match listener.accept_one() {
            Ok(conn) => {
                // Admission control: over the cap → drop immediately, so the
                // excess client sees a crisp reset instead of a silent hang.
                if shared.conns.lock().unwrap().len() >= max_conns {
                    drop(conn);
                    continue;
                }
                let id = shared.next_id.fetch_add(1, Ordering::Relaxed);
                let writer = match conn.clone_writer() {
                    Ok(w) => {
                        w.set_write_timeout(WRITE_TIMEOUT);
                        Arc::new(Mutex::new(w))
                    }
                    Err(_) => continue,
                };
                shared.conns.lock().unwrap().insert(id, writer);
                let (t_shared, t_host) = (shared.clone(), host);
                if let Ok(j) = std::thread::Builder::new()
                    .name("ss-osc-stream".into())
                    .spawn(move || run_reader(conn, id, t_shared, t_host))
                {
                    shared.readers.lock().unwrap().push(j);
                } else {
                    shared.conns.lock().unwrap().remove(&id);
                }
            }
            Err(e) if e.kind() == ErrorKind::WouldBlock => {
                std::thread::sleep(Duration::from_millis(20));
            }
            Err(_) => std::thread::sleep(Duration::from_millis(20)),
        }
    }
}

fn start_socket_server<A: Acceptor>(
    listener: A,
    host: Host,
    max_conns: u32,
    port: i32,
    path: Option<PathBuf>,
) -> Option<SsOscStream> {
    let shared = Arc::new(Shared {
        stop: AtomicBool::new(false),
        conns: Mutex::new(HashMap::new()),
        next_id: AtomicU32::new(1),
        readers: Mutex::new(Vec::new()),
    });
    let max = max_conns.max(1) as usize;
    let t_shared = shared.clone();
    let accept_join = std::thread::Builder::new()
        .name("ss-osc-accept".into())
        .spawn(move || run_accept(listener, t_shared, host, max))
        .ok()?;
    Some(SsOscStream(Box::new(SocketServer {
        shared,
        accept_join: Some(accept_join),
        port,
        path,
    })))
}

// ── C ABI ────────────────────────────────────────────────────────────────────

/// Start a TCP OSC server on `port` bound to `bind_addr` (byte string,
/// ptr+len; empty = all IPv4 interfaces, else an address literal — pass "::"
/// for IPv6). Pass port 0 to bind an ephemeral port and read it back with
/// [`ss_osc_stream_port`]. Returns an owning pointer (null on bind failure);
/// free with [`ss_osc_stream_stop`]. `ctx` and both callbacks must outlive it.
#[no_mangle]
pub unsafe extern "C" fn ss_osc_tcp_start(
    ctx: *mut c_void,
    on_packet: StreamPacketFn,
    on_closed: StreamClosedFn,
    port: i32,
    bind_addr: *const u8,
    bind_addr_len: u32,
    max_conns: u32,
) -> *mut SsOscStream {
    no_unwind(std::ptr::null_mut(), || {
        if !(0..=65535).contains(&port) {
            return std::ptr::null_mut();
        }
        let addr = if bind_addr.is_null() {
            ""
        } else {
            std::str::from_utf8(slice::from_raw_parts(bind_addr, bind_addr_len as usize))
                .unwrap_or("")
        };
        let addr = if addr.is_empty() { "0.0.0.0" } else { addr };
        let listener = match TcpListener::bind((addr, port as u16)) {
            Ok(l) => l,
            Err(e) => {
                eprintln!("[osc] TCP bind {addr}:{port} failed: {e}");
                return std::ptr::null_mut();
            }
        };
        let bound = listener.local_addr().map(|a| a.port() as i32).unwrap_or(port);
        if listener.set_nonblocking(true).is_err() {
            return std::ptr::null_mut();
        }
        let host = Host { ctx, on_packet, on_closed };
        match start_socket_server(listener, host, max_conns, bound, None) {
            Some(srv) => Box::into_raw(Box::new(srv)),
            None => std::ptr::null_mut(),
        }
    })
}

/// Start a UDS stream OSC server bound to `path` (byte string, ptr+len; the
/// socket file is created 0600, replacing a stale file — put it in a 0700
/// directory for the full owner-only guarantee). Null on failure, and always
/// null on Windows (see `pipe.rs` for the named-pipe analogue). Free with
/// [`ss_osc_stream_stop`].
#[no_mangle]
pub unsafe extern "C" fn ss_osc_uds_stream_start(
    ctx: *mut c_void,
    on_packet: StreamPacketFn,
    on_closed: StreamClosedFn,
    path: *const u8,
    path_len: u32,
    max_conns: u32,
) -> *mut SsOscStream {
    #[cfg(unix)]
    {
        no_unwind(std::ptr::null_mut(), || {
            use std::os::unix::fs::PermissionsExt;
            if path.is_null() {
                return std::ptr::null_mut();
            }
            let bytes = slice::from_raw_parts(path, path_len as usize);
            let Ok(path) = std::str::from_utf8(bytes) else {
                return std::ptr::null_mut();
            };
            if path.is_empty() {
                return std::ptr::null_mut();
            }
            let pb = PathBuf::from(path);
            let _ = std::fs::remove_file(&pb); // server owns its socket path
            let listener = match UnixListener::bind(&pb) {
                Ok(l) => l,
                Err(e) => {
                    eprintln!("[osc] UDS stream bind {path} failed: {e}");
                    return std::ptr::null_mut();
                }
            };
            // Owner-only; the bind→chmod window is closed by a 0700 parent dir.
            if std::fs::set_permissions(&pb, std::fs::Permissions::from_mode(0o600)).is_err()
                || listener.set_nonblocking(true).is_err()
            {
                let _ = std::fs::remove_file(&pb);
                return std::ptr::null_mut();
            }
            let host = Host { ctx, on_packet, on_closed };
            match start_socket_server(listener, host, max_conns, 0, Some(pb)) {
                Some(srv) => Box::into_raw(Box::new(srv)),
                None => std::ptr::null_mut(),
            }
        })
    }
    #[cfg(not(unix))]
    {
        let _ = (ctx, on_packet, on_closed, path, path_len, max_conns);
        std::ptr::null_mut()
    }
}

/// The server's actual bound TCP port (useful when started with port 0), or
/// 0 for a path/name-addressed server or a null handle.
#[no_mangle]
pub unsafe extern "C" fn ss_osc_stream_port(handle: *mut SsOscStream) -> i32 {
    if handle.is_null() {
        return 0;
    }
    (*handle).0.port()
}

/// Send one OSC packet, framed, to connection `conn_id`. Returns 1 on send,
/// 0 for an unknown/closed connection or write failure. Safe from any
/// non-audio thread (registry + per-connection write locks).
#[no_mangle]
pub unsafe extern "C" fn ss_osc_stream_send(
    handle: *mut SsOscStream,
    conn_id: u32,
    data: *const u8,
    len: u32,
) -> i32 {
    if handle.is_null() || data.is_null() {
        return 0;
    }
    let me = &*handle;
    let data = slice::from_raw_parts(data, len as usize);
    no_unwind(0, || me.0.send(conn_id, data) as i32)
}

/// Stop accepting, close every connection, join the threads, free the server.
/// `on_closed` does not fire for connections closed by the stop itself.
#[no_mangle]
pub unsafe extern "C" fn ss_osc_stream_stop(handle: *mut SsOscStream) {
    if handle.is_null() {
        return;
    }
    no_unwind((), || drop(Box::from_raw(handle)));
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::collections::VecDeque;
    use std::io::{Read, Write};
    use std::net::TcpStream;
    use std::sync::Mutex;
    use std::time::{Duration, Instant};
    use supersonic_osc::{decode, encode, OscArg};

    // Capture (conn_id, packet) deliveries and closed conn ids.
    struct Cap {
        packets: Mutex<Vec<(u32, Vec<u8>)>>,
        closed: Mutex<Vec<u32>>,
    }
    impl Cap {
        fn new() -> Box<Self> {
            Box::new(Cap { packets: Mutex::new(Vec::new()), closed: Mutex::new(Vec::new()) })
        }
    }
    extern "C" fn on_packet(ctx: *mut c_void, conn: u32, osc: *const u8, len: u32) {
        let c = unsafe { &*(ctx as *const Cap) };
        let bytes = unsafe { slice::from_raw_parts(osc, len as usize) }.to_vec();
        c.packets.lock().unwrap().push((conn, bytes));
    }
    extern "C" fn on_closed(ctx: *mut c_void, conn: u32) {
        let c = unsafe { &*(ctx as *const Cap) };
        c.closed.lock().unwrap().push(conn);
    }

    fn wait_until(mut f: impl FnMut() -> bool) -> bool {
        let deadline = Instant::now() + Duration::from_secs(3);
        while Instant::now() < deadline {
            if f() {
                return true;
            }
            std::thread::sleep(Duration::from_millis(20));
        }
        false
    }

    fn frame(osc: &[u8]) -> Vec<u8> {
        let mut out = Vec::with_capacity(4 + osc.len());
        out.extend_from_slice(&(osc.len() as u32).to_be_bytes());
        out.extend_from_slice(osc);
        out
    }

    // Read one framed packet off a blocking stream (2s timeout set by caller).
    fn read_frame(s: &mut impl Read) -> Option<Vec<u8>> {
        let mut hdr = [0u8; 4];
        s.read_exact(&mut hdr).ok()?;
        let len = u32::from_be_bytes(hdr) as usize;
        let mut body = vec![0u8; len];
        s.read_exact(&mut body).ok()?;
        Some(body)
    }

    fn start_tcp(cap: &Cap, max_conns: u32) -> (*mut SsOscStream, u16) {
        let ctx = cap as *const Cap as *mut c_void;
        let bind = "127.0.0.1";
        let h = unsafe {
            ss_osc_tcp_start(ctx, on_packet, on_closed, 0,
                             bind.as_ptr(), bind.len() as u32, max_conns)
        };
        assert!(!h.is_null(), "tcp server should start");
        let port = unsafe { ss_osc_stream_port(h) };
        assert!(port > 0, "port-0 start should report the real bound port");
        (h, port as u16)
    }

    fn connect(port: u16) -> TcpStream {
        let s = TcpStream::connect(("127.0.0.1", port)).unwrap();
        s.set_read_timeout(Some(Duration::from_secs(2))).unwrap();
        s
    }

    // One client: framed packet in → on_packet with a conn id >= 1; framed
    // reply out via ss_osc_stream_send reaches the client.
    #[test]
    fn tcp_round_trip() {
        let cap = Cap::new();
        let (h, port) = start_tcp(&cap, 4);
        let mut c = connect(port);

        let msg = encode("/status", &[]);
        c.write_all(&frame(&msg)).unwrap();
        let ok = wait_until(|| !cap.packets.lock().unwrap().is_empty());
        assert!(ok, "framed packet should be delivered");
        let (conn, bytes) = cap.packets.lock().unwrap()[0].clone();
        assert!(conn >= 1, "conn ids start at 1 (0 = in-process)");
        assert_eq!(decode(&bytes).unwrap().addr, "/status");

        let reply = encode("/status.reply", &[OscArg::Int(1)]);
        let sent = unsafe { ss_osc_stream_send(h, conn, reply.as_ptr(), reply.len() as u32) };
        assert_eq!(sent, 1);
        let body = read_frame(&mut c).expect("framed reply");
        assert_eq!(decode(&body).unwrap().addr, "/status.reply");

        unsafe { ss_osc_stream_stop(h) };
    }

    // A frame split across writes (even mid-header) reassembles into one packet.
    #[test]
    fn tcp_split_frame_reassembles() {
        let cap = Cap::new();
        let (h, port) = start_tcp(&cap, 4);
        let mut c = connect(port);

        let msg = encode("/s_new", &[OscArg::Str("sine".into()), OscArg::Int(1001)]);
        let framed = frame(&msg);
        c.write_all(&framed[..3]).unwrap(); // mid-header
        std::thread::sleep(Duration::from_millis(150));
        c.write_all(&framed[3..10]).unwrap(); // mid-body
        std::thread::sleep(Duration::from_millis(150));
        c.write_all(&framed[10..]).unwrap();

        let ok = wait_until(|| {
            cap.packets.lock().unwrap().iter().any(|(_, b)| {
                decode(b).map(|m| m.addr == "/s_new").unwrap_or(false)
            })
        });
        assert!(ok, "split frame should reassemble into one packet");
        assert_eq!(cap.packets.lock().unwrap().len(), 1);

        unsafe { ss_osc_stream_stop(h) };
    }

    // Two packets in one write both arrive (no lost second frame).
    #[test]
    fn tcp_coalesced_frames_both_arrive() {
        let cap = Cap::new();
        let (h, port) = start_tcp(&cap, 4);
        let mut c = connect(port);

        let mut both = frame(&encode("/one", &[]));
        both.extend_from_slice(&frame(&encode("/two", &[])));
        c.write_all(&both).unwrap();

        let ok = wait_until(|| cap.packets.lock().unwrap().len() == 2);
        assert!(ok, "both coalesced frames should arrive");
        let addrs: Vec<String> = cap.packets.lock().unwrap().iter()
            .map(|(_, b)| decode(b).unwrap().addr).collect();
        assert_eq!(addrs, vec!["/one", "/two"]);

        unsafe { ss_osc_stream_stop(h) };
    }

    // Admission control: with max_conns=2, the third connection is accepted
    // then immediately closed (crisp EOF), and the first two keep working.
    #[test]
    fn tcp_connection_cap() {
        let cap = Cap::new();
        let (h, port) = start_tcp(&cap, 2);

        let mut c1 = connect(port);
        let mut c2 = connect(port);
        // Ensure both are registered before probing the cap.
        c1.write_all(&frame(&encode("/a", &[]))).unwrap();
        c2.write_all(&frame(&encode("/b", &[]))).unwrap();
        assert!(wait_until(|| cap.packets.lock().unwrap().len() == 2));

        let mut c3 = connect(port);
        let mut buf = [0u8; 1];
        // The over-cap connection is closed by the server: read yields EOF.
        let got = c3.read(&mut buf);
        assert!(matches!(got, Ok(0)) || got.is_err(),
                "over-cap connection should be closed, got {got:?}");

        // Established clients are unaffected.
        c1.write_all(&frame(&encode("/still-alive", &[]))).unwrap();
        assert!(wait_until(|| cap.packets.lock().unwrap().iter().any(|(_, b)| {
            decode(b).map(|m| m.addr == "/still-alive").unwrap_or(false)
        })));

        unsafe { ss_osc_stream_stop(h) };
    }

    // Disconnect fires on_closed with the connection's id, and the id is
    // no longer sendable.
    #[test]
    fn tcp_disconnect_fires_on_closed() {
        let cap = Cap::new();
        let (h, port) = start_tcp(&cap, 4);

        let mut c = connect(port);
        c.write_all(&frame(&encode("/hello", &[]))).unwrap();
        assert!(wait_until(|| !cap.packets.lock().unwrap().is_empty()));
        let conn = cap.packets.lock().unwrap()[0].0;

        drop(c); // client goes away
        assert!(wait_until(|| cap.closed.lock().unwrap().contains(&conn)),
                "on_closed should fire with the dropped connection's id");

        let reply = encode("/late", &[]);
        let sent = unsafe { ss_osc_stream_send(h, conn, reply.as_ptr(), reply.len() as u32) };
        assert_eq!(sent, 0, "a closed conn id must not be sendable");

        unsafe { ss_osc_stream_stop(h) };
    }

    // A frame length beyond MAX_FRAME is a protocol violation: the connection
    // is closed and on_closed fires.
    #[test]
    fn tcp_oversize_frame_closes_connection() {
        let cap = Cap::new();
        let (h, port) = start_tcp(&cap, 4);

        let mut c = connect(port);
        c.write_all(&frame(&encode("/id-probe", &[]))).unwrap();
        assert!(wait_until(|| !cap.packets.lock().unwrap().is_empty()));
        let conn = cap.packets.lock().unwrap()[0].0;

        c.write_all(&0x7FFF_FFFFu32.to_be_bytes()).unwrap();
        assert!(wait_until(|| cap.closed.lock().unwrap().contains(&conn)),
                "oversize frame should close the connection");
        let mut buf = [0u8; 1];
        let got = c.read(&mut buf);
        assert!(matches!(got, Ok(0)) || got.is_err());

        unsafe { ss_osc_stream_stop(h) };
    }

    // Conn ids are unique across reconnects — never reused.
    #[test]
    fn tcp_conn_ids_never_reused() {
        let cap = Cap::new();
        let (h, port) = start_tcp(&cap, 4);

        let mut seen = Vec::new();
        for _ in 0..3 {
            let mut c = connect(port);
            let before = cap.packets.lock().unwrap().len();
            c.write_all(&frame(&encode("/again", &[]))).unwrap();
            assert!(wait_until(|| cap.packets.lock().unwrap().len() > before));
            seen.push(cap.packets.lock().unwrap().last().unwrap().0);
            drop(c);
        }
        seen.dedup();
        assert_eq!(seen.len(), 3, "each connection gets a fresh id: {seen:?}");

        unsafe { ss_osc_stream_stop(h) };
    }

    // A ConnReader replaying a scripted sequence of reads, so the reader loop
    // can be driven through error kinds a real socket won't produce on demand.
    // An exhausted script reads as EOF, which ends run_reader.
    struct ScriptedReader(VecDeque<std::io::Result<Vec<u8>>>);
    impl Read for ScriptedReader {
        fn read(&mut self, buf: &mut [u8]) -> std::io::Result<usize> {
            match self.0.pop_front() {
                Some(Ok(bytes)) => {
                    buf[..bytes.len()].copy_from_slice(&bytes);
                    Ok(bytes.len())
                }
                Some(Err(e)) => Err(e),
                None => Ok(0), // EOF
            }
        }
    }
    struct NullWriter;
    impl ConnWriter for NullWriter {
        fn write_some(&mut self, data: &[u8]) -> std::io::Result<usize> {
            Ok(data.len())
        }
        fn set_write_timeout(&self, _: Duration) {}
        fn shutdown_both(&self) {}
    }
    impl ConnReader for ScriptedReader {
        fn set_timeout(&self, _: Duration) {}
        fn clone_writer(&self) -> std::io::Result<Box<dyn ConnWriter>> {
            Ok(Box::new(NullWriter))
        }
    }

    fn empty_shared() -> Arc<Shared> {
        Arc::new(Shared {
            stop: AtomicBool::new(false),
            conns: Mutex::new(HashMap::new()),
            next_id: AtomicU32::new(1),
            readers: Mutex::new(Vec::new()),
        })
    }

    // A ConnWriter replaying scripted write outcomes, recording everything the
    // wire actually accepted so a test can prove a frame arrived intact.
    struct ScriptedWriter {
        steps: VecDeque<std::io::Result<usize>>, // Ok(n) = accept n bytes
        wrote: Arc<Mutex<Vec<u8>>>,
        shutdowns: Arc<AtomicU32>,
    }
    impl ConnWriter for ScriptedWriter {
        fn write_some(&mut self, data: &[u8]) -> std::io::Result<usize> {
            match self.steps.pop_front() {
                Some(Ok(n)) => {
                    let n = n.min(data.len());
                    self.wrote.lock().unwrap().extend_from_slice(&data[..n]);
                    Ok(n)
                }
                Some(Err(e)) => Err(e),
                None => {
                    self.wrote.lock().unwrap().extend_from_slice(data);
                    Ok(data.len())
                }
            }
        }
        fn set_write_timeout(&self, _: Duration) {}
        fn shutdown_both(&self) {
            self.shutdowns.fetch_add(1, Ordering::Relaxed);
        }
    }

    struct SendProbe {
        server: SocketServer,
        wrote: Arc<Mutex<Vec<u8>>>,
        shutdowns: Arc<AtomicU32>,
    }
    impl SendProbe {
        // A server holding exactly one connection (id 1) whose writer replays
        // `steps`, so send()'s eviction policy can be exercised without a
        // socket that would have to be coaxed into failing.
        fn new(steps: Vec<std::io::Result<usize>>) -> Self {
            let wrote = Arc::new(Mutex::new(Vec::new()));
            let shutdowns = Arc::new(AtomicU32::new(0));
            let w = ScriptedWriter {
                steps: VecDeque::from(steps),
                wrote: wrote.clone(),
                shutdowns: shutdowns.clone(),
            };
            let shared = empty_shared();
            shared.conns.lock().unwrap()
                .insert(1, Arc::new(Mutex::new(Box::new(w) as Box<dyn ConnWriter>)));
            let server = SocketServer {
                shared,
                accept_join: None,
                port: 0,
                path: None,
            };
            SendProbe { server, wrote, shutdowns }
        }
        fn send(&self, osc: &[u8]) -> bool {
            self.server.send(1, osc)
        }
        fn still_registered(&self) -> bool {
            self.server.shared.conns.lock().unwrap().contains_key(&1)
        }
    }

    fn os_err(kind: ErrorKind) -> std::io::Result<usize> {
        Err(std::io::Error::from(kind))
    }

    // A transient kernel refusal (ENOBUFS and friends land in Other/Uncategorized)
    // is not a dead client: nothing of the frame went out, so the stream is still
    // correctly framed and the send can simply be retried.
    #[test]
    fn transient_write_error_keeps_the_connection() {
        let msg = encode("/status.reply", &[OscArg::Int(1)]);
        let p = SendProbe::new(vec![
            Err(std::io::Error::new(ErrorKind::Other, "ENOBUFS")),
            Ok(usize::MAX), // then the whole frame goes out
        ]);

        assert!(p.send(&msg), "a retryable write must still report success");
        assert!(p.still_registered(), "a healthy client must survive a transient");
        assert_eq!(*p.wrote.lock().unwrap(), frame_packet(&msg),
                   "the frame must land exactly once, intact");
        assert_eq!(p.shutdowns.load(Ordering::Relaxed), 0);
    }

    // A transient part-way through a frame resumes at the right offset — the
    // bytes already accepted must not be resent, or the peer desyncs.
    #[test]
    fn transient_mid_frame_resumes_without_duplicating() {
        let msg = encode("/status.reply", &[OscArg::Int(1)]);
        let p = SendProbe::new(vec![
            Ok(6), // partial
            Err(std::io::Error::new(ErrorKind::Other, "ENOBUFS")),
            Ok(usize::MAX), // remainder
        ]);

        assert!(p.send(&msg));
        assert!(p.still_registered());
        assert_eq!(*p.wrote.lock().unwrap(), frame_packet(&msg),
                   "resume must continue from the offset, not restart the frame");
    }

    // EINTR mid-write is the write-side twin of the reader fix: a signal must
    // not cost a frame or a connection.
    #[test]
    fn interrupted_write_is_retried() {
        let msg = encode("/status.reply", &[OscArg::Int(1)]);
        let p = SendProbe::new(vec![
            Ok(4),
            os_err(ErrorKind::Interrupted),
            Ok(usize::MAX),
        ]);

        assert!(p.send(&msg));
        assert!(p.still_registered());
        assert_eq!(*p.wrote.lock().unwrap(), frame_packet(&msg));
    }

    // WouldBlock/TimedOut from a blocking socket means SO_SNDTIMEO expired —
    // a client that stopped reading for WRITE_TIMEOUT. That one must go, or it
    // wedges the gateway thread on every subsequent send.
    #[test]
    fn stuck_client_is_evicted() {
        let msg = encode("/status.reply", &[OscArg::Int(1)]);
        for kind in [ErrorKind::WouldBlock, ErrorKind::TimedOut] {
            let p = SendProbe::new(vec![os_err(kind)]);
            assert!(!p.send(&msg), "{kind:?}: send must report failure");
            assert!(!p.still_registered(), "{kind:?}: stuck client must be evicted");
            assert_eq!(p.shutdowns.load(Ordering::Relaxed), 1,
                       "{kind:?}: eviction must shut the socket down");
        }
    }

    // A dead peer, and a socket accepting nothing, are both terminal.
    #[test]
    fn broken_connection_is_evicted() {
        let msg = encode("/status.reply", &[OscArg::Int(1)]);
        for steps in [vec![os_err(ErrorKind::BrokenPipe)],
                      vec![os_err(ErrorKind::ConnectionReset)],
                      vec![Ok(0)]] {
            let p = SendProbe::new(steps);
            assert!(!p.send(&msg));
            assert!(!p.still_registered(), "a broken connection must be evicted");
        }
    }

    // A transient that never clears still ends the connection rather than
    // parking the gateway thread on one client indefinitely.
    #[test]
    fn endless_transient_gives_up() {
        let msg = encode("/status.reply", &[OscArg::Int(1)]);
        let steps = (0..WRITE_RETRIES + 1)
            .map(|_| Err(std::io::Error::new(ErrorKind::Other, "ENOBUFS")))
            .collect();
        let p = SendProbe::new(steps);

        assert!(!p.send(&msg));
        assert!(!p.still_registered(), "a transient that never clears is terminal");
    }

    // EINTR is not a dead connection: std never retries an interrupted read
    // itself, so a signal landing on a reader thread must not cost the client
    // its connection.
    #[test]
    fn reader_survives_interrupted_read() {
        let cap = Cap::new();
        let host = Host {
            ctx: &*cap as *const Cap as *mut c_void,
            on_packet,
            on_closed,
        };
        let conn = ScriptedReader(VecDeque::from(vec![
            Err(std::io::Error::from(ErrorKind::Interrupted)),
            Ok(frame(&encode("/after-eintr", &[]))),
        ]));

        run_reader(conn, 7, empty_shared(), host); // returns at the script's EOF

        let packets = cap.packets.lock().unwrap();
        assert_eq!(packets.len(), 1,
                   "an interrupted read must not drop the connection");
        assert_eq!(decode(&packets[0].1).unwrap().addr, "/after-eintr");
    }

    #[test]
    fn stop_is_prompt() {
        let cap = Cap::new();
        let (h, port) = start_tcp(&cap, 4);
        let _c = connect(port); // an open connection must not delay stop
        let t0 = Instant::now();
        unsafe { ss_osc_stream_stop(h) };
        assert!(t0.elapsed() < Duration::from_secs(1), "stop should be prompt");
    }

    #[cfg(unix)]
    mod uds {
        use super::*;
        use std::os::unix::fs::PermissionsExt;
        use std::os::unix::net::UnixStream;

        fn tmp(name: &str) -> std::path::PathBuf {
            let dir = std::env::temp_dir()
                .join(format!("ss-stream-test-{}", std::process::id()));
            let _ = std::fs::create_dir_all(&dir);
            dir.join(name)
        }

        // UDS stream: framed round trip works, the socket file is 0600, a
        // stale file is replaced, and stop unlinks the path.
        #[test]
        fn uds_stream_round_trip_and_hygiene() {
            let cap = Cap::new();
            let ctx = &*cap as *const Cap as *mut c_void;
            let path = tmp("stream.sock");
            std::fs::write(&path, b"stale").unwrap();
            let p = path.to_str().unwrap();
            let h = unsafe {
                ss_osc_uds_stream_start(ctx, on_packet, on_closed,
                                        p.as_ptr(), p.len() as u32, 4)
            };
            assert!(!h.is_null(), "uds stream server should start over a stale file");
            let mode = std::fs::metadata(&path).unwrap().permissions().mode();
            assert_eq!(mode & 0o777, 0o600, "socket file must be owner-only");

            let mut c = UnixStream::connect(&path).unwrap();
            c.set_read_timeout(Some(Duration::from_secs(2))).unwrap();
            c.write_all(&frame(&encode("/status", &[]))).unwrap();
            assert!(wait_until(|| !cap.packets.lock().unwrap().is_empty()));
            let (conn, bytes) = cap.packets.lock().unwrap()[0].clone();
            assert_eq!(decode(&bytes).unwrap().addr, "/status");

            let reply = encode("/status.reply", &[OscArg::Int(1)]);
            let sent = unsafe {
                ss_osc_stream_send(h, conn, reply.as_ptr(), reply.len() as u32)
            };
            assert_eq!(sent, 1);
            let body = read_frame(&mut c).expect("framed reply");
            assert_eq!(decode(&body).unwrap().addr, "/status.reply");

            unsafe { ss_osc_stream_stop(h) };
            assert!(!path.exists(), "stop should unlink the socket path");
        }
    }
}

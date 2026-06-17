//! Native C ABI for the OSC subsystem — the seam the C++ engine links against.
//!
//! The engine creates one instance with an emit callback, then drives it:
//! * [`ss_osc_configure`] — (re)bind the cue server (port, loopback, on/off).
//! * [`ss_osc_send`]      — send one OSC packet to an arbitrary host:port
//!                          (off the audio thread).
//!
//! Dual-stack: the cue server binds both IPv4 and IPv6 (loopback → 127.0.0.1 +
//! ::1, all-interfaces → 0.0.0.0 + ::); outbound resolves the destination and
//! sends from the matching-family socket. `std::net` resolves a hostname in a
//! sane family order, so `localhost` (→ ::1 and/or 127.0.0.1) reaches the cue
//! server on whichever family it lands — no IPv4/IPv6 mismatch.
//!
//! Inbound external OSC is re-framed as
//! `/external-osc-cue <ip> <port> <address> <args...>` and handed back via the
//! emit callback (which may fire on a cue recv thread, so the host impl must be
//! thread-safe). The subsystem owns its UDP sockets + recv threads and never
//! touches the audio thread.

use std::ffi::c_void;
use std::io::ErrorKind;
use std::net::{IpAddr, Ipv6Addr, SocketAddr, SocketAddrV6, ToSocketAddrs, UdpSocket};
use std::slice;
use std::sync::atomic::{AtomicBool, AtomicU32, Ordering};
use std::sync::{Arc, Mutex};
use std::thread::JoinHandle;
use std::time::Duration;

use socket2::{Domain, Protocol, Socket, Type};

use supersonic_osc::{decode_packet, encode, OscArg, OscPacket};
// Emit-callback shape + panic fence, shared across the subsystem C ABIs.
pub use supersonic_osc::ffi::{no_unwind, EmitFn, EMIT_BROADCAST};

// Process-wide cap on "dropped malformed inbound OSC" log lines, so a junk
// flood can't spam the log.
static MALFORMED_LOGGED: AtomicU32 = AtomicU32::new(0);

/// The host emit callback + opaque context, made Send/Sync so the cue recv
/// threads can hold a copy. Safety: the C++ engine guarantees `ctx` outlives the
/// instance and the callback is thread-safe.
#[derive(Clone, Copy)]
struct Host {
    ctx: *mut c_void,
    emit: EmitFn,
}
unsafe impl Send for Host {}
unsafe impl Sync for Host {}

impl Host {
    fn emit(&self, osc: &[u8]) {
        (self.emit)(self.ctx, EMIT_BROADCAST, osc.as_ptr(), osc.len() as u32);
    }
}

/// A running cue server: one recv thread per bound socket (IPv4 + IPv6) sharing a
/// stop flag. Dropping it stops the threads (≤ the read timeout) and closes the
/// sockets.
struct CueServer {
    stop: Arc<AtomicBool>,
    joins: Vec<JoinHandle<()>>,
}

impl Drop for CueServer {
    fn drop(&mut self) {
        self.stop.store(true, Ordering::Relaxed);
        for j in self.joins.drain(..) {
            let _ = j.join();
        }
    }
}

struct CueState {
    server: Option<CueServer>,
    port: u16,
    loopback: bool,
}

/// The opaque handle the C++ side owns.
pub struct SsOsc {
    host: Host,
    // Outbound sockets (ephemeral source ports), one per family, bound once at
    // create. Separate from the cue sockets so outbound works regardless of cue
    // state and a socket never sends to its own bound port.
    send4: Option<UdpSocket>,
    send6: Option<UdpSocket>,
    cues_on: Arc<AtomicBool>,
    cue: Mutex<CueState>,
}

// A dual-stack-safe IPv6 UDP bind: only_v6(true) so it coexists with the IPv4
// socket on the same port (without it, `::` is dual-stack on Linux and steals
// the v4 port).
fn bind_v6(addr: Ipv6Addr, port: u16) -> std::io::Result<UdpSocket> {
    let sock = Socket::new(Domain::IPV6, Type::DGRAM, Some(Protocol::UDP))?;
    sock.set_only_v6(true)?;
    let sa = SocketAddr::from(SocketAddrV6::new(addr, port, 0, 0));
    sock.bind(&sa.into())?;
    Ok(sock.into())
}

// Re-frame one inbound message as /external-osc-cue and emit it. Recurses into
// bundles, forwarding each contained message.
fn forward(host: &Host, ip: &str, port: i32, pkt: &OscPacket) {
    match pkt {
        OscPacket::Message(m) => {
            let mut args = Vec::with_capacity(m.args.len() + 3);
            args.push(OscArg::Str(ip.to_string()));
            args.push(OscArg::Int(port));
            args.push(OscArg::Str(m.addr.clone()));
            args.extend(m.args.iter().cloned());
            host.emit(&encode("/external-osc-cue", &args));
        }
        OscPacket::Bundle(b) => {
            for el in &b.elements {
                forward(host, ip, port, el);
            }
        }
    }
}

fn run_cue_server(socket: UdpSocket, stop: Arc<AtomicBool>, cues_on: Arc<AtomicBool>, host: Host) {
    // Timed read so the loop wakes to check `stop`; ~0% idle CPU.
    let _ = socket.set_read_timeout(Some(Duration::from_millis(100)));
    let mut buf = vec![0u8; 65536];
    while !stop.load(Ordering::Relaxed) {
        match socket.recv_from(&mut buf) {
            Ok((n, src)) => {
                // Drain regardless, but only forward when cue reception is on.
                if cues_on.load(Ordering::Relaxed) {
                    match decode_packet(&buf[..n]) {
                        // Fence the codec/emit so a panic drops one datagram rather
                        // than silently killing the recv thread (server bound-but-deaf).
                        Some(pkt) => no_unwind((), || {
                            forward(&host, &src.ip().to_string(), src.port() as i32, &pkt)
                        }),
                        // Don't silently swallow junk — surface the first few so a
                        // misbehaving external sender is debuggable (cap to avoid a
                        // flood).
                        None => {
                            if MALFORMED_LOGGED.fetch_add(1, Ordering::Relaxed) < 5 {
                                eprintln!("[osc] dropped malformed inbound OSC from {} ({n} bytes)", src.ip());
                            }
                        }
                    }
                }
            }
            Err(e) if e.kind() == ErrorKind::WouldBlock || e.kind() == ErrorKind::TimedOut => {}
            Err(_) => break, // socket closed / fatal
        }
    }
}

impl SsOsc {
    fn configure(&self, port: i32, loopback: bool, cues_on: bool) {
        self.cues_on.store(cues_on, Ordering::Relaxed);

        let mut cs = self.cue.lock().unwrap();
        if port <= 0 {
            cs.server = None; // unbind
            cs.port = 0;
            return;
        }
        let port = port as u16;
        if cs.server.is_some() && cs.port == port && cs.loopback == loopback {
            return; // only cues_on changed — no rebind
        }

        cs.server = None; // drop old first (joins threads, frees the ports)

        // Bind IPv4 + IPv6 listeners (each family is best-effort: a host with no
        // IPv6 still gets IPv4, and vice versa).
        let (v4ip, v6ip) = if loopback {
            ("127.0.0.1", Ipv6Addr::LOCALHOST)
        } else {
            ("0.0.0.0", Ipv6Addr::UNSPECIFIED)
        };
        let scope = if loopback { "loopback only" } else { "all interfaces" };

        let mut socks: Vec<(&str, UdpSocket)> = Vec::new();
        match UdpSocket::bind((v4ip, port)) {
            Ok(s) => socks.push(("IPv4", s)),
            Err(e) => eprintln!("[osc] cue server IPv4 bind on port {port} failed: {e}"),
        }
        match bind_v6(v6ip, port) {
            Ok(s) => socks.push(("IPv6", s)),
            Err(e) => eprintln!("[osc] cue server IPv6 bind on port {port} failed: {e}"),
        }
        if socks.is_empty() {
            return;
        }

        let stop = Arc::new(AtomicBool::new(false));
        let mut joins = Vec::new();
        for (fam, sock) in socks {
            let (t_stop, t_on, t_host) = (stop.clone(), self.cues_on.clone(), self.host);
            if let Ok(j) = std::thread::Builder::new()
                .name("ss-osc-cue".into())
                .spawn(move || run_cue_server(sock, t_stop, t_on, t_host))
            {
                joins.push(j);
                eprintln!("[osc] cue server listening on port {port} {fam} ({scope})");
            }
        }
        cs.server = Some(CueServer { stop, joins });
        cs.port = port;
        cs.loopback = loopback;
    }

    fn send(&self, host: &str, port: u16, data: &[u8]) {
        let resolved = match (host, port).to_socket_addrs() {
            Ok(it) => it,
            Err(_) => return,
        };
        // Send to the first resolved address whose family socket exists — so a
        // dual-stack host works, and a v6-only-down host still reaches IPv4.
        for addr in resolved {
            let sock = if addr.is_ipv4() { self.send4.as_ref() } else { self.send6.as_ref() };
            if let Some(s) = sock {
                let _ = s.send_to(data, addr);
                return;
            }
        }
    }
}

/// A raw OSC ingress server: receives datagrams on a bound port (IPv4 + IPv6)
/// and hands the raw OSC bytes to the host callback without any re-framing. The
/// standalone host uses this for its control port (`/osc/at`, `/midi/at`,
/// `/sched/flush`); the cue server above is a separate, cue-specific path.
pub struct SsOscIngress {
    stop: Arc<AtomicBool>,
    joins: Vec<JoinHandle<()>>,
}

impl Drop for SsOscIngress {
    fn drop(&mut self) {
        self.stop.store(true, Ordering::Relaxed);
        for j in self.joins.drain(..) {
            let _ = j.join();
        }
    }
}

fn run_raw_server(socket: UdpSocket, stop: Arc<AtomicBool>, host: Host) {
    let _ = socket.set_read_timeout(Some(Duration::from_millis(100)));
    let mut buf = vec![0u8; 65536];
    while !stop.load(Ordering::Relaxed) {
        match socket.recv_from(&mut buf) {
            Ok((n, _src)) => no_unwind((), || host.emit(&buf[..n])),
            Err(e) if e.kind() == ErrorKind::WouldBlock || e.kind() == ErrorKind::TimedOut => {}
            Err(_) => break,
        }
    }
}

fn start_ingress(host: Host, port: u16, loopback: bool) -> Option<SsOscIngress> {
    let (v4ip, v6ip) = if loopback {
        ("127.0.0.1", Ipv6Addr::LOCALHOST)
    } else {
        ("0.0.0.0", Ipv6Addr::UNSPECIFIED)
    };

    let mut socks: Vec<UdpSocket> = Vec::new();
    match UdpSocket::bind((v4ip, port)) {
        Ok(s) => socks.push(s),
        Err(e) => eprintln!("[osc] ingress IPv4 bind on port {port} failed: {e}"),
    }
    match bind_v6(v6ip, port) {
        Ok(s) => socks.push(s),
        Err(e) => eprintln!("[osc] ingress IPv6 bind on port {port} failed: {e}"),
    }
    if socks.is_empty() {
        return None;
    }

    let stop = Arc::new(AtomicBool::new(false));
    let mut joins = Vec::new();
    for sock in socks {
        let (t_stop, t_host) = (stop.clone(), host);
        if let Ok(j) = std::thread::Builder::new()
            .name("ss-osc-ingress".into())
            .spawn(move || run_raw_server(sock, t_stop, t_host))
        {
            joins.push(j);
        }
    }
    Some(SsOscIngress { stop, joins })
}

// ── Source-bearing ingress ───────────────────────────────────────────────────
// A raw ingress that ALSO delivers the sender's (ip, port) with each datagram, so
// the host can intern (ip,port)->token and address a reply back later. This is
// what an engine transport needs (the plain ingress above drops the source). It
// also takes a bind ADDRESS string rather than a loopback bool, so the caller can
// bind a specific NIC IP (empty = all interfaces).

/// emit-with-source callback: (ctx, ip, ip_len, port, osc, len). `ip` is a byte
/// string valid only for the call; `osc`/`len` is the verbatim datagram.
pub type EmitSrcFn =
    extern "C" fn(*mut c_void, *const u8, u32, i32, *const u8, u32);

#[derive(Clone, Copy)]
struct HostSrc {
    ctx: *mut c_void,
    emit: EmitSrcFn,
}
unsafe impl Send for HostSrc {}
unsafe impl Sync for HostSrc {}

impl HostSrc {
    fn emit(&self, ip: &str, port: i32, osc: &[u8]) {
        (self.emit)(self.ctx, ip.as_ptr(), ip.len() as u32, port, osc.as_ptr(), osc.len() as u32);
    }
}

fn run_raw_server_src(socket: UdpSocket, stop: Arc<AtomicBool>, host: HostSrc) {
    let _ = socket.set_read_timeout(Some(Duration::from_millis(100)));
    let mut buf = vec![0u8; 65536];
    while !stop.load(Ordering::Relaxed) {
        match socket.recv_from(&mut buf) {
            Ok((n, src)) => no_unwind((), || {
                host.emit(&src.ip().to_string(), src.port() as i32, &buf[..n])
            }),
            Err(e) if e.kind() == ErrorKind::WouldBlock || e.kind() == ErrorKind::TimedOut => {}
            Err(_) => break,
        }
    }
}

// Bind the ingress socket(s) for `bind_addr`: empty -> all interfaces, dual-stack
// (0.0.0.0 + ::); a literal IP -> exactly that address (its family); otherwise a
// hostname is resolved. Each family is best-effort.
fn bind_ingress(bind_addr: &str, port: u16) -> Vec<UdpSocket> {
    let mut socks = Vec::new();
    if bind_addr.is_empty() {
        match UdpSocket::bind(("0.0.0.0", port)) {
            Ok(s) => socks.push(s),
            Err(e) => eprintln!("[osc] ingress IPv4 bind on port {port} failed: {e}"),
        }
        match bind_v6(Ipv6Addr::UNSPECIFIED, port) {
            Ok(s) => socks.push(s),
            Err(e) => eprintln!("[osc] ingress IPv6 bind on port {port} failed: {e}"),
        }
    } else {
        let bound = match bind_addr.parse::<IpAddr>() {
            Ok(IpAddr::V6(v6)) => bind_v6(v6, port),
            _ => UdpSocket::bind((bind_addr, port)),  // IPv4 literal or hostname
        };
        match bound {
            Ok(s) => socks.push(s),
            Err(e) => eprintln!("[osc] ingress bind {bind_addr}:{port} failed: {e}"),
        }
    }
    socks
}

fn start_ingress_src(host: HostSrc, port: u16, bind_addr: &str) -> Option<SsOscIngress> {
    let socks = bind_ingress(bind_addr, port);
    if socks.is_empty() {
        return None;
    }
    let stop = Arc::new(AtomicBool::new(false));
    let mut joins = Vec::new();
    for sock in socks {
        let (t_stop, t_host) = (stop.clone(), host);
        if let Ok(j) = std::thread::Builder::new()
            .name("ss-osc-ingress".into())
            .spawn(move || run_raw_server_src(sock, t_stop, t_host))
        {
            joins.push(j);
        }
    }
    Some(SsOscIngress { stop, joins })
}

// ── C ABI ────────────────────────────────────────────────────────────────────

/// Create the OSC subsystem. Returns an owning pointer (null on failure); free
/// with [`ss_osc_destroy`]. `ctx`/`emit` must stay valid until then.
#[no_mangle]
pub extern "C" fn ss_osc_create(ctx: *mut c_void, emit: EmitFn) -> *mut SsOsc {
    no_unwind(std::ptr::null_mut(), || {
        let send4 = UdpSocket::bind(("0.0.0.0", 0)).ok();
        let send6 = UdpSocket::bind((Ipv6Addr::UNSPECIFIED, 0)).ok();
        if send4.is_none() && send6.is_none() {
            eprintln!("[osc] failed to open any outbound socket");
        }
        Box::into_raw(Box::new(SsOsc {
            host: Host { ctx, emit },
            send4,
            send6,
            cues_on: Arc::new(AtomicBool::new(false)),
            cue: Mutex::new(CueState { server: None, port: 0, loopback: true }),
        }))
    })
}

/// Destroy the subsystem: stops the cue recv threads and closes the sockets.
#[no_mangle]
pub unsafe extern "C" fn ss_osc_destroy(handle: *mut SsOsc) {
    if handle.is_null() {
        return;
    }
    no_unwind((), || drop(Box::from_raw(handle)));
}

/// (Re)configure the cue server: bind `port` (0 = unbind) on loopback only
/// (`loopback` != 0) or all interfaces, and toggle inbound cue forwarding
/// (`cues_on` != 0). Off the audio thread.
#[no_mangle]
pub unsafe extern "C" fn ss_osc_configure(handle: *mut SsOsc, port: i32, loopback: i32, cues_on: i32) {
    if handle.is_null() {
        return;
    }
    let me = &*handle;
    no_unwind((), || me.configure(port, loopback != 0, cues_on != 0));
}

/// Send one OSC packet (`data`/`len`) to `host`:`port`. `host` is a byte string
/// (ptr+len, not NUL-terminated; an IPv4/IPv6 literal or a hostname, no brackets).
/// Off the audio thread (the deferred-event dispatch thread).
#[no_mangle]
pub unsafe extern "C" fn ss_osc_send(
    handle: *mut SsOsc,
    host: *const u8,
    host_len: u32,
    port: i32,
    data: *const u8,
    len: u32,
) {
    if handle.is_null() || host.is_null() || data.is_null() || port <= 0 || port > 65535 {
        return;
    }
    let me = &*handle;
    let host_bytes = slice::from_raw_parts(host, host_len as usize);
    let data = slice::from_raw_parts(data, len as usize);
    no_unwind((), || {
        if let Ok(h) = std::str::from_utf8(host_bytes) {
            me.send(h, port as u16, data);
        }
    });
}

/// Start a raw OSC ingress server on `port`: `loopback` != 0 binds 127.0.0.1 +
/// ::1, else 0.0.0.0 + ::. Received datagrams are delivered to `emit` as raw OSC
/// bytes (kind = broadcast). Returns an owning pointer (null on bind failure);
/// free with [`ss_osc_ingress_stop`]. `ctx`/`emit` must outlive it.
#[no_mangle]
pub extern "C" fn ss_osc_ingress_start(
    ctx: *mut c_void,
    emit: EmitFn,
    port: i32,
    loopback: i32,
) -> *mut SsOscIngress {
    no_unwind(std::ptr::null_mut(), || {
        if port <= 0 || port > 65535 {
            return std::ptr::null_mut();
        }
        match start_ingress(Host { ctx, emit }, port as u16, loopback != 0) {
            Some(srv) => Box::into_raw(Box::new(srv)),
            None => std::ptr::null_mut(),
        }
    })
}

/// Start a source-bearing OSC ingress on `port`, bound to `bind_addr` (a byte
/// string, ptr+len; empty = all interfaces, else an IPv4/IPv6 literal or hostname
/// — e.g. "127.0.0.1" for loopback). Each received datagram is delivered verbatim
/// to `emit` ALONG WITH the sender's (ip, port), so the caller can intern an
/// origin token and reply to it. Returns an owning pointer (null on bind failure);
/// free with [`ss_osc_ingress_stop`]. `ctx`/`emit` must outlive it.
#[no_mangle]
pub unsafe extern "C" fn ss_osc_ingress_start_with_src(
    ctx: *mut c_void,
    emit: EmitSrcFn,
    port: i32,
    bind_addr: *const u8,
    bind_addr_len: u32,
) -> *mut SsOscIngress {
    no_unwind(std::ptr::null_mut(), || {
        if port <= 0 || port > 65535 {
            return std::ptr::null_mut();
        }
        let addr = if bind_addr.is_null() {
            ""
        } else {
            std::str::from_utf8(slice::from_raw_parts(bind_addr, bind_addr_len as usize)).unwrap_or("")
        };
        match start_ingress_src(HostSrc { ctx, emit }, port as u16, addr) {
            Some(srv) => Box::into_raw(Box::new(srv)),
            None => std::ptr::null_mut(),
        }
    })
}

/// Stop the ingress recv threads and close the sockets.
#[no_mangle]
pub unsafe extern "C" fn ss_osc_ingress_stop(handle: *mut SsOscIngress) {
    if handle.is_null() {
        return;
    }
    no_unwind((), || drop(Box::from_raw(handle)));
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::net::UdpSocket;
    use std::time::{Duration, Instant};
    use supersonic_osc::{decode, encode, OscArg, OscMessage};

    // Capture emitted /external-osc-cue packets via the C emit callback.
    struct Collector(Mutex<Vec<Vec<u8>>>);
    extern "C" fn collect(ctx: *mut c_void, _kind: i32, osc: *const u8, len: u32) {
        let c = unsafe { &*(ctx as *const Collector) };
        let bytes = unsafe { slice::from_raw_parts(osc, len as usize) }.to_vec();
        c.0.lock().unwrap().push(bytes);
    }
    fn cues(c: &Collector) -> Vec<OscMessage> {
        c.0.lock().unwrap().iter().filter_map(|b| decode(b)).collect()
    }

    fn free_port() -> u16 {
        UdpSocket::bind("127.0.0.1:0").unwrap().local_addr().unwrap().port()
    }
    fn v6_loopback_available() -> bool {
        UdpSocket::bind("[::1]:0").is_ok()
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
    fn send_osc(h: *mut SsOsc, host: &str, port: u16, data: &[u8]) {
        unsafe {
            ss_osc_send(h, host.as_ptr(), host.len() as u32, port as i32,
                        data.as_ptr(), data.len() as u32);
        }
    }

    // INBOUND: external OSC on the cue port (IPv4 and IPv6) is forwarded as an
    // /external-osc-cue carrying the correct sender family.
    #[test]
    fn inbound_ipv4_and_ipv6() {
        let collector = Box::new(Collector(Mutex::new(Vec::new())));
        let ctx = &*collector as *const Collector as *mut c_void;
        let h = ss_osc_create(ctx, collect);
        let have_v6 = v6_loopback_available();

        let port = free_port();
        unsafe { ss_osc_configure(h, port as i32, /*loopback*/ 1, /*cues_on*/ 1) };
        std::thread::sleep(Duration::from_millis(150)); // let recv threads bind

        let s4 = UdpSocket::bind("127.0.0.1:0").unwrap();
        let s6 = if have_v6 { Some(UdpSocket::bind("[::1]:0").unwrap()) } else { None };
        let msg = encode("/hello", &[OscArg::Int(42), OscArg::Str("hi".into())]);

        let ok = wait_until(|| {
            let _ = s4.send_to(&msg, ("127.0.0.1", port));
            if let Some(s6) = &s6 {
                let _ = s6.send_to(&msg, ("::1", port));
            }
            let got = cues(&collector);
            let v4 = got.iter().any(|m| m.args.first().and_then(|a| a.as_str()) == Some("127.0.0.1"));
            let v6 = !have_v6
                || got.iter().any(|m| m.args.first().and_then(|a| a.as_str()).is_some_and(|s| s.contains(':')));
            v4 && v6
        });
        assert!(ok, "expected inbound cues for IPv4{}", if have_v6 { " and IPv6" } else { " (no v6 here)" });

        // Verify the re-frame shape: /external-osc-cue <ip> <port> <addr> <args...>
        let cue = cues(&collector).into_iter().find(|m| m.addr == "/external-osc-cue").unwrap();
        assert_eq!(cue.args.get(2).and_then(|a| a.as_str()), Some("/hello"));
        assert_eq!(cue.args.get(3).and_then(|a| a.as_i32()), Some(42));
        assert_eq!(cue.args.get(4).and_then(|a| a.as_str()), Some("hi"));

        unsafe { ss_osc_destroy(h) };
        drop(collector);
    }

    // OUTBOUND: ss_osc_send delivers to an IPv4 and an IPv6 listener.
    #[test]
    fn outbound_ipv4_and_ipv6() {
        let collector = Box::new(Collector(Mutex::new(Vec::new())));
        let ctx = &*collector as *const Collector as *mut c_void;
        let h = ss_osc_create(ctx, collect);
        let inner = encode("/bar", &[OscArg::Int(7)]);
        let mut buf = [0u8; 1024];

        let l4 = UdpSocket::bind("127.0.0.1:0").unwrap();
        l4.set_read_timeout(Some(Duration::from_secs(2))).unwrap();
        let p4 = l4.local_addr().unwrap().port();
        send_osc(h, "127.0.0.1", p4, &inner);
        let (n, _) = l4.recv_from(&mut buf).expect("IPv4 outbound delivery");
        assert_eq!(decode(&buf[..n]).unwrap().addr, "/bar");

        if v6_loopback_available() {
            let l6 = UdpSocket::bind("[::1]:0").unwrap();
            l6.set_read_timeout(Some(Duration::from_secs(2))).unwrap();
            let p6 = l6.local_addr().unwrap().port();
            send_osc(h, "::1", p6, &inner);
            let (n, _) = l6.recv_from(&mut buf).expect("IPv6 outbound delivery");
            let m = decode(&buf[..n]).unwrap();
            assert_eq!(m.addr, "/bar");
            assert_eq!(m.args.first().and_then(|a| a.as_i32()), Some(7));
        }

        unsafe { ss_osc_destroy(h) };
        drop(collector);
    }

    // SELF-LOOP: send to our own cue port via "localhost" — must round-trip back
    // as a cue regardless of which family localhost resolves to.
    #[test]
    fn self_loop_localhost() {
        let collector = Box::new(Collector(Mutex::new(Vec::new())));
        let ctx = &*collector as *const Collector as *mut c_void;
        let h = ss_osc_create(ctx, collect);
        let port = free_port();
        unsafe { ss_osc_configure(h, port as i32, 1, 1) };
        std::thread::sleep(Duration::from_millis(150));

        let inner = encode("/selfcue", &[OscArg::Int(99)]);
        let ok = wait_until(|| {
            send_osc(h, "localhost", port, &inner);
            cues(&collector).iter().any(|m| m.args.get(2).and_then(|a| a.as_str()) == Some("/selfcue"))
        });
        assert!(ok, "localhost self-loop should round-trip");

        unsafe { ss_osc_destroy(h) };
        drop(collector);
    }

    // "Allow Incoming OSC" semantic: cues_on gates forwarding. Off → inbound is
    // received but dropped (no cue); flipping it on (no rebind) resumes cues.
    #[test]
    fn cues_on_gates_forwarding() {
        let collector = Box::new(Collector(Mutex::new(Vec::new())));
        let ctx = &*collector as *const Collector as *mut c_void;
        let h = ss_osc_create(ctx, collect);
        let port = free_port();
        unsafe { ss_osc_configure(h, port as i32, /*loopback*/ 1, /*cues_on*/ 0) };
        std::thread::sleep(Duration::from_millis(150)); // recv threads bind (and drop)

        let s = UdpSocket::bind("127.0.0.1:0").unwrap();
        let msg = encode("/probe", &[OscArg::Int(1)]);
        for _ in 0..8 {
            let _ = s.send_to(&msg, ("127.0.0.1", port));
            std::thread::sleep(Duration::from_millis(50));
        }
        assert!(cues(&collector).is_empty(), "cues_on=0 must NOT forward inbound OSC");

        // Enable forwarding (same port/loopback → no rebind) and confirm cues flow.
        unsafe { ss_osc_configure(h, port as i32, 1, 1) };
        let ok = wait_until(|| {
            let _ = s.send_to(&msg, ("127.0.0.1", port));
            !cues(&collector).is_empty()
        });
        assert!(ok, "cues_on=1 must forward inbound OSC");

        unsafe { ss_osc_destroy(h) };
        drop(collector);
    }

    // This machine's primary non-loopback IPv4, if any (UDP connect sets a route
    // without sending; local_addr then reveals the chosen source IP).
    fn primary_v4() -> Option<std::net::Ipv4Addr> {
        let s = UdpSocket::bind("0.0.0.0:0").ok()?;
        s.connect("8.8.8.8:80").ok()?;
        match s.local_addr().ok()? {
            SocketAddr::V4(a) if !a.ip().is_loopback() => Some(*a.ip()),
            _ => None,
        }
    }

    // Raw ingress: a datagram to the ingress port is delivered verbatim (not
    // re-framed) to the host callback. IPv4 and IPv6 both reach it.
    #[test]
    fn raw_ingress_delivers_verbatim() {
        let collector = Box::new(Collector(Mutex::new(Vec::new())));
        let ctx = &*collector as *const Collector as *mut c_void;
        let port = free_port();
        let ing = ss_osc_ingress_start(ctx, collect, port as i32, /*loopback*/ 1);
        assert!(!ing.is_null());
        std::thread::sleep(Duration::from_millis(150));

        let msg = encode("/osc/at", &[OscArg::Long(42), OscArg::Str("verbatim".into())]);
        let s = UdpSocket::bind("127.0.0.1:0").unwrap();
        let ok = wait_until(|| {
            let _ = s.send_to(&msg, ("127.0.0.1", port));
            // Delivered without re-framing: address stays /osc/at, args intact.
            cues(&collector).iter().any(|m| {
                m.addr == "/osc/at" && m.args.get(1).and_then(|a| a.as_str()) == Some("verbatim")
            })
        });
        assert!(ok, "raw ingress should deliver the datagram verbatim");

        unsafe { ss_osc_ingress_stop(ing) };
        drop(collector);
    }

    // Source-bearing ingress: the datagram is delivered verbatim AND with the
    // sender's (ip, port), so a transport can intern an origin token and reply.
    #[test]
    fn ingress_with_src_delivers_sender() {
        struct Cap(Mutex<Vec<(String, i32, Vec<u8>)>>);
        extern "C" fn collect_src(ctx: *mut c_void, ip: *const u8, ip_len: u32,
                                  port: i32, osc: *const u8, len: u32) {
            let c = unsafe { &*(ctx as *const Cap) };
            let ips = String::from_utf8_lossy(
                unsafe { slice::from_raw_parts(ip, ip_len as usize) }).to_string();
            let bytes = unsafe { slice::from_raw_parts(osc, len as usize) }.to_vec();
            c.0.lock().unwrap().push((ips, port, bytes));
        }

        let cap = Box::new(Cap(Mutex::new(Vec::new())));
        let ctx = &*cap as *const Cap as *mut c_void;
        let port = free_port();
        let bind = "127.0.0.1";
        let ing = unsafe {
            ss_osc_ingress_start_with_src(ctx, collect_src, port as i32,
                                          bind.as_ptr(), bind.len() as u32)
        };
        assert!(!ing.is_null());
        std::thread::sleep(Duration::from_millis(150));

        let s = UdpSocket::bind("127.0.0.1:0").unwrap();
        let myport = s.local_addr().unwrap().port() as i32;
        let msg = encode("/osc/at", &[OscArg::Long(7)]);
        let ok = wait_until(|| {
            let _ = s.send_to(&msg, ("127.0.0.1", port));
            let g = cap.0.lock().unwrap();
            g.iter().any(|(ip, p, b)| {
                ip == "127.0.0.1" && *p == myport
                    && decode(b).map(|m| m.addr == "/osc/at").unwrap_or(false)
            })
        });
        assert!(ok, "source-bearing ingress should deliver sender ip+port + bytes");

        unsafe { ss_osc_ingress_stop(ing) };
        drop(cap);
    }

    // "Allow OSC From Other Computers" semantic: loopback-only binds 127.0.0.1
    // (+ ::1), so a datagram to this host's real IP is NOT received; all-interfaces
    // binds 0.0.0.0 (+ ::) and IS. Skipped if the box has no non-loopback IPv4.
    #[test]
    fn loopback_scope_restricts_to_local() {
        let Some(ip) = primary_v4() else {
            eprintln!("skip loopback_scope: no non-loopback IPv4 here");
            return;
        };
        let collector = Box::new(Collector(Mutex::new(Vec::new())));
        let ctx = &*collector as *const Collector as *mut c_void;
        let h = ss_osc_create(ctx, collect);
        let port = free_port();
        let s = UdpSocket::bind("0.0.0.0:0").unwrap();
        let msg = encode("/probe", &[OscArg::Int(1)]);

        // Loopback-only: a packet to our real IP must NOT reach the cue server.
        unsafe { ss_osc_configure(h, port as i32, /*loopback*/ 1, /*cues_on*/ 1) };
        std::thread::sleep(Duration::from_millis(200));
        for _ in 0..8 {
            let _ = s.send_to(&msg, (ip, port));
            std::thread::sleep(Duration::from_millis(50));
        }
        assert!(cues(&collector).is_empty(),
                "loopback-only must not receive on the real IP {ip}");

        // All-interfaces: now the same real-IP packet IS received.
        unsafe { ss_osc_configure(h, port as i32, /*loopback*/ 0, 1) };
        let ok = wait_until(|| {
            let _ = s.send_to(&msg, (ip, port));
            !cues(&collector).is_empty()
        });
        assert!(ok, "all-interfaces must receive on the real IP {ip}");

        unsafe { ss_osc_destroy(h) };
        drop(collector);
    }
}

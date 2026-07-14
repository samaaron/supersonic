//! transport_probe — OSC client for the CI transport harness.
//!
//! Two modes over every platform/protocol:
//!
//!   transport_probe <proto> <target>              smoke: one /status, want a reply
//!   transport_probe load <proto> <target> <count> load: blast <count> /sync ids,
//!                                                 verify the /synced replies
//!
//!   proto ∈ udp | tcp (host:port) | uds | uds-dgram (path, unix) | pipe (name, windows)
//!
//! Exit 0 = success; nonzero = transport error, timeout, or a failed load
//! invariant. Stream transports (tcp/uds/pipe) speak the shared 4-byte
//! big-endian length-prefixed framing; datagram transports send raw packets.

use std::io::{Read, Write};
use std::time::{Duration, Instant};

use supersonic_osc::{decode, encode, OscArg};

const TIMEOUT: Duration = Duration::from_secs(3);
// Load runs blast many messages through the real engine; give it wall-clock room.
const LOAD_TIMEOUT: Duration = Duration::from_secs(30);
// Datagram load drains replies until this long passes with none arriving (they
// come back-to-back, so a gap this size means the server is done) — far shorter
// than TIMEOUT so the drain doesn't idle 3s after the last reply.
const DRAIN_GAP: Duration = Duration::from_millis(500);

fn frame(pkt: &[u8]) -> Vec<u8> {
    let mut out = Vec::with_capacity(4 + pkt.len());
    out.extend_from_slice(&(pkt.len() as u32).to_be_bytes());
    out.extend_from_slice(pkt);
    out
}

fn read_frame(r: &mut impl Read) -> std::io::Result<Vec<u8>> {
    let mut hdr = [0u8; 4];
    r.read_exact(&mut hdr)?;
    let len = u32::from_be_bytes(hdr) as usize;
    let mut body = vec![0u8; len];
    r.read_exact(&mut body)?;
    Ok(body)
}

fn report(reply: &[u8]) {
    match decode(reply) {
        Some(m) => println!("OK {} ({} bytes)", m.addr, reply.len()),
        None => println!("OK <undecodable> ({} bytes)", reply.len()),
    }
}

fn run(proto: &str, target: &str, probe: &[u8]) -> Result<Vec<u8>, String> {
    match proto {
        "udp" => {
            let s = std::net::UdpSocket::bind("0.0.0.0:0").map_err(|e| e.to_string())?;
            s.set_read_timeout(Some(TIMEOUT)).ok();
            s.send_to(probe, target).map_err(|e| format!("send: {e}"))?;
            let mut buf = [0u8; 65536];
            let (n, _) = s.recv_from(&mut buf).map_err(|e| format!("recv: {e}"))?;
            Ok(buf[..n].to_vec())
        }
        "tcp" => {
            let mut s = std::net::TcpStream::connect(target).map_err(|e| format!("connect: {e}"))?;
            s.set_read_timeout(Some(TIMEOUT)).ok();
            s.set_write_timeout(Some(TIMEOUT)).ok();
            s.write_all(&frame(probe)).map_err(|e| format!("send: {e}"))?;
            read_frame(&mut s).map_err(|e| format!("recv: {e}"))
        }
        #[cfg(unix)]
        "uds" => {
            let mut s = std::os::unix::net::UnixStream::connect(target)
                .map_err(|e| format!("connect: {e}"))?;
            s.set_read_timeout(Some(TIMEOUT)).ok();
            s.set_write_timeout(Some(TIMEOUT)).ok();
            s.write_all(&frame(probe)).map_err(|e| format!("send: {e}"))?;
            read_frame(&mut s).map_err(|e| format!("recv: {e}"))
        }
        #[cfg(unix)]
        "uds-dgram" => {
            // Bind our own path so the server can address the reply (macOS
            // has no autobind).
            let own = std::env::temp_dir().join(format!("ss-probe-{}.sock", std::process::id()));
            let _ = std::fs::remove_file(&own);
            let s = std::os::unix::net::UnixDatagram::bind(&own)
                .map_err(|e| format!("bind {}: {e}", own.display()))?;
            s.set_read_timeout(Some(TIMEOUT)).ok();
            let res = (|| {
                s.send_to(probe, target).map_err(|e| format!("send: {e}"))?;
                let mut buf = [0u8; 65536];
                let (n, _) = s.recv_from(&mut buf).map_err(|e| format!("recv: {e}"))?;
                Ok(buf[..n].to_vec())
            })();
            let _ = std::fs::remove_file(&own);
            res
        }
        #[cfg(windows)]
        "pipe" => {
            // A named-pipe client is just a file open on \\.\pipe\<name>.
            let full = if target.starts_with(r"\\.\pipe\") {
                target.to_string()
            } else {
                format!(r"\\.\pipe\{target}")
            };
            let mut f = std::fs::OpenOptions::new()
                .read(true)
                .write(true)
                .open(&full)
                .map_err(|e| format!("open {full}: {e}"))?;
            f.write_all(&frame(probe)).map_err(|e| format!("send: {e}"))?;
            // A pipe File has no read timeout; the wall-clock cap in main()
            // bounds this read so a silent server can't hang the probe.
            read_frame(&mut f).map_err(|e| format!("recv: {e}"))
        }
        "shm" => {
            // SHM peer plane: `target` is the segment port. Attach, write the
            // probe into the command ring, and read the first reply back off the
            // reply ring.
            let port: u32 = target.parse().map_err(|_| format!("shm target not a port: {target}"))?;
            let peer = supersonic_osc_net::shm::ShmPeer::open(port)?;
            peer.attach(std::process::id());
            if !peer.write_cmd(probe) {
                return Err("command ring full".into());
            }
            let deadline = Instant::now() + TIMEOUT;
            loop {
                let mut reply = None;
                peer.drain_replies(|p| {
                    if reply.is_none() {
                        reply = Some(p.to_vec());
                    }
                });
                if let Some(r) = reply {
                    return Ok(r);
                }
                if Instant::now() >= deadline {
                    return Err("no reply".into());
                }
                std::thread::sleep(Duration::from_millis(5));
            }
        }
        other => Err(format!("unsupported protocol on this platform: {other}")),
    }
}

// ── load mode ────────────────────────────────────────────────────────────────

fn sync_msg(id: i32) -> Vec<u8> {
    encode("/sync", &[OscArg::Int(id)])
}

/// The id of a `/synced` reply, if that's what the packet is.
fn synced_id(pkt: &[u8]) -> Option<i32> {
    let m = decode(pkt)?;
    if m.addr != "/synced" {
        return None;
    }
    m.args.first().and_then(|a| a.as_i32())
}

/// A bidirectional stream we can duplicate so a reader thread drains replies
/// while the main thread keeps writing (else the server's send buffer fills and
/// it drops us).
trait CloneStream: Read + Write + Send + Sized + 'static {
    fn clone_stream(&self) -> std::io::Result<Self>;
}
impl CloneStream for std::net::TcpStream {
    fn clone_stream(&self) -> std::io::Result<Self> {
        self.try_clone()
    }
}
#[cfg(unix)]
impl CloneStream for std::os::unix::net::UnixStream {
    fn clone_stream(&self) -> std::io::Result<Self> {
        self.try_clone()
    }
}
#[cfg(windows)]
impl CloneStream for std::fs::File {
    fn clone_stream(&self) -> std::io::Result<Self> {
        self.try_clone()
    }
}

/// Blast `count` /sync ids down a reliable stream while a reader thread collects
/// the /synced replies; every id must come back exactly once, in order.
fn stream_load<S: CloneStream>(mut writer: S, count: i32) -> Result<String, String> {
    let reader_conn = writer.clone_stream().map_err(|e| format!("clone: {e}"))?;
    let t0 = Instant::now();
    let reader = std::thread::spawn(move || -> Result<i64, String> {
        let mut r = reader_conn;
        let (mut got, mut last) = (0i64, 0i32);
        while got < count as i64 {
            let body = read_frame(&mut r).map_err(|e| format!("recv after {got}: {e}"))?;
            if let Some(id) = synced_id(&body) {
                if id <= last {
                    return Err(format!("out-of-order reply: {id} after {last}"));
                }
                last = id;
                got += 1;
            }
        }
        Ok(got)
    });
    // Capture a write failure rather than early-returning, so the reader thread
    // is always joined (a broken connection makes its read fail promptly too).
    let mut write_err = None;
    for id in 1..=count {
        if let Err(e) = writer.write_all(&frame(&sync_msg(id))) {
            write_err = Some(format!("send {id}: {e}"));
            break;
        }
    }
    writer.flush().ok();
    let reader_res = reader.join().map_err(|_| "reader panicked".to_string())?;
    if let Some(e) = write_err {
        return Err(e);
    }
    let got = reader_res?;
    if got != count as i64 {
        return Err(format!("only {got}/{count} replies"));
    }
    let secs = t0.elapsed().as_secs_f64();
    Ok(format!("{count} in {secs:.2}s = {:.0} msg/s", count as f64 / secs))
}

/// Blast `count` /sync ids over a datagram socket, then drain replies until they
/// stop. Datagram loss is allowed; corruption and reordering are not.
fn dgram_load(sock: &DgramSock, target: &str, count: i32) -> Result<String, String> {
    let t0 = Instant::now();
    for id in 1..=count {
        let _ = sock.send_to(&sync_msg(id), target);
        if id % 256 == 0 {
            std::thread::sleep(Duration::from_micros(50)); // modest pacing keeps loss sane
        }
    }
    let (mut got, mut last) = (0i64, 0i32);
    let mut buf = [0u8; 65536];
    while got < count as i64 {
        match sock.recv(&mut buf) {
            Ok(n) => {
                if let Some(id) = synced_id(&buf[..n]) {
                    if id <= last {
                        return Err(format!("out-of-order reply: {id} after {last}"));
                    }
                    last = id;
                    got += 1;
                }
            }
            Err(_) => break, // read timeout — no more replies coming
        }
    }
    if got <= count as i64 / 4 {
        return Err(format!("only {got}/{count} delivered — gross loss"));
    }
    let secs = t0.elapsed().as_secs_f64();
    let loss = 100.0 * (count as i64 - got) as f64 / count as f64;
    Ok(format!("{got}/{count} ({loss:.0}% loss) in {secs:.2}s"))
}

/// Minimal datagram abstraction over UDP and (unix) UDS datagram.
enum DgramSock {
    Udp(std::net::UdpSocket),
    #[cfg(unix)]
    Uds(std::os::unix::net::UnixDatagram, std::path::PathBuf),
}
impl DgramSock {
    fn send_to(&self, buf: &[u8], target: &str) -> std::io::Result<usize> {
        match self {
            DgramSock::Udp(s) => s.send_to(buf, target),
            #[cfg(unix)]
            DgramSock::Uds(s, _) => s.send_to(buf, target),
        }
    }
    fn recv(&self, buf: &mut [u8]) -> std::io::Result<usize> {
        match self {
            DgramSock::Udp(s) => s.recv(buf),
            #[cfg(unix)]
            DgramSock::Uds(s, _) => s.recv(buf),
        }
    }
}
#[cfg(unix)]
impl Drop for DgramSock {
    fn drop(&mut self) {
        if let DgramSock::Uds(_, path) = self {
            let _ = std::fs::remove_file(path);
        }
    }
}

fn load(proto: &str, target: &str, count: i32) -> Result<String, String> {
    match proto {
        "tcp" => {
            let s = std::net::TcpStream::connect(target).map_err(|e| format!("connect: {e}"))?;
            s.set_read_timeout(Some(LOAD_TIMEOUT)).ok();
            stream_load(s, count)
        }
        #[cfg(unix)]
        "uds" => {
            let s = std::os::unix::net::UnixStream::connect(target)
                .map_err(|e| format!("connect: {e}"))?;
            s.set_read_timeout(Some(LOAD_TIMEOUT)).ok();
            stream_load(s, count)
        }
        #[cfg(windows)]
        "pipe" => {
            let full = if target.starts_with(r"\\.\pipe\") {
                target.to_string()
            } else {
                format!(r"\\.\pipe\{target}")
            };
            let f = std::fs::OpenOptions::new()
                .read(true)
                .write(true)
                .open(&full)
                .map_err(|e| format!("open {full}: {e}"))?;
            stream_load(f, count)
        }
        "udp" => {
            let s = std::net::UdpSocket::bind("0.0.0.0:0").map_err(|e| e.to_string())?;
            s.set_read_timeout(Some(DRAIN_GAP)).ok();
            dgram_load(&DgramSock::Udp(s), target, count)
        }
        #[cfg(unix)]
        "uds-dgram" => {
            let own = std::env::temp_dir().join(format!("ss-load-{}.sock", std::process::id()));
            let _ = std::fs::remove_file(&own);
            let s = std::os::unix::net::UnixDatagram::bind(&own)
                .map_err(|e| format!("bind {}: {e}", own.display()))?;
            s.set_read_timeout(Some(DRAIN_GAP)).ok();
            dgram_load(&DgramSock::Uds(s, own), target, count)
        }
        "shm" => {
            // Peer-plane load: keep the command ring full (it backpressures
            // losslessly) while continuously draining the reply ring, exactly as
            // a real peer would — so nothing is dropped and /synced ids arrive in
            // order. `target` is the segment port.
            let port: u32 = target.parse().map_err(|_| format!("shm target not a port: {target}"))?;
            let peer = supersonic_osc_net::shm::ShmPeer::open(port)?;
            peer.attach(std::process::id());
            let t0 = Instant::now();
            let (mut sent, mut got, mut last, mut disorder) = (0i32, 0i64, 0i32, false);
            let deadline = Instant::now() + LOAD_TIMEOUT;
            while got < count as i64 {
                while sent < count && peer.write_cmd(&sync_msg(sent + 1)) {
                    sent += 1;
                }
                peer.drain_replies(|p| {
                    if let Some(id) = synced_id(p) {
                        if id <= last {
                            disorder = true;
                        }
                        last = id;
                        got += 1;
                    }
                });
                if disorder {
                    return Err(format!("out-of-order reply near {last}"));
                }
                if Instant::now() >= deadline {
                    return Err(format!("only {got}/{count} replies"));
                }
            }
            let dropped = peer.replies_dropped();
            if dropped != 0 {
                return Err(format!("{dropped} replies dropped (peer fell behind)"));
            }
            let secs = t0.elapsed().as_secs_f64();
            Ok(format!("{count} in {secs:.2}s = {:.0} msg/s", count as f64 / secs))
        }
        other => Err(format!("unsupported protocol on this platform: {other}")),
    }
}

fn main() {
    let args: Vec<String> = std::env::args().collect();

    // Load mode: `transport_probe load <proto> <target> <count>`.
    if args.len() == 5 && args[1] == "load" {
        let (proto, target) = (args[2].clone(), args[3].clone());
        let count: i32 = args[4].parse().unwrap_or(0);
        if count <= 0 {
            eprintln!("load: <count> must be a positive integer");
            std::process::exit(2);
        }
        let (tx, rx) = std::sync::mpsc::channel();
        std::thread::spawn(move || {
            let _ = tx.send(load(&proto, &target, count));
        });
        match rx.recv_timeout(LOAD_TIMEOUT + Duration::from_secs(2)) {
            Ok(Ok(summary)) => println!("LOAD OK {} {}: {summary}", args[2], args[3]),
            Ok(Err(e)) => {
                eprintln!("LOAD FAIL {} {}: {e}", args[2], args[3]);
                std::process::exit(1);
            }
            Err(_) => {
                eprintln!("LOAD FAIL {} {}: timed out", args[2], args[3]);
                std::process::exit(1);
            }
        }
        return;
    }

    if args.len() != 3 {
        eprintln!("usage: transport_probe <udp|tcp|uds|uds-dgram|pipe> <target>");
        eprintln!("       transport_probe load <proto> <target> <count>");
        std::process::exit(2);
    }
    let (proto, target) = (args[1].clone(), args[2].clone());

    // Hard wall-clock cap on the whole probe, enforced here rather than relying
    // on per-transport socket timeouts: a Windows named-pipe File read has no
    // read timeout, so a server that accepts the pipe but never replies would
    // otherwise hang forever (and hang the harness, which runs us with no outer
    // timeout of its own). Run the probe on a worker and give up if it stalls.
    let (tx, rx) = std::sync::mpsc::channel();
    std::thread::spawn(move || {
        let probe = encode("/status", &[]);
        let _ = tx.send(run(&proto, &target, &probe));
    });
    match rx.recv_timeout(TIMEOUT + Duration::from_secs(1)) {
        Ok(Ok(reply)) => report(&reply),
        Ok(Err(e)) => {
            eprintln!("FAIL {} {}: {e}", args[1], args[2]);
            std::process::exit(1);
        }
        Err(_) => {
            eprintln!("FAIL {} {}: timed out", args[1], args[2]);
            std::process::exit(1);
        }
    }
}

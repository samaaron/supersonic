// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2025-2026 Sam Aaron
//! transport_probe — OSC client for the CI transport harness
//! (test/transport-harness/). clockwork's probe, with scsynth's verbs: the
//! load phase blasts /sync and counts the /synced replies, which is what a
//! SuperSonic answers — clockwork's own /clockwork/sync is the substrate's
//! verb, and a guest engine's host does not carry it.
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

use clockwork_osc::{decode, encode, OscArg};

const TIMEOUT: Duration = Duration::from_secs(3);
// Load runs blast many messages through the real engine; give it wall-clock room.
const LOAD_TIMEOUT: Duration = Duration::from_secs(30);
// How long one datagram read waits before the load loop looks at the clock.
const DGRAM_POLL: Duration = Duration::from_millis(100);
// At most this many /sync ids unanswered at once in a datagram load. A Unix
// datagram socket on macOS holds 4,096 bytes by default and a datagram costs
// more than its payload against that; with this few in flight neither the
// engine's socket nor the probe's can fill, however the threads are scheduled.
const DGRAM_WINDOW: i32 = 16;
// No reply for this long with ids unanswered means one was lost. Long enough
// that a runner starving the engine for seconds does not read as loss.
const DGRAM_STALL: Duration = Duration::from_secs(10);

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
            // SHM peer plane: `target` is the engine's attach endpoint, or its
            // port (then the default endpoint for it). Attach, write the probe
            // into the command ring, and read the first reply back off the
            // reply ring.
            let endpoint = match target.parse::<u32>() {
                Ok(port) => clockwork_comms::shm::default_endpoint(port),
                Err(_) => target.to_string(),
            };
            let peer = clockwork_comms::shm::ShmPeer::open(&endpoint)?;
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

/// Windowed single-threaded load for Windows named pipes. stream_load's
/// cloned-handle reader thread can't be used here: a synchronous pipe handle
/// serializes I/O on its file object, so once the writer blocks inside
/// WriteFile (server-inbound buffer full) the reader can no longer issue
/// ReadFile, and with the server's reply write blocked against a full
/// client-inbound buffer the four parties deadlock. Bounding the ids in
/// flight keeps both 64KB pipe buffers nearly empty, so no write can block.
#[cfg(windows)]
fn pipe_load(mut f: std::fs::File, count: i32) -> Result<String, String> {
    // ~24-byte frames each way: 512 in flight ≈ 12KB per direction, a 5x
    // margin under the 64KB pipe buffers that must never fill.
    const WINDOW: i32 = 512;
    let t0 = Instant::now();
    let (mut sent, mut got, mut last) = (0i32, 0i64, 0i32);
    while got < count as i64 {
        while sent < count && sent - (got as i32) < WINDOW {
            sent += 1;
            f.write_all(&frame(&sync_msg(sent)))
                .map_err(|e| format!("send {sent}: {e}"))?;
        }
        let body = read_frame(&mut f).map_err(|e| format!("recv after {got}: {e}"))?;
        if let Some(id) = synced_id(&body) {
            if id <= last {
                return Err(format!("out-of-order reply: {id} after {last}"));
            }
            last = id;
            got += 1;
        }
    }
    let secs = t0.elapsed().as_secs_f64();
    Ok(format!("{count} in {secs:.2}s = {:.0} msg/s", count as f64 / secs))
}

/// A send the kernel refused for want of buffer space, not for anything wrong
/// with the socket. macOS says so with ENOBUFS when the receiving socket is
/// full; other platforms block or report WouldBlock.
fn send_buffer_full(e: &std::io::Error) -> bool {
    if e.kind() == std::io::ErrorKind::WouldBlock {
        return true;
    }
    #[cfg(any(target_os = "macos", target_os = "ios", target_os = "freebsd"))]
    const ENOBUFS: i32 = 55;
    #[cfg(any(target_os = "linux", target_os = "android"))]
    const ENOBUFS: i32 = 105;
    #[cfg(windows)]
    const ENOBUFS: i32 = 10055;
    #[cfg(not(any(target_os = "macos", target_os = "ios", target_os = "freebsd",
                  target_os = "linux", target_os = "android", windows)))]
    const ENOBUFS: i32 = i32::MIN;
    e.raw_os_error() == Some(ENOBUFS)
}

/// Send `count` /sync ids over a datagram socket and require every /synced
/// reply, once and in order, with no more than DGRAM_WINDOW ids unanswered at
/// a time. The same shape as pipe_load, for the same reason: never let a
/// buffer fill.
///
/// This used to send all `count` first, ignore every send error, and only then
/// read. A datagram socket has no backpressure — what does not fit is dropped
/// — so the replies to everything sent while the probe was still sending had
/// nowhere to go, and on macOS its own sends were refused the same way. The
/// count measured scheduling, not the transport: 3,986 to 4,360 of 5,000 on
/// green CI runs, 789 on 2026-09-14 on the same code, which failed a README
/// commit. Draining replies on a second thread while sending was tried and was
/// not enough: a starved reader still let a 4 KB socket fill, and lost up to
/// half the replies on a loaded laptop.
///
/// With the window nothing is dropped however the threads are scheduled, so a
/// missing reply is one the transport lost, and that is a failure.
fn dgram_load(sock: &DgramSock, target: &str, count: i32) -> Result<String, String> {
    let t0 = Instant::now();
    let deadline = t0 + LOAD_TIMEOUT;
    let (mut sent, mut got, mut last) = (0i32, 0i32, 0i32);
    let mut retried = 0u64;
    let mut progress = Instant::now();
    let mut buf = [0u8; 65536];
    while got < count {
        while sent < count && sent - got < DGRAM_WINDOW {
            let msg = sync_msg(sent + 1);
            let give_up = Instant::now() + TIMEOUT;
            loop {
                match sock.send_to(&msg, target) {
                    Ok(_) => break,
                    Err(e) if send_buffer_full(&e) && Instant::now() < give_up => {
                        retried += 1;
                        std::thread::sleep(Duration::from_micros(200));
                    }
                    Err(e) => return Err(format!("send {}: {e}", sent + 1)),
                }
            }
            sent += 1;
        }
        match sock.recv(&mut buf) {
            Ok(n) => {
                if let Some(id) = synced_id(&buf[..n]) {
                    if id <= last {
                        return Err(format!("out-of-order reply: {id} after {last}"));
                    }
                    if id != last + 1 {
                        return Err(format!("reply {id} arrived with {} before it lost", id - last - 1));
                    }
                    last = id;
                    got += 1;
                    progress = Instant::now();
                }
            }
            Err(e) if matches!(e.kind(), std::io::ErrorKind::WouldBlock
                                       | std::io::ErrorKind::TimedOut
                                       | std::io::ErrorKind::Interrupted) => {}
            Err(e) => return Err(format!("recv after {got} replies: {e}")),
        }
        if progress.elapsed() >= DGRAM_STALL {
            return Err(format!("no reply for {}s with {} unanswered: /sync {} was lost",
                               DGRAM_STALL.as_secs(), sent - got, got + 1));
        }
        if Instant::now() >= deadline {
            return Err(format!("only {got}/{count} replies within {}s", LOAD_TIMEOUT.as_secs()));
        }
    }
    let secs = t0.elapsed().as_secs_f64();
    Ok(format!("{count} in {secs:.2}s = {:.0} msg/s, {retried} sends retried", count as f64 / secs))
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
            pipe_load(f, count)
        }
        "udp" => {
            let s = std::net::UdpSocket::bind("0.0.0.0:0").map_err(|e| e.to_string())?;
            s.set_read_timeout(Some(DGRAM_POLL)).ok();
            dgram_load(&DgramSock::Udp(s), target, count)
        }
        #[cfg(unix)]
        "uds-dgram" => {
            let own = std::env::temp_dir().join(format!("ss-load-{}.sock", std::process::id()));
            let _ = std::fs::remove_file(&own);
            let s = std::os::unix::net::UnixDatagram::bind(&own)
                .map_err(|e| format!("bind {}: {e}", own.display()))?;
            s.set_read_timeout(Some(DGRAM_POLL)).ok();
            dgram_load(&DgramSock::Uds(s, own), target, count)
        }
        "shm" => {
            // Peer-plane load: keep the command ring full (it backpressures
            // losslessly) while continuously draining the reply ring, exactly as
            // a real peer would — so nothing is dropped and /synced ids arrive in
            // order. `target` is the engine's attach endpoint, or its port.
            let endpoint = match target.parse::<u32>() {
                Ok(port) => clockwork_comms::shm::default_endpoint(port),
                Err(_) => target.to_string(),
            };
            let peer = clockwork_comms::shm::ShmPeer::open(&endpoint)?;
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
    // otherwise hang forever (and hang clockwork, which runs us with no outer
    // timeout of its own). Run the probe on a worker and give up if it stalls.
    let (tx, rx) = std::sync::mpsc::channel();
    std::thread::spawn(move || {
        let probe = encode("/clockwork/status", &[]);
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

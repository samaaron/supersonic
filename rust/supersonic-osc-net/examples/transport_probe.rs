//! transport_probe — minimal OSC client for the CI transport harness.
//!
//! Sends /status to a running SuperSonic over the chosen transport and waits
//! (≤3s) for a reply. Exit 0 = a reply arrived (printed); nonzero = transport
//! error or timeout. One binary covers every platform/protocol combination:
//!
//!   transport_probe udp       <host:port>
//!   transport_probe tcp       <host:port>
//!   transport_probe uds       <socket path>      (unix)
//!   transport_probe uds-dgram <socket path>      (unix)
//!   transport_probe pipe      <pipe name>        (windows)
//!
//! Stream transports (tcp/uds/pipe) speak the shared 4-byte big-endian
//! length-prefixed framing; datagram transports send raw packets.

use std::io::{Read, Write};
use std::time::Duration;

use supersonic_osc::{decode, encode};

const TIMEOUT: Duration = Duration::from_secs(3);

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
        other => Err(format!("unsupported protocol on this platform: {other}")),
    }
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    if args.len() != 3 {
        eprintln!("usage: transport_probe <udp|tcp|uds|uds-dgram|pipe> <target>");
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

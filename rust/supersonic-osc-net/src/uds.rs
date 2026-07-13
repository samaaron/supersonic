//! Unix-domain-socket datagram OSC ingress — the kernel-ACL'd sibling of the
//! UDP control port (`ffi::ss_osc_ingress_start_with_src`).
//!
//! The server binds a filesystem socket path (created `0600`; put it in a
//! `0700` directory for the full owner-only guarantee) and delivers each
//! datagram verbatim to the host TOGETHER WITH the sender's socket path, so a
//! transport can intern an origin token and address a reply back. A client
//! that wants replies must bind its own path (macOS has no autobind); an
//! unbound sender is delivered with an empty peer path and is unaddressable.
//!
//! Unix-only: on Windows the start function returns null; the named-pipe
//! server (pipe.rs) is the platform's owner-ACL'd analogue.

#[cfg(unix)]
mod imp {
    use std::ffi::c_void;
    use std::io::ErrorKind;
    use std::os::unix::fs::PermissionsExt;
    use std::os::unix::net::UnixDatagram;
    use std::path::{Path, PathBuf};
    use std::sync::atomic::{AtomicBool, Ordering};
    use std::sync::Arc;
    use std::thread::JoinHandle;
    use std::time::Duration;

    use supersonic_osc::ffi::no_unwind;

    /// emit-with-source callback: (ctx, peer_path, peer_len, osc, len). Both
    /// byte strings are valid only for the duration of the call; `peer_path`
    /// is empty when the sender did not bind a path (unaddressable).
    pub type EmitPathFn =
        extern "C" fn(*mut c_void, *const u8, u32, *const u8, u32);

    #[derive(Clone, Copy)]
    struct HostPath {
        ctx: *mut c_void,
        emit: EmitPathFn,
    }
    unsafe impl Send for HostPath {}
    unsafe impl Sync for HostPath {}

    impl HostPath {
        fn emit(&self, peer: &[u8], osc: &[u8]) {
            (self.emit)(self.ctx, peer.as_ptr(), peer.len() as u32,
                        osc.as_ptr(), osc.len() as u32);
        }
    }

    /// A running UDS datagram server: one recv thread; sends go out through a
    /// clone of the bound socket, so replies originate from the server path.
    /// Dropping it stops the thread, closes the socket, and unlinks the path.
    pub struct SsOscUds {
        stop: Arc<AtomicBool>,
        join: Option<JoinHandle<()>>,
        sender: UnixDatagram,
        path: PathBuf,
    }

    impl Drop for SsOscUds {
        fn drop(&mut self) {
            self.stop.store(true, Ordering::Relaxed);
            if let Some(j) = self.join.take() {
                let _ = j.join();
            }
            let _ = std::fs::remove_file(&self.path);
        }
    }

    fn run_recv(socket: UnixDatagram, stop: Arc<AtomicBool>, host: HostPath) {
        // Timed read so the loop wakes to check `stop`; ~0% idle CPU (the same
        // idiom as the UDP recv loops in ffi.rs).
        let _ = socket.set_read_timeout(Some(Duration::from_millis(100)));
        let mut buf = vec![0u8; 65536];
        while !stop.load(Ordering::Relaxed) {
            match socket.recv_from(&mut buf) {
                Ok((n, src)) => {
                    let peer: &[u8] = src
                        .as_pathname()
                        .and_then(Path::to_str)
                        .map(str::as_bytes)
                        .unwrap_or(&[]);
                    // A client that bound with the full sizeof(sockaddr_un)
                    // has its path reported NUL-padded by the kernel; trim so
                    // the peer stays addressable for replies.
                    let end = peer.iter().position(|&b| b == 0).unwrap_or(peer.len());
                    no_unwind((), || host.emit(&peer[..end], &buf[..n]));
                }
                Err(e) if e.kind() == ErrorKind::WouldBlock
                       || e.kind() == ErrorKind::TimedOut => {}
                Err(_) => break, // socket closed / fatal
            }
        }
    }

    pub fn start(host_ctx: *mut c_void, emit: EmitPathFn, path: &str) -> Option<Box<SsOscUds>> {
        let pb = PathBuf::from(path);
        // The server owns its socket path: replace a stale file from a previous
        // run (bind fails on an existing path).
        let _ = std::fs::remove_file(&pb);
        let socket = match UnixDatagram::bind(&pb) {
            Ok(s) => s,
            Err(e) => {
                eprintln!("[osc] UDS dgram bind {path} failed: {e}");
                return None;
            }
        };
        // Owner-only. There is a bind→chmod window; the caller closes it by
        // placing the socket in a 0700 directory (Sonic Pi's session dir).
        if let Err(e) = std::fs::set_permissions(&pb, std::fs::Permissions::from_mode(0o600)) {
            eprintln!("[osc] UDS dgram chmod {path} failed: {e}");
            return None;
        }
        let sender = match socket.try_clone() {
            Ok(s) => s,
            Err(e) => {
                eprintln!("[osc] UDS dgram clone failed: {e}");
                return None;
            }
        };
        let stop = Arc::new(AtomicBool::new(false));
        let host = HostPath { ctx: host_ctx, emit };
        let t_stop = stop.clone();
        let join = std::thread::Builder::new()
            .name("ss-osc-uds".into())
            .spawn(move || run_recv(socket, t_stop, host))
            .ok()?;
        Some(Box::new(SsOscUds { stop, join: Some(join), sender, path: pb }))
    }

    pub fn send(server: &SsOscUds, path: &[u8], data: &[u8]) -> bool {
        let Ok(path) = std::str::from_utf8(path) else { return false };
        if path.is_empty() {
            return false;
        }
        server.sender.send_to(data, path).is_ok()
    }
}

#[cfg(unix)]
pub use imp::{EmitPathFn, SsOscUds};

// Windows: no AF_UNIX in Rust std — the start stub returns null and the C++
// side reports the flag as unsupported (pipe.rs is the Windows analogue).
#[cfg(not(unix))]
pub struct SsOscUds;
#[cfg(not(unix))]
pub type EmitPathFn =
    extern "C" fn(*mut std::ffi::c_void, *const u8, u32, *const u8, u32);

// ── C ABI ────────────────────────────────────────────────────────────────────

use std::ffi::c_void;
#[cfg(unix)]
use std::slice;

/// Start a UDS datagram OSC server bound to `path` (byte string, ptr+len, not
/// NUL-terminated; created 0600, replacing a stale socket file). Each datagram
/// is delivered verbatim to `emit` with the sender's socket path. Returns an
/// owning pointer (null on failure, and always null on Windows); free with
/// [`ss_osc_uds_stop`]. `ctx`/`emit` must outlive it.
#[no_mangle]
pub unsafe extern "C" fn ss_osc_uds_dgram_start(
    ctx: *mut c_void,
    emit: EmitPathFn,
    path: *const u8,
    path_len: u32,
) -> *mut SsOscUds {
    #[cfg(unix)]
    {
        supersonic_osc::ffi::no_unwind(std::ptr::null_mut(), || {
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
            match imp::start(ctx, emit, path) {
                Some(srv) => Box::into_raw(srv),
                None => std::ptr::null_mut(),
            }
        })
    }
    #[cfg(not(unix))]
    {
        let _ = (ctx, emit, path, path_len);
        std::ptr::null_mut()
    }
}

/// Send one OSC packet to the peer bound at `path` (byte string, ptr+len),
/// from the server's own socket (so the client sees the server path as the
/// sender). Returns 1 on send, 0 on failure/empty path. Off the audio thread.
#[no_mangle]
pub unsafe extern "C" fn ss_osc_uds_dgram_send(
    handle: *mut SsOscUds,
    path: *const u8,
    path_len: u32,
    data: *const u8,
    len: u32,
) -> i32 {
    #[cfg(unix)]
    {
        if handle.is_null() || path.is_null() || data.is_null() {
            return 0;
        }
        let me = &*handle;
        let path = slice::from_raw_parts(path, path_len as usize);
        let data = slice::from_raw_parts(data, len as usize);
        supersonic_osc::ffi::no_unwind(0, || imp::send(me, path, data) as i32)
    }
    #[cfg(not(unix))]
    {
        let _ = (handle, path, path_len, data, len);
        0
    }
}

/// Stop the recv thread, close the socket, unlink the path, free the server.
#[no_mangle]
pub unsafe extern "C" fn ss_osc_uds_stop(handle: *mut SsOscUds) {
    if handle.is_null() {
        return;
    }
    #[cfg(unix)]
    supersonic_osc::ffi::no_unwind((), || drop(Box::from_raw(handle)));
    #[cfg(not(unix))]
    let _ = handle;
}

#[cfg(all(test, unix))]
mod tests {
    use super::*;
    use std::os::unix::fs::PermissionsExt;
    use std::os::unix::net::UnixDatagram;
    use std::sync::Mutex;
    use std::time::{Duration, Instant};
    use supersonic_osc::{decode, encode, OscArg};

    // Capture (peer_path, bytes) pairs via the C callback.
    struct Cap(Mutex<Vec<(String, Vec<u8>)>>);
    extern "C" fn collect(ctx: *mut c_void, peer: *const u8, peer_len: u32,
                          osc: *const u8, len: u32) {
        let c = unsafe { &*(ctx as *const Cap) };
        let peer = String::from_utf8_lossy(
            unsafe { slice::from_raw_parts(peer, peer_len as usize) }).to_string();
        let bytes = unsafe { slice::from_raw_parts(osc, len as usize) }.to_vec();
        c.0.lock().unwrap().push((peer, bytes));
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

    fn tmp(name: &str) -> std::path::PathBuf {
        let dir = std::env::temp_dir().join(format!("ss-uds-test-{}", std::process::id()));
        let _ = std::fs::create_dir_all(&dir);
        dir.join(name)
    }

    fn start_server(cap: &Cap, path: &str) -> *mut SsOscUds {
        let ctx = cap as *const Cap as *mut c_void;
        unsafe {
            ss_osc_uds_dgram_start(ctx, collect, path.as_ptr(), path.len() as u32)
        }
    }

    // A bound client's datagram arrives verbatim, with the client's path, and
    // a reply sent to that path round-trips back to the client.
    #[test]
    fn round_trip_with_reply() {
        let cap = Box::new(Cap(Mutex::new(Vec::new())));
        let server_path = tmp("rt-server.sock");
        let client_path = tmp("rt-client.sock");
        let _ = std::fs::remove_file(&client_path);
        let h = start_server(&cap, server_path.to_str().unwrap());
        assert!(!h.is_null());

        let client = UnixDatagram::bind(&client_path).unwrap();
        client.set_read_timeout(Some(Duration::from_secs(2))).unwrap();
        let msg = encode("/s_new", &[OscArg::Str("sine".into()), OscArg::Int(1000)]);
        client.send_to(&msg, &server_path).unwrap();

        let ok = wait_until(|| {
            cap.0.lock().unwrap().iter().any(|(peer, bytes)| {
                peer == client_path.to_str().unwrap()
                    && decode(bytes).map(|m| m.addr == "/s_new").unwrap_or(false)
            })
        });
        assert!(ok, "datagram should arrive with the client's socket path");

        // Reply to the captured peer path; the client receives it.
        let reply = encode("/done", &[OscArg::Str("/s_new".into())]);
        let peer = client_path.to_str().unwrap();
        let sent = unsafe {
            ss_osc_uds_dgram_send(h, peer.as_ptr(), peer.len() as u32,
                                  reply.as_ptr(), reply.len() as u32)
        };
        assert_eq!(sent, 1);
        let mut buf = [0u8; 1024];
        let (n, _) = client.recv_from(&mut buf).expect("reply delivery");
        assert_eq!(decode(&buf[..n]).unwrap().addr, "/done");

        unsafe { ss_osc_uds_stop(h) };
        let _ = std::fs::remove_file(&client_path);
    }

    // An unbound sender is delivered with an empty peer path (unaddressable),
    // and replying to an empty path fails cleanly.
    #[test]
    fn unbound_sender_is_unaddressable() {
        let cap = Box::new(Cap(Mutex::new(Vec::new())));
        let server_path = tmp("unbound-server.sock");
        let h = start_server(&cap, server_path.to_str().unwrap());
        assert!(!h.is_null());

        let client = UnixDatagram::unbound().unwrap();
        let msg = encode("/status", &[]);
        let ok = wait_until(|| {
            let _ = client.send_to(&msg, &server_path);
            cap.0.lock().unwrap().iter().any(|(peer, _)| peer.is_empty())
        });
        assert!(ok, "unbound sender should be delivered with an empty peer path");

        let sent = unsafe {
            ss_osc_uds_dgram_send(h, "".as_ptr(), 0, msg.as_ptr(), msg.len() as u32)
        };
        assert_eq!(sent, 0, "empty peer path must not be sendable");

        unsafe { ss_osc_uds_stop(h) };
    }

    // The socket file is created owner-only (0600) and a stale file from a
    // dead server is replaced on the next start.
    #[test]
    fn socket_file_owner_only_and_stale_replaced() {
        let cap = Box::new(Cap(Mutex::new(Vec::new())));
        let path = tmp("perm-server.sock");
        std::fs::write(&path, b"stale").unwrap(); // simulate a leftover file

        let h = start_server(&cap, path.to_str().unwrap());
        assert!(!h.is_null(), "a stale file at the path must be replaced");
        let mode = std::fs::metadata(&path).unwrap().permissions().mode();
        assert_eq!(mode & 0o777, 0o600, "socket file must be owner-only");

        unsafe { ss_osc_uds_stop(h) };
        assert!(!path.exists(), "stop should unlink the socket path");
    }

    // Stop unlinks the path and joins the recv thread promptly.
    #[test]
    fn stop_is_prompt() {
        let cap = Box::new(Cap(Mutex::new(Vec::new())));
        let path = tmp("stop-server.sock");
        let h = start_server(&cap, path.to_str().unwrap());
        assert!(!h.is_null());
        let t0 = Instant::now();
        unsafe { ss_osc_uds_stop(h) };
        assert!(t0.elapsed() < Duration::from_secs(1), "stop should be prompt");
        assert!(!path.exists());
    }
}

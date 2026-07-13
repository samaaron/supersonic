//! Windows named-pipe stream server — the pipe analogue of the UDS stream
//! server, behind the same [`SsOscStream`] C ABI (conn ids, `on_packet` /
//! `on_closed`, [`ss_osc_stream_send`](crate::stream::ss_osc_stream_send) /
//! `_stop`), so the C++ StreamOscTransport works unchanged over TCP, UDS, and
//! pipes.
//!
//! Wire format is identical too: byte-mode pipes carrying 4-byte big-endian
//! length-prefixed OSC packets, so a client frames the same way over every
//! stream transport.
//!
//! Security: instances are created with an owner-only DACL (SYSTEM + the
//! current user's SID, built via SDDL) and `PIPE_REJECT_REMOTE_CLIENTS`; the
//! first instance uses `FILE_FLAG_FIRST_PIPE_INSTANCE`, so a squatter holding
//! the name fails the start instead of intercepting clients. The instance
//! count is the connection cap — an over-cap client's connect fails busy.
//!
//! ⚠ Not yet exercised on a real Windows machine: coverage so far is a
//! cross-target type-check plus the CI transport harness (run.ps1).

#![allow(clippy::missing_safety_doc)]

use std::ffi::c_void;

use crate::stream::SsOscStream;

#[cfg(windows)]
mod imp {
    use std::collections::HashMap;
    use std::ffi::c_void;
    use std::sync::atomic::{AtomicBool, AtomicU32, Ordering};
    use std::sync::{Arc, Mutex};
    use std::thread::JoinHandle;
    use std::time::Duration;

    use windows_sys::Win32::Foundation::{
        CloseHandle, GetLastError, LocalFree, ERROR_MORE_DATA, ERROR_PIPE_CONNECTED,
        GENERIC_READ, GENERIC_WRITE, HANDLE, INVALID_HANDLE_VALUE,
    };
    use windows_sys::Win32::Security::Authorization::{
        ConvertSidToStringSidW, ConvertStringSecurityDescriptorToSecurityDescriptorW,
        SDDL_REVISION_1,
    };
    use windows_sys::Win32::Security::{
        GetTokenInformation, TokenUser, SECURITY_ATTRIBUTES, TOKEN_QUERY, TOKEN_USER,
    };
    use windows_sys::Win32::Storage::FileSystem::{
        CreateFileW, ReadFile, WriteFile, FILE_FLAG_FIRST_PIPE_INSTANCE, OPEN_EXISTING,
        PIPE_ACCESS_DUPLEX,
    };
    use windows_sys::Win32::System::Pipes::{
        ConnectNamedPipe, CreateNamedPipeW, DisconnectNamedPipe, PeekNamedPipe,
        PIPE_READMODE_BYTE, PIPE_REJECT_REMOTE_CLIENTS, PIPE_TYPE_BYTE, PIPE_WAIT,
    };
    use windows_sys::Win32::System::Threading::{GetCurrentProcess, OpenProcessToken};

    use crate::stream::{drain_frames, frame_packet, Host, StreamServerImpl};

    const PIPE_BUF_BYTES: u32 = 65536;

    /// UTF-16, NUL-terminated.
    fn wide(s: &str) -> Vec<u16> {
        s.encode_utf16().chain(std::iter::once(0)).collect()
    }

    /// An owner-only security descriptor (SYSTEM + the current user), built
    /// from SDDL. Owned pointer, freed with LocalFree.
    struct OwnerSd(*mut c_void);
    unsafe impl Send for OwnerSd {}
    unsafe impl Sync for OwnerSd {}
    impl Drop for OwnerSd {
        fn drop(&mut self) {
            unsafe { LocalFree(self.0) };
        }
    }

    fn owner_only_sd() -> Option<OwnerSd> {
        unsafe {
            // Current user's SID string, from the process token.
            let mut token: HANDLE = std::ptr::null_mut();
            if OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &mut token) == 0 {
                return None;
            }
            let mut needed: u32 = 0;
            GetTokenInformation(token, TokenUser, std::ptr::null_mut(), 0, &mut needed);
            let mut buf = vec![0u8; needed as usize];
            let ok = GetTokenInformation(
                token,
                TokenUser,
                buf.as_mut_ptr() as *mut c_void,
                needed,
                &mut needed,
            );
            CloseHandle(token);
            if ok == 0 {
                return None;
            }
            let user = &*(buf.as_ptr() as *const TOKEN_USER);
            let mut sid_w: *mut u16 = std::ptr::null_mut();
            if ConvertSidToStringSidW(user.User.Sid, &mut sid_w) == 0 {
                return None;
            }
            let mut sid = String::new();
            let mut p = sid_w;
            while *p != 0 {
                sid.push(char::from_u32(*p as u32).unwrap_or('?'));
                p = p.add(1);
            }
            LocalFree(sid_w as *mut c_void);

            // Protected DACL: full control for SYSTEM and the owner, nobody else.
            let sddl = wide(&format!("D:P(A;;GA;;;SY)(A;;GA;;;{sid})"));
            let mut sd: *mut c_void = std::ptr::null_mut();
            if ConvertStringSecurityDescriptorToSecurityDescriptorW(
                sddl.as_ptr(),
                SDDL_REVISION_1,
                &mut sd as *mut *mut c_void as *mut _,
                std::ptr::null_mut(),
            ) == 0
            {
                return None;
            }
            Some(OwnerSd(sd))
        }
    }

    /// One live client connection: the instance handle plus a closed flag,
    /// both guarded by one mutex so a send can never race the reader's
    /// CloseHandle (no write-after-close, no handle-reuse ABA).
    struct PipeConn {
        state: Mutex<(HANDLE, bool)>, // (handle, closed)
    }
    unsafe impl Send for PipeConn {}
    unsafe impl Sync for PipeConn {}

    impl PipeConn {
        fn send(&self, framed: &[u8]) -> bool {
            let guard = self.state.lock().unwrap();
            let (handle, closed) = *guard;
            if closed {
                return false;
            }
            let mut written: u32 = 0;
            unsafe {
                WriteFile(
                    handle,
                    framed.as_ptr(),
                    framed.len() as u32,
                    &mut written,
                    std::ptr::null_mut(),
                ) != 0
                    && written == framed.len() as u32
            }
        }
        /// Disconnect + close under the same lock sends take.
        fn close(&self) {
            let mut guard = self.state.lock().unwrap();
            let (handle, closed) = *guard;
            if !closed {
                unsafe {
                    DisconnectNamedPipe(handle);
                    CloseHandle(handle);
                }
                *guard = (std::ptr::null_mut(), true);
            }
        }
    }

    struct Shared {
        stop: AtomicBool,
        conns: Mutex<HashMap<u32, Arc<PipeConn>>>,
        next_id: AtomicU32, // conn ids from 1, never reused (0 = in-process)
    }

    pub(super) struct PipeServer {
        shared: Arc<Shared>,
        threads: Vec<JoinHandle<()>>,
        name_w: Vec<u16>, // for the stop-wake connects
    }

    impl StreamServerImpl for PipeServer {
        fn send(&self, conn_id: u32, data: &[u8]) -> bool {
            let conn = self.shared.conns.lock().unwrap().get(&conn_id).cloned();
            let Some(conn) = conn else { return false };
            conn.send(&frame_packet(data))
        }
    }

    impl Drop for PipeServer {
        fn drop(&mut self) {
            self.shared.stop.store(true, Ordering::Relaxed);
            // Close live connections (their readers wake within one poll tick)…
            for c in self.shared.conns.lock().unwrap().values() {
                c.close();
            }
            // …and wake every instance blocked in ConnectNamedPipe by briefly
            // connecting to the pipe ourselves.
            for _ in 0..self.threads.len() {
                unsafe {
                    let h = CreateFileW(
                        self.name_w.as_ptr(),
                        GENERIC_READ | GENERIC_WRITE,
                        0,
                        std::ptr::null(),
                        OPEN_EXISTING,
                        0,
                        std::ptr::null_mut(),
                    );
                    if h != INVALID_HANDLE_VALUE {
                        CloseHandle(h);
                    }
                }
            }
            for j in self.threads.drain(..) {
                let _ = j.join();
            }
        }
    }

    /// Serve one connected client until EOF/violation/stop. Peek + read run
    /// under the connection's state lock, so they can never race a concurrent
    /// close() (drop path) or send(); the idle sleep is outside the lock.
    fn serve_client(conn: &PipeConn, id: u32, shared: &Shared, host: &Host) {
        let mut acc: Vec<u8> = Vec::new();
        let mut tmp = [0u8; 8192];
        loop {
            if shared.stop.load(Ordering::Relaxed) {
                break;
            }
            let got: u32 = {
                let guard = conn.state.lock().unwrap();
                let (handle, closed) = *guard;
                if closed {
                    break;
                }
                // Peek-gated read: never block in ReadFile, so the loop stays
                // responsive to stop and to close() from the drop path.
                let mut avail: u32 = 0;
                let peek_ok = unsafe {
                    PeekNamedPipe(
                        handle,
                        std::ptr::null_mut(),
                        0,
                        std::ptr::null_mut(),
                        &mut avail,
                        std::ptr::null_mut(),
                    )
                };
                if peek_ok == 0 {
                    break; // broken pipe — client gone
                }
                if avail == 0 {
                    0
                } else {
                    let want = (avail as usize).min(tmp.len()) as u32;
                    let mut n: u32 = 0;
                    let read_ok = unsafe {
                        ReadFile(handle, tmp.as_mut_ptr(), want, &mut n, std::ptr::null_mut())
                    };
                    // Byte-mode pipes shouldn't produce ERROR_MORE_DATA, but
                    // treat it as a successful partial read if they do.
                    if read_ok == 0 && unsafe { GetLastError() } != ERROR_MORE_DATA {
                        break;
                    }
                    n
                }
            };
            if got == 0 {
                std::thread::sleep(Duration::from_millis(30));
                continue;
            }
            acc.extend_from_slice(&tmp[..got as usize]);
            if !drain_frames(&mut acc, host, id) {
                break; // protocol violation — drop the connection
            }
        }
    }

    /// One pipe instance: (create if needed →) wait for a client → serve →
    /// disconnect → loop. `pre` carries the first, squat-checked instance
    /// (as usize — a raw HANDLE isn't Send).
    fn run_instance(
        mut pre: Option<usize>,
        name_w: Vec<u16>,
        sd: Arc<OwnerSd>,
        max_conns: u32,
        shared: Arc<Shared>,
        host: Host,
    ) {
        while !shared.stop.load(Ordering::Relaxed) {
            let handle = match pre.take() {
                Some(h) => h as HANDLE,
                None => {
                    let sa = SECURITY_ATTRIBUTES {
                        nLength: std::mem::size_of::<SECURITY_ATTRIBUTES>() as u32,
                        lpSecurityDescriptor: sd.0,
                        bInheritHandle: 0,
                    };
                    let h = unsafe {
                        CreateNamedPipeW(
                            name_w.as_ptr(),
                            PIPE_ACCESS_DUPLEX,
                            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT
                                | PIPE_REJECT_REMOTE_CLIENTS,
                            max_conns,
                            PIPE_BUF_BYTES,
                            PIPE_BUF_BYTES,
                            0,
                            &sa,
                        )
                    };
                    if h == INVALID_HANDLE_VALUE {
                        return; // can't recreate the instance — retire the slot
                    }
                    h
                }
            };

            let connected = unsafe {
                ConnectNamedPipe(handle, std::ptr::null_mut()) != 0
                    || GetLastError() == ERROR_PIPE_CONNECTED
            };
            if !connected || shared.stop.load(Ordering::Relaxed) {
                unsafe { CloseHandle(handle) };
                if shared.stop.load(Ordering::Relaxed) {
                    return;
                }
                continue;
            }

            let id = shared.next_id.fetch_add(1, Ordering::Relaxed);
            let conn = Arc::new(PipeConn { state: Mutex::new((handle, false)) });
            shared.conns.lock().unwrap().insert(id, conn.clone());

            serve_client(&conn, id, &shared, &host);

            shared.conns.lock().unwrap().remove(&id);
            conn.close();
            if !shared.stop.load(Ordering::Relaxed) {
                host.closed(id);
            }
        }
    }

    pub(super) fn start(
        host: Host,
        name: &str,
        max_conns: u32,
    ) -> Option<super::SsOscStream> {
        let full = if name.starts_with(r"\\.\pipe\") {
            name.to_string()
        } else {
            format!(r"\\.\pipe\{name}")
        };
        let name_w = wide(&full);
        let sd = Arc::new(owner_only_sd()?);
        let max = max_conns.max(1);

        // First instance is created here, squat-checked, so a hostile process
        // already holding the name fails the START (and start returns null)
        // rather than sitting between us and our clients.
        let sa = SECURITY_ATTRIBUTES {
            nLength: std::mem::size_of::<SECURITY_ATTRIBUTES>() as u32,
            lpSecurityDescriptor: sd.0,
            bInheritHandle: 0,
        };
        let first = unsafe {
            CreateNamedPipeW(
                name_w.as_ptr(),
                PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                max,
                PIPE_BUF_BYTES,
                PIPE_BUF_BYTES,
                0,
                &sa,
            )
        };
        if first == INVALID_HANDLE_VALUE {
            eprintln!("[osc] named pipe create {full} failed (name in use?)");
            return None;
        }

        let shared = Arc::new(Shared {
            stop: AtomicBool::new(false),
            conns: Mutex::new(HashMap::new()),
            next_id: AtomicU32::new(1),
        });
        let mut threads = Vec::new();
        for i in 0..max {
            let pre = if i == 0 { Some(first as usize) } else { None };
            let (t_name, t_sd, t_shared) = (name_w.clone(), sd.clone(), shared.clone());
            if let Ok(j) = std::thread::Builder::new()
                .name("ss-osc-pipe".into())
                .spawn(move || run_instance(pre, t_name, t_sd, max, t_shared, host))
            {
                threads.push(j);
            }
        }
        if threads.is_empty() {
            unsafe { CloseHandle(first) };
            return None;
        }
        Some(super::SsOscStream(Box::new(PipeServer { shared, threads, name_w })))
    }
}

/// Start a named-pipe OSC server (Windows). `name` is a byte string (ptr+len,
/// not NUL-terminated) — either a bare name ("supersonic-cmd") that is
/// prefixed with `\\.\pipe\`, or a full pipe path. Instances carry an
/// owner-only DACL and reject remote clients; `max_conns` is the instance
/// count, so an over-cap connect fails busy. Same handle/send/stop ABI as the
/// socket stream servers. Always null on non-Windows platforms.
#[no_mangle]
pub unsafe extern "C" fn ss_osc_pipe_start(
    ctx: *mut c_void,
    on_packet: crate::stream::StreamPacketFn,
    on_closed: crate::stream::StreamClosedFn,
    name: *const u8,
    name_len: u32,
    max_conns: u32,
) -> *mut SsOscStream {
    #[cfg(windows)]
    {
        supersonic_osc::ffi::no_unwind(std::ptr::null_mut(), || {
            if name.is_null() {
                return std::ptr::null_mut();
            }
            let bytes = std::slice::from_raw_parts(name, name_len as usize);
            let Ok(name) = std::str::from_utf8(bytes) else {
                return std::ptr::null_mut();
            };
            if name.is_empty() {
                return std::ptr::null_mut();
            }
            let host = crate::stream::Host { ctx, on_packet, on_closed };
            match imp::start(host, name, max_conns) {
                Some(srv) => Box::into_raw(Box::new(srv)),
                None => std::ptr::null_mut(),
            }
        })
    }
    #[cfg(not(windows))]
    {
        let _ = (ctx, on_packet, on_closed, name, name_len, max_conns);
        std::ptr::null_mut()
    }
}

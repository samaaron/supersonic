//! Cross-process SHM peer client — the peer side of the SHM command plane
//! (`src/shm_peer_plane.h` / `ShmTransport`). It maps a running server's shared
//! segment, attaches to the plane, writes OSC into the command ring, and reads
//! replies from the reply ring, using the byte-identical Message wire format
//! (`src/ring/ring.h`). The transport harness uses it to drive the
//! `--shm-commands` transport end-to-end on every OS — the SHM analogue of the
//! socket clients in `transport_probe`.
//!
//! This is a Rust-side harness/test client, not part of the C ABI. It mirrors
//! three C++ contracts, and self-checks them at attach so a layout drift fails
//! loudly rather than corrupting the ring:
//!   * segment header offsets — `server_shm.hpp` (all `uint32_t` fields);
//!   * the 64-byte plane header — `shm_peer_plane.h`;
//!   * the ring writer/reader protocol — `RingBufferWriter.h` + `ring_drain.h`.
//! Both processes are the same machine (native endianness), so integers are
//! read/written with `to_ne_bytes`, matching the C++ `memcpy`s.

#![allow(clippy::missing_safety_doc)]

use std::sync::atomic::{fence, AtomicI32, AtomicU32, Ordering};

const SEG_MAGIC: u32 = 0x5C09_E007; // shm_segment_header::MAGIC (E007)
const MESSAGE_MAGIC: u32 = 0xDEAD_BEEF; // ring/ring.h
const PADDING_MAGIC: u32 = 0xBADD_CAFE;
const MSG_HDR: usize = 16; // sizeof(Message)

// shm_segment_header field byte offsets (all u32; see server_shm.hpp). magic is
// field 0; peer_offset is field 27 (SEGMENT-relative), then the plane geometry.
const HDR_MAGIC: usize = 0;
const HDR_PEER_OFFSET: usize = 27 * 4;
const HDR_PEER_CMD_BYTES: usize = 29 * 4;
const HDR_PEER_REP_BYTES: usize = 30 * 4;

// ShmPeerPlaneHeader field byte offsets (64-byte header; shm_peer_plane.h).
const P_OWNER_PID: usize = 0;
const P_GENERATION: usize = 4;
const P_CMD_HEAD: usize = 8;
const P_CMD_TAIL: usize = 12;
const P_CMD_SEQ: usize = 16;
const P_CMD_LOCK: usize = 20;
const P_REP_HEAD: usize = 24;
const P_REP_TAIL: usize = 28;
const P_REP_DROPPED: usize = 36;
const P_CMD_RING_SIZE: usize = 40;
const P_REP_RING_SIZE: usize = 44;
const PLANE_HDR: usize = 64;

// ── platform segment mapping ─────────────────────────────────────────────────

struct Segment {
    base: *mut u8,
    len: usize,
    #[cfg(unix)]
    fd: i32,
    #[cfg(windows)]
    mapping: windows_sys::Win32::Foundation::HANDLE,
}

impl Drop for Segment {
    fn drop(&mut self) {
        unsafe {
            #[cfg(unix)]
            {
                libc::munmap(self.base as *mut libc::c_void, self.len);
                libc::close(self.fd);
            }
            #[cfg(windows)]
            {
                use windows_sys::Win32::Foundation::CloseHandle;
                use windows_sys::Win32::System::Memory::{
                    MEMORY_MAPPED_VIEW_ADDRESS, UnmapViewOfFile,
                };
                UnmapViewOfFile(MEMORY_MAPPED_VIEW_ADDRESS { Value: self.base as *mut _ });
                CloseHandle(self.mapping);
            }
        }
    }
}

#[cfg(unix)]
fn open_segment(port: u32) -> Result<Segment, String> {
    use std::ffi::CString;
    let name = CString::new(format!("/SuperSonic_{port}")).unwrap();
    unsafe {
        let fd = libc::shm_open(name.as_ptr(), libc::O_RDWR, 0);
        if fd < 0 {
            return Err(format!(
                "shm_open SuperSonic_{port}: {}",
                std::io::Error::last_os_error()
            ));
        }
        let mut st: libc::stat = std::mem::zeroed();
        if libc::fstat(fd, &mut st) != 0 {
            libc::close(fd);
            return Err("fstat failed".into());
        }
        let len = st.st_size as usize;
        let base = libc::mmap(
            std::ptr::null_mut(),
            len,
            libc::PROT_READ | libc::PROT_WRITE,
            libc::MAP_SHARED,
            fd,
            0,
        );
        if base == libc::MAP_FAILED {
            libc::close(fd);
            return Err("mmap failed".into());
        }
        Ok(Segment { base: base as *mut u8, len, fd })
    }
}

#[cfg(windows)]
fn open_segment(port: u32) -> Result<Segment, String> {
    use windows_sys::Win32::Foundation::CloseHandle;
    use windows_sys::Win32::System::Memory::{
        MapViewOfFile, OpenFileMappingW, VirtualQuery, FILE_MAP_ALL_ACCESS,
        MEMORY_BASIC_INFORMATION,
    };
    let name: Vec<u16> = format!("SuperSonic_{port}")
        .encode_utf16()
        .chain(std::iter::once(0))
        .collect();
    unsafe {
        let mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, 0, name.as_ptr());
        if mapping.is_null() {
            return Err(format!(
                "OpenFileMapping SuperSonic_{port}: {}",
                std::io::Error::last_os_error()
            ));
        }
        let view = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, 0); // 0 = whole mapping
        if view.Value.is_null() {
            CloseHandle(mapping);
            return Err("MapViewOfFile failed".into());
        }
        // Region size — the mapping has no size query, so measure the view like
        // server_shm.hpp's shm_open_existing does.
        let mut info: MEMORY_BASIC_INFORMATION = std::mem::zeroed();
        VirtualQuery(view.Value, &mut info, std::mem::size_of::<MEMORY_BASIC_INFORMATION>());
        Ok(Segment { base: view.Value as *mut u8, len: info.RegionSize, mapping })
    }
}

impl Segment {
    #[inline]
    fn read_u32(&self, off: usize) -> u32 {
        unsafe { std::ptr::read_unaligned(self.base.add(off) as *const u32) }
    }
    #[inline]
    fn atomic_i32(&self, off: usize) -> &AtomicI32 {
        unsafe { &*(self.base.add(off) as *const AtomicI32) }
    }
    #[inline]
    fn atomic_u32(&self, off: usize) -> &AtomicU32 {
        unsafe { &*(self.base.add(off) as *const AtomicU32) }
    }
    #[inline]
    unsafe fn write(&self, off: usize, src: &[u8]) {
        std::ptr::copy_nonoverlapping(src.as_ptr(), self.base.add(off), src.len());
    }
    #[inline]
    unsafe fn zero(&self, off: usize, len: usize) {
        std::ptr::write_bytes(self.base.add(off), 0, len);
    }
    #[inline]
    fn slice(&self, off: usize, len: usize) -> &[u8] {
        unsafe { std::slice::from_raw_parts(self.base.add(off), len) }
    }
}

// ── the peer ─────────────────────────────────────────────────────────────────

pub struct ShmPeer {
    seg: Segment,
    plane: usize,    // byte offset of the plane header in the segment
    cmd_ring: usize, // byte offset of the command ring base
    cmd_size: i64,
    rep_ring: usize,
    rep_size: i64,
}

impl ShmPeer {
    /// Map the `SuperSonic_<port>` segment and locate its peer plane, validating
    /// the magic and cross-checking the geometry the header and plane both carry.
    pub fn open(port: u32) -> Result<ShmPeer, String> {
        let seg = open_segment(port)?;
        if seg.len < 128 {
            return Err("segment smaller than a header".into());
        }
        let magic = seg.read_u32(HDR_MAGIC);
        if magic != SEG_MAGIC {
            return Err(format!("bad segment magic {magic:#010x} (want {SEG_MAGIC:#010x})"));
        }
        fence(Ordering::Acquire); // pairs with the creator's release before MAGIC

        let plane = seg.read_u32(HDR_PEER_OFFSET) as usize;
        let cmd_size = seg.read_u32(HDR_PEER_CMD_BYTES);
        let rep_size = seg.read_u32(HDR_PEER_REP_BYTES);
        // The plane is alignas(8) and its cursors are accessed as atomics, so a
        // misaligned peer_offset would build misaligned atomic refs (UB). The
        // engine always 8-aligns it; reject anything else rather than trust a
        // valid-magic-but-corrupt segment.
        if plane % 8 != 0 {
            return Err(format!("peer plane offset {plane} is not 8-aligned"));
        }
        let end = plane
            .checked_add(PLANE_HDR + cmd_size as usize + rep_size as usize)
            .ok_or("plane geometry overflow")?;
        if end > seg.len {
            return Err("peer plane runs past the segment".into());
        }
        // The plane header carries the same geometry; a mismatch means we located
        // it wrong (header layout drift) — fail rather than write a bad ring.
        let p_cmd = seg.read_u32(plane + P_CMD_RING_SIZE);
        let p_rep = seg.read_u32(plane + P_REP_RING_SIZE);
        if p_cmd != cmd_size || p_rep != rep_size {
            return Err(format!(
                "plane geometry mismatch: header {cmd_size}/{rep_size} vs plane {p_cmd}/{p_rep}"
            ));
        }
        Ok(ShmPeer {
            cmd_ring: plane + PLANE_HDR,
            rep_ring: plane + PLANE_HDR + cmd_size as usize,
            cmd_size: cmd_size as i64,
            rep_size: rep_size as i64,
            plane,
            seg,
        })
    }

    /// Claim the plane (shm_peer_attach): stamp pid, reset the writer lock, skip
    /// any stale replies, bump the generation.
    pub fn attach(&self, pid: u32) {
        self.seg.atomic_u32(self.plane + P_OWNER_PID).store(pid, Ordering::Relaxed);
        self.seg.atomic_i32(self.plane + P_CMD_LOCK).store(0, Ordering::Relaxed);
        let head = self.seg.atomic_i32(self.plane + P_REP_HEAD).load(Ordering::Acquire);
        self.seg.atomic_i32(self.plane + P_REP_TAIL).store(head, Ordering::Release);
        self.seg.atomic_u32(self.plane + P_GENERATION).fetch_add(1, Ordering::AcqRel);
    }

    pub fn replies_dropped(&self) -> u32 {
        self.seg.atomic_u32(self.plane + P_REP_DROPPED).load(Ordering::Relaxed)
    }

    /// Write one OSC packet into the command ring (RingBufferWriter protocol).
    /// Returns false when the ring is full — backpressure, no blocking.
    pub fn write_cmd(&self, data: &[u8]) -> bool {
        let sz = self.cmd_size;
        let total = (MSG_HDR + data.len()) as i64;
        let aligned = (total + 3) & !3;

        let lock = self.seg.atomic_i32(self.plane + P_CMD_LOCK);
        let head = self.seg.atomic_i32(self.plane + P_CMD_HEAD);
        let tail = self.seg.atomic_i32(self.plane + P_CMD_TAIL);
        let seq = self.seg.atomic_i32(self.plane + P_CMD_SEQ);

        while lock
            .compare_exchange_weak(0, 1, Ordering::Acquire, Ordering::Relaxed)
            .is_err()
        {
            std::hint::spin_loop();
        }

        let h = head.load(Ordering::Relaxed) as i64;
        let t = tail.load(Ordering::Acquire) as i64;
        let used = (h - t + sz) % sz;
        let avail = sz - used - 1;
        if aligned > avail {
            lock.store(0, Ordering::Release);
            return false;
        }

        let mut uh = h;
        let space_to_end = sz - uh;
        if aligned > space_to_end {
            let space_at_front = if t > 0 { t - 1 } else { 0 };
            if aligned > space_at_front {
                lock.store(0, Ordering::Release);
                return false;
            }
            unsafe {
                self.seg.write(self.cmd_ring + uh as usize, &PADDING_MAGIC.to_ne_bytes());
                self.seg.zero(self.cmd_ring + uh as usize + 4, (space_to_end - 4) as usize);
            }
            uh = 0;
        }

        let s = seq.fetch_add(1, Ordering::Relaxed) as u32;
        let mut hdr = [0u8; MSG_HDR];
        hdr[0..4].copy_from_slice(&MESSAGE_MAGIC.to_ne_bytes());
        hdr[4..8].copy_from_slice(&(total as u32).to_ne_bytes());
        hdr[8..12].copy_from_slice(&s.to_ne_bytes());
        // sourceId 0: the host drain re-stamps SHM_PEER_ORIGIN_TOKEN.
        unsafe {
            let base = self.cmd_ring + uh as usize;
            self.seg.write(base, &hdr);
            self.seg.write(base + MSG_HDR, data);
            if aligned > total {
                self.seg.zero(base + total as usize, (aligned - total) as usize);
            }
        }
        head.store(((uh + aligned) % sz) as i32, Ordering::Release);
        lock.store(0, Ordering::Release);
        true
    }

    /// Drain every complete reply frame currently in the reply ring, handing each
    /// payload to `f` (ring_drain protocol; the peer owns rep_tail).
    pub fn drain_replies(&self, mut f: impl FnMut(&[u8])) {
        let sz = self.rep_size;
        let head_a = self.seg.atomic_i32(self.plane + P_REP_HEAD);
        let tail_a = self.seg.atomic_i32(self.plane + P_REP_TAIL);
        loop {
            let head = head_a.load(Ordering::Acquire) as i64;
            let tail = tail_a.load(Ordering::Relaxed) as i64;
            if head == tail {
                break;
            }
            if head < 0 || head >= sz {
                break; // bad head — producer state we can't repair
            }
            if tail < 0 || tail >= sz {
                tail_a.store(head as i32, Ordering::Release);
                break;
            }
            let ut = tail;
            let avail = (head - ut + sz) % sz;
            let space_to_end = sz - ut;
            if space_to_end < 4 || avail < 4 {
                tail_a.store(head as i32, Ordering::Release);
                break;
            }
            let magic = self.seg.read_u32(self.rep_ring + ut as usize);
            if magic == PADDING_MAGIC {
                if ut == 0 {
                    tail_a.store(head as i32, Ordering::Release);
                    break;
                }
                tail_a.store(0, Ordering::Release);
                continue;
            }
            if magic != MESSAGE_MAGIC || space_to_end < MSG_HDR as i64 {
                tail_a.store(head as i32, Ordering::Release);
                break;
            }
            let total = self.seg.read_u32(self.rep_ring + ut as usize + 4) as i64;
            let footprint = (total + 3) & !3;
            if total < MSG_HDR as i64
                || footprint > sz
                || footprint > space_to_end
                || footprint > avail
            {
                tail_a.store(head as i32, Ordering::Release);
                break;
            }
            let payload_size = (total - MSG_HDR as i64) as usize;
            if payload_size > 0 {
                f(self.seg.slice(self.rep_ring + ut as usize + MSG_HDR, payload_size));
            }
            tail_a.store(((ut + footprint) % sz) as i32, Ordering::Release);
        }
    }
}

//! Installing sample data into the engine's buffer table.
//!
//! Three operations, all of them SuperSonic's rather than the engine's:
//! handing a buffer memory that was allocated elsewhere, copying frames into a
//! buffer that already has memory, and reporting what a buffer holds.
//!
//! Where the memory comes from is the reason these exist at all. On web,
//! JavaScript decodes a sample and passes the block in; on native, the sample
//! loader reads it on its own I/O thread. Neither is the engine allocating a
//! buffer for itself, which is all `/b_alloc` knows how to do.
//!
//! # The two arrays
//!
//! The engine keeps the buffer table twice: the one commands write, and the
//! one ugens read. Everything here writes the first and then copies it over
//! the second, so a ugen never sees a buffer half-updated — it either has the
//! old sample or the new one.
//!
//! # Guard samples
//!
//! An interpolating oscillator reads a little before and after the frame it
//! wants, so the web path allocates
//!
//! ```text
//!   [GUARD_BEFORE * channels][audio][GUARD_AFTER * channels]
//! ```
//!
//! and hands over the start of the whole block; the buffer's own pointer is
//! moved past the prefix. The native loader allocates exactly the audio, so it
//! says so and no offset is applied. Getting this wrong does not crash — it
//! plays the sample three samples early, per channel.

#![allow(clippy::missing_safety_doc)]

use std::os::raw::c_int;
use supersonic_ugen_abi::{SndBuf, SndBufUpdates, World};

/// Samples of headroom before the audio, for cubic interpolation.
pub const GUARD_BEFORE: usize = 3;
/// Frames the client allocates AFTER the audio, for the same interpolators
/// reading past the end. This crate never touches them; it is named here so
/// the layout has one definition, and the C that sizes a lane range reads it
/// through the getters below rather than carrying a copy.
pub const GUARD_AFTER: usize = 1;

/// The guard layout, for C. `/b_allocPtr` bounds-checks a lane range against
/// these, and a native test lays a sample out with them; both would otherwise
/// be a third and fourth copy of two numbers.
#[no_mangle]
pub extern "C" fn supersonic_buffer_guard_before() -> u32 { GUARD_BEFORE as u32 }
#[no_mangle]
pub extern "C" fn supersonic_buffer_guard_after() -> u32 { GUARD_AFTER as u32 }

/// The mask a delay line uses to wrap: the largest power of two below `x`,
/// minus one. Same as the engine's own `BUFMASK`.
fn buf_mask(x: i32) -> i32 {
    if x <= 0 {
        return 0;
    }
    (1 << (31 - x.leading_zeros() as i32)) - 1
}

extern "C" {
    /// The engine's log, so a rejected buffer says why in the same stream as
    /// everything else.
    fn clockwork_log(fmt: *const std::os::raw::c_char, ...) -> c_int;
}

unsafe fn log(message: &str) {
    let c = format!("{message}\0");
    clockwork_log(b"%s\0".as_ptr() as *const _, c.as_ptr());
}

/// Whether `bufnum` names a buffer of this world.
unsafe fn in_range(world: *mut World, bufnum: i32) -> bool {
    !world.is_null() && bufnum >= 0 && bufnum < (*world).mNumSndBufs as i32
}

/// The buffer commands write.
unsafe fn nrt_buf(world: *mut World, bufnum: i32) -> *mut SndBuf {
    (*world).mSndBufsNonRealTimeMirror.add(bufnum as usize)
}

/// The buffer ugens read.
unsafe fn rt_buf(world: *mut World, bufnum: i32) -> *mut SndBuf {
    (*world).mSndBufs.add(bufnum as usize)
}

/// What a buffer holds, for a query.
#[repr(C)]
pub struct BufferInfo {
    pub bufnum: c_int,
    pub frames: c_int,
    pub channels: c_int,
    pub samples: c_int,
    pub samplerate: f64,
}

/// Give a buffer memory that was allocated elsewhere.
///
/// `has_guard_samples` says whether `data` points at the guard prefix rather
/// than at the audio. Returns 0, or -1 with the reason logged.
///
/// # Safety
/// `world` must be a live world and `data` must point at
/// `frames * channels` samples, plus the guards if it claims to have them.
#[no_mangle]
pub unsafe extern "C" fn buffer_set_data(
    world: *mut World,
    bufnum: c_int,
    data: *mut f32,
    num_frames: c_int,
    num_channels: c_int,
    sample_rate: f64,
    has_guard_samples: bool,
) -> c_int {
    if world.is_null() || data.is_null() {
        log("[buffer_set_data] Error: NULL world or data pointer");
        return -1;
    }
    if !in_range(world, bufnum) {
        log(&format!(
            "[buffer_set_data] Error: Invalid buffer number {bufnum} (max: {})",
            (*world).mNumSndBufs as i32 - 1
        ));
        return -1;
    }
    if num_frames <= 0 || num_channels <= 0 {
        log(&format!(
            "[buffer_set_data] Error: Invalid dimensions (frames: {num_frames}, channels: {num_channels})"
        ));
        return -1;
    }

    let num_samples = num_frames * num_channels;
    let buf = nrt_buf(world, bufnum);

    (*buf).data = if has_guard_samples {
        data.add(GUARD_BEFORE * num_channels as usize)
    } else {
        data
    };
    (*buf).channels = num_channels;
    (*buf).frames = num_frames;
    (*buf).samples = num_samples;
    // Delay lines wrap on `mask`; interpolating oscillators use `mask1`.
    (*buf).mask = buf_mask(num_samples);
    (*buf).mask1 = (*buf).mask - 1;
    (*buf).samplerate = sample_rate;
    (*buf).sampledur = 1.0 / sample_rate;
    // Neither is used here: the coordinate flag is for FFT chains, and no
    // sound file is held open.
    (*buf).coord = 0;
    (*buf).sndfile = std::ptr::null_mut();

    // Ugens read the other array, so the buffer does not exist for them until
    // this copy lands.
    *rt_buf(world, bufnum) = std::ptr::read(buf);
    let updates = (*world).mSndBufUpdates as *mut SndBufUpdates;
    if !updates.is_null() {
        (*updates.add(bufnum as usize)).writes += 1;
    }
    0
}

/// Copy frames into a buffer that already has memory.
///
/// Truncates rather than overruns when the source is longer than the room
/// left, and says so.
///
/// # Safety
/// As [`buffer_set_data`].
#[no_mangle]
pub unsafe extern "C" fn buffer_read_data(
    world: *mut World,
    bufnum: c_int,
    data: *mut f32,
    num_frames: c_int,
    num_channels: c_int,
    buf_start_frame: c_int,
    _sample_rate: f64,
) -> c_int {
    if world.is_null() || data.is_null() {
        log("[buffer_read_data] Error: NULL world or data pointer");
        return -1;
    }
    if !in_range(world, bufnum) {
        log(&format!("[buffer_read_data] Error: Invalid buffer number {bufnum}"));
        return -1;
    }

    let buf = nrt_buf(world, bufnum);
    if (*buf).data.is_null() {
        log(&format!("[buffer_read_data] Error: Buffer {bufnum} has no data allocated"));
        return -1;
    }
    if buf_start_frame < 0 || buf_start_frame >= (*buf).frames {
        log(&format!(
            "[buffer_read_data] Error: bufStartFrame {buf_start_frame} out of range (0-{})",
            (*buf).frames - 1
        ));
        return -1;
    }
    if num_channels != (*buf).channels {
        log(&format!(
            "[buffer_read_data] Error: Channel mismatch (source: {num_channels}, buffer: {})",
            (*buf).channels
        ));
        return -1;
    }

    let available = (*buf).frames - buf_start_frame;
    let to_write = if num_frames > available {
        log(&format!(
            "[buffer_read_data] Warning: Truncating write from {num_frames} to {available} frames"
        ));
        available
    } else {
        num_frames
    };

    let samples = (to_write * num_channels) as usize;
    let offset = (buf_start_frame * num_channels) as usize;
    std::ptr::copy_nonoverlapping(data, (*buf).data.add(offset), samples);
    0
}

/// Report what a buffer holds.
///
/// # Safety
/// `world` must be live and `info` writable.
#[no_mangle]
pub unsafe extern "C" fn buffer_get_info(
    world: *mut World,
    bufnum: c_int,
    info: *mut BufferInfo,
) -> c_int {
    if world.is_null() || info.is_null() {
        log("[buffer_get_info] Error: NULL world or info pointer");
        return -1;
    }
    if !in_range(world, bufnum) {
        log(&format!("[buffer_get_info] Error: Invalid buffer number {bufnum}"));
        return -1;
    }
    let buf = nrt_buf(world, bufnum);
    (*info).bufnum = bufnum;
    (*info).frames = (*buf).frames;
    (*info).channels = (*buf).channels;
    (*info).samples = (*buf).samples;
    (*info).samplerate = (*buf).samplerate;
    0
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_mask_is_the_power_of_two_below() {
        // What a delay line wraps on: a full power of two of headroom, so the
        // wrap is an AND rather than a modulo.
        assert_eq!(buf_mask(0), 0);
        assert_eq!(buf_mask(1), 0);
        assert_eq!(buf_mask(2), 1);
        assert_eq!(buf_mask(3), 1);
        assert_eq!(buf_mask(4), 3);
        assert_eq!(buf_mask(1023), 511);
        assert_eq!(buf_mask(1024), 1023);
        assert_eq!(buf_mask(1025), 1023);
    }
}

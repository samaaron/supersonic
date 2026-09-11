//! Look-ahead brickwall limiter DSP.
//!
//! The processing core behind the `SPLimiter` / `SPLimiter2` ugens. It has no
//! dependency on the audio engine, deliberately, so the guarantees below can be
//! tested on their own.
//!
//! The design target is minimum latency for a given transparency, because this
//! sits on a master bus where the delay is added to live input monitoring.
//! Latency is exactly `lookahead` samples, not twice it.
//!
//! # How it works
//!
//! Writing `D` for the look-ahead in samples, `x` for the input and `L` for the
//! ceiling, the output is
//!
//! ```text
//!     y[n] = g[n] * x[n-D]
//! ```
//!
//! so the gain `g[n]` is chosen with `D` samples of the signal's future in
//! hand. The required instantaneous gain is
//!
//! ```text
//!     r[n] = min(1, L/|x[n]|)
//! ```
//!
//! which is spiky and discontinuous; multiplying by it directly *is* nonlinear
//! distortion. So `g` is built from `r` in three stages:
//!
//! 1. `m[n]` = min over `k` in `[n-D, n]` of `r[k]` (sliding minimum)
//! 2. `h[n]` = `m[n]` with the rising limb slowed (program-dependent release)
//! 3. `g[n]` = `h` smoothed by a length-`K` kernel (cascaded box filters)
//!
//! This is the same structure as Hämäläinen's peak limiter (DAFx-02), which
//! feeds an order-statistics filter into the gain smoother so the smoothed gain
//! provably cannot exceed the required one; here the statistic is a minimum in
//! the gain domain rather than a maximum in the level domain.
//!
//! # Why the ceiling is guaranteed
//!
//! Stage 3 is a normalised non-negative kernel, so
//!
//! ```text
//!     g[n] = sum over j in [0, K-1] of w[j] * h[n-j],   sum of w[j] = 1
//! ```
//!
//! Stage 2 only ever holds the envelope *below* its input, so `h <= m`
//! pointwise. And `m[n-j]` is a minimum over `[n-j-D, n-j]`, a window which
//! contains index `n-D` for every `j` in `[0, D]`. So as long as `K <= D+1`,
//! every term of the sum is bounded by `r[n-D]`, and therefore
//!
//! ```text
//!     g[n] <= r[n-D]   =>   |y[n]| = g[n]*|x[n-D]| <= L
//! ```
//!
//! with no overshoot and no clipping, for any input whatsoever. The `K <= D+1`
//! constraint is what [`box_length`] enforces when it sizes the box filters.
//! The bound holds for a *constant* `L`; lowering the ceiling takes `D` samples
//! to apply to audio already in the delay line.
//!
//! In stereo the detector runs on `max(|l|, |r|)`, so the single gain is bounded
//! by the required gain of whichever channel needs it most, and the guarantee
//! holds for both. That also means L and R are always scaled identically, which
//! is what keeps the stereo image still when the limiter works; an unlinked pair
//! moves the image on any peak that is not perfectly symmetric.
//!
//! # Why the pieces are the shape they are
//!
//! The sliding minimum is a monotonic deque (Lemire's streaming min-max
//! filter), so it costs O(1) amortised per sample rather than O(D). It also
//! removes only the gain that is genuinely required: a per-block peak-and-ramp
//! scheme instead drags the whole mix down for a full block because one sample
//! in it was loud.
//!
//! The smoothing kernel is three cascaded box filters, which is a quadratic
//! B-spline, continuous in its first derivative — unlike a linear ramp, whose
//! slope discontinuities at both ends are themselves a distortion source.
//! Cascaded boxes give that shape for O(1) per sample via running sums, where a
//! direct FIR would be O(K). Cascaded smoothers for this purpose are also what
//! Sanfilippo (IFC-22) arrives at, using exponentials rather than boxes; boxes
//! are used here because their finite support is what closes the `K <= D+1`
//! proof above.
//!
//! The release is program-dependent, keyed on how long reduction has
//! *persisted* rather than on how deep it currently is. An isolated transient
//! recovers fast, which is inaudible because the transient that caused it masks
//! the recovery. Sustained reduction recovers slowly, because a gain that keeps
//! snapping back between hits modulates the low end audibly, measurable as
//! intermodulation. Keying this on depth instead gets it backwards: heavy
//! programme material is both deep and sustained, and wants the slow limb.
//!
//! Attack needs no such treatment. It is fixed at the look-ahead length, and a
//! gain dip in the couple of milliseconds before a transient is masked by the
//! transient itself.
//!
//! # Where the memory comes from
//!
//! The caller allocates, and the core is placed inside that allocation along
//! with its buffers. That is not an accident of the C++ this replaces: the ugen
//! constructs on the audio thread and takes its scratch from the server's
//! real-time heap, so the core must not reach for the global allocator.

#![allow(clippy::missing_safety_doc)]

use std::os::raw::{c_int, c_void};

/// Smallest look-ahead the core will run at, in samples. Keeps the box filters
/// non-degenerate.
pub const MIN_LOOKAHEAD: i32 = 4;
/// Largest look-ahead, which bounds what a synthdef can ask the server to
/// allocate.
pub const MAX_LOOKAHEAD: i32 = 1 << 16;
/// The most channels one detector links.
pub const MAX_CHANNELS: i32 = 2;

/// Ratio of the slow (sustained reduction) release time to the fast (isolated
/// transient) one.
const SLOW_RELEASE_RATIO: f32 = 8.0;
/// Time constant, in seconds, over which reduction is judged to have persisted.
/// Long enough not to be swayed by a single transient, short enough to reach
/// the slow limb within a bar or two.
const SUSTAIN_TIME_CONSTANT: f32 = 0.15;
/// Average gain drop below unity at which the release is fully slow. 0.25 is a
/// little over 2.5 dB of *sustained* reduction.
const FULL_SUSTAIN_DEPTH: f32 = 0.25;

/// Clamp a requested look-ahead to the supported range.
pub fn clamp_lookahead(samples: i32) -> i32 {
    samples.clamp(MIN_LOOKAHEAD, MAX_LOOKAHEAD)
}

pub fn clamp_channels(channels: i32) -> i32 {
    channels.clamp(1, MAX_CHANNELS)
}

/// Look-ahead in samples for a duration in seconds, clamped.
///
/// Rounds to nearest rather than up: a float seconds value is not exact, so
/// rounding up turns a requested 1.5 ms at 48 kHz into 73 samples rather than
/// 72 purely on the representation error.
pub fn lookahead_samples(seconds: f32, sample_rate: f64) -> i32 {
    if !(seconds > 0.0) || !(sample_rate > 0.0) {
        return MIN_LOOKAHEAD;
    }
    let n = (seconds as f64 * sample_rate + 0.5).floor();
    if n > MAX_LOOKAHEAD as f64 {
        return MAX_LOOKAHEAD;
    }
    clamp_lookahead(n as i32)
}

/// Box-filter length for a look-ahead of `d` samples.
///
/// Three cascaded boxes of length `B` give a kernel of length `3B-2`, and the
/// ceiling proof needs that to be `<= d+1`, which this satisfies for every
/// `d >= 0`, since `3*floor((d+3)/3) <= d+3`.
pub fn box_length(d: i32) -> i32 {
    ((d + 3) / 3).max(1)
}

/// Kernel length actually in use. Exposed so tests can assert the `K <= D+1`
/// invariant the ceiling guarantee rests on.
pub fn kernel_length(d: i32) -> i32 {
    box_length(d) * 3 - 2
}

/// Deque capacity for a look-ahead of `d` samples.
///
/// The sliding minimum spans `d+1` indices, and a push happens before the
/// now-stale head is evicted, so the deque holds up to `d+2` entries, plus one
/// slot to keep "full" distinguishable from "empty".
pub fn deque_capacity(d: i32) -> i32 {
    d + 3
}

/// Bytes of scratch the buffers need, not counting the core itself.
fn buffer_bytes(lookahead: i32, channels: i32) -> usize {
    let d = clamp_lookahead(lookahead);
    let cap = deque_capacity(d);
    let floats = d as usize * clamp_channels(channels) as usize
        + cap as usize
        + box_length(d) as usize * 3;
    let ints = cap as usize;
    floats * 4 + ints * 4
}

/// Total memory the core needs, in bytes, the core's own state included.
///
/// Callers allocate this once and hand it to [`SpLimiter::place`].
pub fn memory_bytes(lookahead: i32, channels: i32) -> usize {
    let state = std::mem::size_of::<SpLimiter>();
    let aligned = (state + 7) & !7;
    aligned + buffer_bytes(lookahead, channels)
}

/// The limiter, and where its buffers live inside the caller's allocation.
///
/// The buffers are named by offset rather than by pointer, so the whole thing
/// is one block that can be moved or freed as a unit and cannot outlive its
/// own storage.
#[repr(C)]
pub struct SpLimiter {
    /// Start of the buffers, which follow this struct in the same block.
    buffers: *mut u8,

    lookahead: i32,
    channels: i32,
    deque_cap: i32,
    box_len: i32,
    sample_rate: f64,

    /// Offsets into `buffers`, in elements of their own type.
    delay_off: usize,
    deque_vals_off: usize,
    box_off: usize,
    deque_idx_off: usize,

    delay_pos: i32,
    head: i32,
    tail: i32,
    count: i32,
    sample_index: u32,

    box_sum: [f64; 3],
    box_pos: [i32; 3],

    env: f32,
    gain: f32,
    sustain: f32,
}

/// One block's worth of parameters, computed once per call rather than per
/// sample.
///
/// Public so a caller processing sample by sample — the C ABI, whose input and
/// output may be the same buffer — pays for them once too.
#[derive(Clone, Copy)]
pub struct Coefs {
    level: f32,
    fast: f32,
    slow: f32,
    sustain_alpha: f32,
}

impl SpLimiter {
    /// Build a limiter inside `mem`, which must be at least
    /// [`memory_bytes`] and aligned for `f64`.
    ///
    /// # Safety
    /// `mem` must point at that much writable memory, and the returned
    /// reference must not outlive it.
    pub unsafe fn place(
        mem: *mut u8,
        lookahead: i32,
        channels: i32,
        sample_rate: f64,
    ) -> *mut SpLimiter {
        let d = clamp_lookahead(lookahead);
        let ch = clamp_channels(channels);
        let cap = deque_capacity(d);
        let box_len = box_length(d);

        let state = (std::mem::size_of::<SpLimiter>() + 7) & !7;
        let buffers = mem.add(state);

        // Floats first, then the deque's indices, so every f32 run stays
        // 4-aligned behind the 8-aligned struct.
        let delay_off = 0usize;
        let deque_vals_off = delay_off + (d as usize * ch as usize);
        let box_off = deque_vals_off + cap as usize;
        let float_count = box_off + box_len as usize * 3;
        let deque_idx_off = float_count;

        let core = mem as *mut SpLimiter;
        std::ptr::write(
            core,
            SpLimiter {
                buffers,
                lookahead: d,
                channels: ch,
                deque_cap: cap,
                box_len,
                sample_rate: if sample_rate > 0.0 { sample_rate } else { 44100.0 },
                delay_off,
                deque_vals_off,
                box_off,
                deque_idx_off,
                delay_pos: 0,
                head: 0,
                tail: 0,
                count: 0,
                sample_index: 0,
                box_sum: [0.0; 3],
                box_pos: [0; 3],
                env: 1.0,
                gain: 1.0,
                sustain: 0.0,
            },
        );
        (*core).reset();
        core
    }

    fn floats(&mut self, off: usize, len: usize) -> &mut [f32] {
        unsafe { std::slice::from_raw_parts_mut((self.buffers as *mut f32).add(off), len) }
    }

    fn delay(&mut self, channel: usize) -> &mut [f32] {
        let d = self.lookahead as usize;
        let off = self.delay_off + channel * d;
        self.floats(off, d)
    }

    fn deque_vals(&mut self) -> &mut [f32] {
        let (off, len) = (self.deque_vals_off, self.deque_cap as usize);
        self.floats(off, len)
    }

    fn box_buf(&mut self, which: usize) -> &mut [f32] {
        let len = self.box_len as usize;
        let off = self.box_off + which * len;
        self.floats(off, len)
    }

    fn deque_idx(&mut self) -> &mut [u32] {
        let (off, len) = (self.deque_idx_off, self.deque_cap as usize);
        unsafe { std::slice::from_raw_parts_mut((self.buffers as *mut u32).add(off), len) }
    }

    /// Zero the delay lines and park the gain at unity.
    pub fn reset(&mut self) {
        for c in 0..self.channels as usize {
            self.delay(c).fill(0.0);
        }
        self.deque_vals().fill(0.0);
        self.deque_idx().fill(0);
        let box_len = self.box_len;
        for b in 0..3 {
            self.box_buf(b).fill(1.0);
            self.box_sum[b] = box_len as f64;
            self.box_pos[b] = 0;
        }
        self.delay_pos = 0;
        self.head = 0;
        self.tail = 0;
        self.count = 0;
        self.sample_index = 0;
        self.env = 1.0;
        self.gain = 1.0;
        self.sustain = 0.0;
    }

    /// Latency in samples: the delay from an input sample to the output sample
    /// carrying it.
    pub fn latency_samples(&self) -> i32 {
        self.lookahead
    }

    pub fn channels(&self) -> i32 {
        self.channels
    }

    /// Current smoothed gain, for metering.
    pub fn current_gain(&self) -> f32 {
        self.gain
    }

    /// One-pole coefficient for a time constant in seconds. Larger means
    /// slower.
    fn one_pole_coef(&self, seconds: f32) -> f32 {
        let n = seconds as f64 * self.sample_rate;
        if n < 1.0 {
            return 0.0;
        }
        (-1.0 / n).exp() as f32
    }

    /// The per-block parameters, to be handed back to [`step_mono`] or
    /// [`step_stereo`] for every sample of that block.
    ///
    /// [`step_mono`]: SpLimiter::step_mono
    /// [`step_stereo`]: SpLimiter::step_stereo
    pub fn begin(&self, level: f32, release: f32) -> Coefs {
        self.coefs(level, release)
    }

    fn coefs(&self, level: f32, release: f32) -> Coefs {
        let level = if level > 0.0 { level } else { 1e-6 };
        let release = if release > 0.0 { release } else { 1e-4 };
        Coefs {
            level,
            fast: self.one_pole_coef(release),
            slow: self.one_pole_coef(release * SLOW_RELEASE_RATIO),
            // How fast the sustain estimate itself moves. Complement of a
            // one-pole coefficient, so it is a per-sample blend weight.
            sustain_alpha: 1.0 - self.one_pole_coef(SUSTAIN_TIME_CONSTANT),
        }
    }

    fn next_i(&self, i: i32) -> i32 {
        if i + 1 >= self.deque_cap {
            0
        } else {
            i + 1
        }
    }

    fn prev_i(&self, i: i32) -> i32 {
        if i == 0 {
            self.deque_cap - 1
        } else {
            i - 1
        }
    }

    /// One sample of the gain path: sliding minimum, program-dependent release,
    /// then the smoothing cascade.
    fn next_gain(&mut self, mag: f32, k: &Coefs) -> f32 {
        let required = if mag > k.level { k.level / mag } else { 1.0 };

        // Sliding minimum over the look-ahead window, via a deque held
        // monotonically non-decreasing from head to tail.
        let window = self.lookahead as u32 + 1;
        loop {
            if self.count == 0 {
                break;
            }
            let at = self.prev_i(self.tail) as usize;
            if self.deque_vals()[at] < required {
                break;
            }
            self.tail = self.prev_i(self.tail);
            self.count -= 1;
        }
        let (tail, index) = (self.tail as usize, self.sample_index);
        self.deque_vals()[tail] = required;
        self.deque_idx()[tail] = index;
        self.tail = self.next_i(self.tail);
        self.count += 1;
        // Unsigned subtraction, so this stays correct across the wrap of
        // sample_index on a long-running session.
        loop {
            if self.count == 0 {
                break;
            }
            let head = self.head as usize;
            if index.wrapping_sub(self.deque_idx()[head]) < window {
                break;
            }
            self.head = self.next_i(self.head);
            self.count -= 1;
        }
        let head = self.head as usize;
        let min_gain = self.deque_vals()[head];

        // How much reduction is being asked for, averaged over
        // SUSTAIN_TIME_CONSTANT. This is what decides the release rate: brief
        // reduction leaves it near zero, sustained reduction drives it up.
        self.sustain += (1.0 - min_gain - self.sustain) * k.sustain_alpha;

        // Program-dependent release. Falls instantly, rises at a rate that
        // slows the longer reduction has persisted. Whichever limb is taken,
        // env stays <= min_gain, which is what the ceiling proof requires.
        if min_gain <= self.env {
            self.env = min_gain;
        } else {
            let sustained = (self.sustain * (1.0 / FULL_SUSTAIN_DEPTH)).clamp(0.0, 1.0);
            let coef = k.fast + (k.slow - k.fast) * sustained;
            self.env = min_gain + (self.env - min_gain) * coef;
        }

        // Three cascaded box filters: a quadratic B-spline kernel of length
        // 3*box_len-2, computed in O(1) from running sums. The sums are f64 so
        // that repeated add/subtract over a long session cannot drift the gain
        // above its input.
        let mut v = self.env;
        let box_len = self.box_len;
        for b in 0..3 {
            let pos = self.box_pos[b] as usize;
            let old = self.box_buf(b)[pos];
            self.box_sum[b] += v as f64 - old as f64;
            self.box_buf(b)[pos] = v;
            self.box_pos[b] += 1;
            if self.box_pos[b] >= box_len {
                self.box_pos[b] = 0;
            }
            v = (self.box_sum[b] / box_len as f64) as f32;
        }
        self.gain = v;
        self.sample_index = self.sample_index.wrapping_add(1);
        self.gain
    }

    fn advance(&mut self) {
        self.delay_pos += 1;
        if self.delay_pos >= self.lookahead {
            self.delay_pos = 0;
        }
    }

    /// One sample in, one sample out, plus the gain that was applied.
    ///
    /// Taking a sample rather than a slice is what lets the caller work in
    /// place: it has already read the input before this writes the output.
    pub fn step_mono(&mut self, k: &Coefs, x: f32) -> (f32, f32) {
        let g = self.next_gain(x.abs(), k);
        let pos = self.delay_pos as usize;
        let delayed = self.delay(0)[pos];
        self.delay(0)[pos] = x;
        self.advance();
        (g * delayed, g)
    }

    /// The stereo pair, with one gain derived from whichever channel needs it
    /// most. Returns `(left, right, gain)`.
    pub fn step_stereo(&mut self, k: &Coefs, l: f32, r: f32) -> (f32, f32, f32) {
        let g = self.next_gain(l.abs().max(r.abs()), k);
        let pos = self.delay_pos as usize;
        let dl = self.delay(0)[pos];
        let dr = self.delay(1)[pos];
        self.delay(0)[pos] = l;
        self.delay(1)[pos] = r;
        self.advance();
        (g * dl, g * dr, g)
    }

    /// Mono, over slices that do not overlap.
    ///
    /// `gain_out`, when given, receives the applied gain per sample, so a meter
    /// can read measured gain reduction rather than inferring it.
    pub fn process_mono(
        &mut self,
        input: &[f32],
        output: &mut [f32],
        level: f32,
        release: f32,
        mut gain_out: Option<&mut [f32]>,
    ) {
        let k = self.begin(level, release);
        let n = input.len().min(output.len());
        for i in 0..n {
            let (y, g) = self.step_mono(&k, input[i]);
            output[i] = y;
            if let Some(go) = gain_out.as_deref_mut() {
                if i < go.len() {
                    go[i] = g;
                }
            }
        }
    }

    /// Stereo with a linked detector, over slices that do not overlap.
    #[allow(clippy::too_many_arguments)]
    pub fn process_stereo(
        &mut self,
        in_l: &[f32],
        in_r: &[f32],
        out_l: &mut [f32],
        out_r: &mut [f32],
        level: f32,
        release: f32,
        mut gain_out: Option<&mut [f32]>,
    ) {
        let k = self.begin(level, release);
        let n = in_l.len().min(in_r.len()).min(out_l.len()).min(out_r.len());
        for i in 0..n {
            let (l, r, g) = self.step_stereo(&k, in_l[i], in_r[i]);
            out_l[i] = l;
            out_r[i] = r;
            if let Some(go) = gain_out.as_deref_mut() {
                if i < go.len() {
                    go[i] = g;
                }
            }
        }
    }
}

// ── C ABI ───────────────────────────────────────────────────────────────────
//
// The ugen is C++ and constructs on the audio thread, so it allocates from the
// server's real-time heap and hands the block over here.

#[no_mangle]
pub extern "C" fn sp_limiter_min_lookahead() -> c_int {
    MIN_LOOKAHEAD
}

#[no_mangle]
pub extern "C" fn sp_limiter_max_lookahead() -> c_int {
    MAX_LOOKAHEAD
}

#[no_mangle]
pub extern "C" fn sp_limiter_max_channels() -> c_int {
    MAX_CHANNELS
}

#[no_mangle]
pub extern "C" fn sp_limiter_clamp_lookahead(samples: c_int) -> c_int {
    clamp_lookahead(samples)
}

#[no_mangle]
pub extern "C" fn sp_limiter_clamp_channels(channels: c_int) -> c_int {
    clamp_channels(channels)
}

#[no_mangle]
pub extern "C" fn sp_limiter_lookahead_samples(seconds: f32, sample_rate: f64) -> c_int {
    lookahead_samples(seconds, sample_rate)
}

#[no_mangle]
pub extern "C" fn sp_limiter_box_length(d: c_int) -> c_int {
    box_length(d)
}

#[no_mangle]
pub extern "C" fn sp_limiter_kernel_length(d: c_int) -> c_int {
    kernel_length(d)
}

#[no_mangle]
pub extern "C" fn sp_limiter_deque_capacity(d: c_int) -> c_int {
    deque_capacity(d)
}

#[no_mangle]
pub extern "C" fn sp_limiter_memory_bytes(lookahead: c_int, channels: c_int) -> usize {
    memory_bytes(lookahead, channels)
}

/// Build a limiter inside `mem` and return the handle.
#[no_mangle]
pub unsafe extern "C" fn sp_limiter_configure(
    mem: *mut c_void,
    lookahead: c_int,
    channels: c_int,
    sample_rate: f64,
) -> *mut c_void {
    if mem.is_null() {
        return std::ptr::null_mut();
    }
    SpLimiter::place(mem as *mut u8, lookahead, channels, sample_rate) as *mut c_void
}

#[no_mangle]
pub unsafe extern "C" fn sp_limiter_reset(handle: *mut c_void) {
    if !handle.is_null() {
        (*(handle as *mut SpLimiter)).reset();
    }
}

#[no_mangle]
pub unsafe extern "C" fn sp_limiter_latency(handle: *const c_void) -> c_int {
    if handle.is_null() {
        return 0;
    }
    (*(handle as *const SpLimiter)).latency_samples()
}

#[no_mangle]
pub unsafe extern "C" fn sp_limiter_channels(handle: *const c_void) -> c_int {
    if handle.is_null() {
        return 0;
    }
    (*(handle as *const SpLimiter)).channels()
}

#[no_mangle]
pub unsafe extern "C" fn sp_limiter_current_gain(handle: *const c_void) -> f32 {
    if handle.is_null() {
        return 1.0;
    }
    (*(handle as *const SpLimiter)).current_gain()
}

#[no_mangle]
pub unsafe extern "C" fn sp_limiter_process_mono(
    handle: *mut c_void,
    input: *const f32,
    output: *mut f32,
    num_samples: c_int,
    level: f32,
    release: f32,
    gain_out: *mut f32,
) {
    if handle.is_null() || input.is_null() || output.is_null() || num_samples <= 0 {
        return;
    }
    let n = num_samples as usize;
    let core = &mut *(handle as *mut SpLimiter);
    // Sample at a time through raw pointers, because `input` and `output` may
    // be the same buffer — the ugen processes in place. Two overlapping slices
    // would be undefined however carefully the loop was written.
    let k = core.begin(level, release);
    for i in 0..n {
        let (y, g) = core.step_mono(&k, *input.add(i));
        *output.add(i) = y;
        if !gain_out.is_null() {
            *gain_out.add(i) = g;
        }
    }
}

#[allow(clippy::too_many_arguments)]
#[no_mangle]
pub unsafe extern "C" fn sp_limiter_process_stereo(
    handle: *mut c_void,
    in_l: *const f32,
    in_r: *const f32,
    out_l: *mut f32,
    out_r: *mut f32,
    num_samples: c_int,
    level: f32,
    release: f32,
    gain_out: *mut f32,
) {
    if handle.is_null()
        || in_l.is_null()
        || in_r.is_null()
        || out_l.is_null()
        || out_r.is_null()
        || num_samples <= 0
    {
        return;
    }
    let n = num_samples as usize;
    let core = &mut *(handle as *mut SpLimiter);
    // As above: the ugen may hand the same buffers in and out.
    let k = core.begin(level, release);
    for i in 0..n {
        let (yl, yr, g) = core.step_stereo(&k, *in_l.add(i), *in_r.add(i));
        *out_l.add(i) = yl;
        *out_r.add(i) = yr;
        if !gain_out.is_null() {
            *gain_out.add(i) = g;
        }
    }
}

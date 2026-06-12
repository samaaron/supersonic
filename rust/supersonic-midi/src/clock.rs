//! MIDI clock-IN tempo estimation.
//!
//! Pure: the host supplies pulse arrival timestamps; the estimator returns a
//! distilled BPM, identical on native and web. (Clock-OUT generation lives
//! engine-side in C++ MidiClockOut, timed off SuperClock.)

use std::collections::VecDeque;

/// Standard MIDI clock resolution: 24 pulses per quarter-note (beat).
pub const PPQN: u32 = 24;

/// Upper bound on the median window, so [`ClockEstimator::median`] can use a
/// fixed stack buffer instead of a per-pulse heap allocation. Far above any
/// sensible window (default 7); larger requests are clamped at construction.
const MAX_MEDIAN: usize = 64;

/// Tuning for [`ClockEstimator`].
#[derive(Clone, Copy, Debug)]
pub struct EstimatorParams {
    /// EMA smoothing factor for the BPM estimate (0..1). Higher = snappier.
    pub alpha: f64,
    /// Median window length (intervals) used for outlier rejection.
    pub window: usize,
    /// Reject an interval whose ratio to the window median falls outside
    /// `[1/ratio, ratio]` — catches dropouts (huge gap) and double-pulses
    /// (tiny gap) without blocking ordinary tempo variation.
    pub reject_ratio: f64,
    /// After this many consecutive rejects, force-accept and resync — so a
    /// genuine large tempo change can't wedge the filter.
    pub max_consecutive_rejects: u32,
    /// Maximum change in the *output* BPM per accepted pulse. Models the
    /// consumer-DAW practice of slew-limiting external sync so SuperClock's
    /// tempo can't lurch. `f64::INFINITY` disables it.
    pub max_step_bpm: f64,
}

impl Default for EstimatorParams {
    fn default() -> Self {
        Self {
            alpha: 0.15,
            window: 7,
            reject_ratio: 2.0,
            max_consecutive_rejects: 4,
            max_step_bpm: f64::INFINITY,
        }
    }
}

/// Estimates BPM from incoming MIDI clock pulse arrival timestamps
/// (microseconds). MIDI clock has no phase to lock — ticks are anonymous — so
/// this is a 1-D tempo smoother: a median/ratio outlier reject (for USB-MIDI
/// dropouts and tick-bunching) feeding an EMA, with an optional slew limit on
/// the output. Musical position is *not* derived here; see [`crate::sync`] for
/// Start/Continue/Stop/SongPosition handling.
#[derive(Debug)]
pub struct ClockEstimator {
    params: EstimatorParams,
    last_us: Option<u64>,
    intervals: VecDeque<f64>, // accepted inter-pulse intervals (µs)
    x: f64,                   // EMA of BPM
    out: f64,                 // slew-limited output BPM
    consecutive_rejects: u32,
    initialized: bool,
}

impl Default for ClockEstimator {
    fn default() -> Self {
        Self::with_params(EstimatorParams::default())
    }
}

impl ClockEstimator {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn with_params(mut params: EstimatorParams) -> Self {
        // Keep the window within the fixed median buffer (and ≥1).
        params.window = params.window.clamp(1, MAX_MEDIAN);
        Self {
            params,
            last_us: None,
            intervals: VecDeque::with_capacity(params.window),
            x: 0.0,
            out: 0.0,
            consecutive_rejects: 0,
            initialized: false,
        }
    }

    /// Forget history (call on transport start/stop or when sync is toggled).
    pub fn reset(&mut self) {
        self.last_us = None;
        self.intervals.clear();
        self.x = 0.0;
        self.out = 0.0;
        self.consecutive_rejects = 0;
        self.initialized = false;
    }

    /// Current best BPM estimate, if enough pulses have been seen.
    pub fn bpm(&self) -> Option<f64> {
        if self.initialized {
            Some(self.out)
        } else {
            None
        }
    }

    fn median(&self) -> f64 {
        // Copy into a stack buffer instead of allocating a Vec on every pulse —
        // this runs 24×/beat per clocked device, so the per-pulse heap alloc was
        // pure waste. The window is tiny (default 7); MAX_MEDIAN caps it safely.
        let n = self.intervals.len().min(MAX_MEDIAN);
        let mut buf = [0.0f64; MAX_MEDIAN];
        for (slot, v) in buf[..n].iter_mut().zip(self.intervals.iter()) {
            *slot = *v;
        }
        let s = &mut buf[..n];
        s.sort_by(|a, b| a.partial_cmp(b).unwrap());
        s[n / 2]
    }

    /// Feed one clock pulse arrival timestamp (µs). Returns the updated BPM
    /// estimate, or `None` until the first interval is measured.
    pub fn update(&mut self, ts_us: u64) -> Option<f64> {
        let prev = match self.last_us {
            Some(p) => p,
            None => {
                self.last_us = Some(ts_us);
                return None;
            }
        };
        self.last_us = Some(ts_us);

        let dt = ts_us.saturating_sub(prev) as f64;
        if dt <= 0.0 {
            return self.bpm();
        }

        // Absolute plausibility band first: outside 20..400 BPM the interval is
        // a delivery artefact (queue-flush burst / stall gap), not a tempo
        // observation — it must never enter the window or the reject counter,
        // or a tick burst force-resyncs the filter onto garbage.
        let measured = 60.0 / (dt / 1_000_000.0 * PPQN as f64);
        if !(20.0..=400.0).contains(&measured) {
            return self.bpm();
        }

        // Outlier rejection against the running median, with a forced resync
        // escape hatch so a real large tempo change can't wedge the filter.
        if self.intervals.len() >= 3 {
            let med = self.median();
            let r = self.params.reject_ratio;
            if dt < med / r || dt > med * r {
                self.consecutive_rejects += 1;
                if self.consecutive_rejects < self.params.max_consecutive_rejects {
                    return self.bpm();
                }
                self.intervals.clear(); // resync to the new tempo
            }
        }
        self.consecutive_rejects = 0;

        if self.intervals.len() == self.params.window {
            self.intervals.pop_front();
        }
        self.intervals.push_back(dt);

        if !self.initialized {
            self.x = measured;
            self.out = measured;
            self.initialized = true;
        } else {
            self.x += self.params.alpha * (measured - self.x);
            let step = (self.x - self.out).clamp(-self.params.max_step_bpm, self.params.max_step_bpm);
            self.out += step;
        }
        Some(self.out)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// µs between pulses at `bpm` (24 PPQN).
    fn interval_us(bpm: f64) -> f64 {
        60.0 / bpm / PPQN as f64 * 1_000_000.0
    }

    fn feed_steady(e: &mut ClockEstimator, t: &mut f64, bpm: f64, n: usize) -> Option<f64> {
        let dt = interval_us(bpm);
        let mut last = None;
        for _ in 0..n {
            last = e.update(*t as u64);
            *t += dt;
        }
        last
    }

    #[test]
    fn median_picks_middle_value() {
        // Guards the stack-buffer median refactor: behaviour must match a
        // plain sort-and-take-middle, regardless of insertion order.
        let mut e = ClockEstimator::new();
        for v in [30.0, 10.0, 20.0, 50.0, 40.0] {
            e.intervals.push_back(v);
        }
        assert_eq!(e.median(), 30.0);
    }

    #[test]
    fn estimator_converges_to_steady_tempo() {
        let mut e = ClockEstimator::new();
        let mut t = 0.0;
        let bpm = feed_steady(&mut e, &mut t, 120.0, 200).expect("estimate");
        assert!((bpm - 120.0).abs() < 1.0, "bpm={bpm}");
    }

    #[test]
    fn estimator_rejects_dropouts() {
        let mut e = ClockEstimator::new();
        let mut t = 0.0;
        feed_steady(&mut e, &mut t, 100.0, 100);
        let before = e.bpm().unwrap();
        // A huge gap (transport paused / cable glitch) implies ~0 BPM → ignored.
        e.update((t + 5_000_000.0) as u64);
        assert!((e.bpm().unwrap() - before).abs() < 1.0, "dropout moved estimate");
    }

    #[test]
    fn estimator_rejects_double_pulse() {
        let mut e = ClockEstimator::new();
        let mut t = 0.0;
        feed_steady(&mut e, &mut t, 120.0, 100);
        let before = e.bpm().unwrap();
        // A spurious tick 100µs after the last one (tick-bunching) → ignored.
        e.update((t - interval_us(120.0) + 100.0) as u64);
        assert!((e.bpm().unwrap() - before).abs() < 1.0, "double-pulse moved estimate");
    }

    #[test]
    fn estimator_survives_stall_and_burst() {
        // Field capture 2026-06-11 (MOTU rig): the tick stream periodically
        // stalls ~120ms then flushes the queued ticks ~300µs apart. The burst
        // must neither move the estimate nor force-resync the filter.
        let mut e = ClockEstimator::new();
        let mut t = 0.0;
        feed_steady(&mut e, &mut t, 111.0, 100);
        let before = e.bpm().unwrap();
        t += 122_000.0 - interval_us(111.0); // stall: next tick arrives 122ms late
        for _ in 0..6 {
            e.update(t as u64); // queued ticks flushed in a burst
            t += 300.0;
        }
        let after = feed_steady(&mut e, &mut t, 111.0, 24).unwrap();
        assert!((after - before).abs() < 1.0, "stall+burst moved estimate: {before} -> {after}");
    }

    #[test]
    fn estimator_tracks_tempo_change() {
        let mut e = ClockEstimator::new();
        let mut t = 0.0;
        feed_steady(&mut e, &mut t, 100.0, 100);
        let bpm = feed_steady(&mut e, &mut t, 140.0, 150).unwrap();
        assert!((bpm - 140.0).abs() < 1.5, "did not track change: bpm={bpm}");
    }

    #[test]
    fn estimator_slew_limits_output() {
        let mut e = ClockEstimator::with_params(EstimatorParams {
            max_step_bpm: 0.1,
            ..EstimatorParams::default()
        });
        let mut t = 0.0;
        feed_steady(&mut e, &mut t, 120.0, 50); // settle at 120
        // Now jump to 80 BPM; output must not move more than 0.1 BPM per pulse.
        let dt = interval_us(80.0);
        let mut prev = e.bpm().unwrap();
        for _ in 0..50 {
            let cur = e.update(t as u64).unwrap();
            assert!((cur - prev).abs() <= 0.1 + 1e-9, "step {} too large", cur - prev);
            prev = cur;
            t += dt;
        }
        assert!(prev > 80.0, "slew should still be settling toward 80");
    }
}

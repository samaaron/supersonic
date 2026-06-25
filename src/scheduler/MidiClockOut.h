/*
 * MidiClockOut.h — engine-side scheduler for `midi_clock_beat` bursts.
 *
 * onBeat() (one beat = 24 pulses over a duration) backs the manual
 * `midi_clock_beat`: it schedules port-targeted clock-tick one-shots into the
 * EngineScheduler. Command handlers run on the NRT gateway / MIDI dispatch
 * thread and only record intent under a mutex; generate() — called from the
 * render context, the ONLY place that enqueues (the EngineScheduler's slot pool
 * is RT-thread only) — drains it. All timing comes from SuperClock so the ticks
 * stay sample-locked to scsynth audio.
 *
 * This file owns *when*; the Rust subsystem owns *how* (the port send).
 */
#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

class SuperClock;

class MidiClockOut {
public:
    static constexpr int64_t kPulsesPerBeat    = 24;   // 24 PPQN
    static constexpr double  kLookaheadSeconds = 0.01; // schedule ticks ~10 ms ahead of due

    // ── Command thread (NRT gateway / MIDI dispatch) — records intent only,
    //    serialised by mLock. NOT the audio thread; only generate() enqueues. ──
    // Manual burst: one beat = 24 pulses spread over durationSeconds.
    void onBeat(SuperClock& clock, const std::string& port, double durationSeconds);

    // Drop all pending bursts (engine shutdown / test isolation). NRT only.
    void reset();

    // ── Render thread (RT) — the only enqueuer ───────────────────────────
    // Schedule every burst tick due within the look-ahead window.
    // try_lock — a block skipped on contention is caught up by the next call.
    void generate(double nowNtp);

private:
    // A one-shot OSC packet to emit at an absolute NTP time (a burst tick).
    struct Pending {
        double               atNtp;
        std::vector<uint8_t> osc;
    };

    std::mutex           mLock;
    std::vector<Pending> mPending;

    static std::vector<uint8_t> encodeClockTick(const std::string& port);
};

// Process-wide instance, shared by the render thread (generate) and the NRT
// command thread. Mirrors get_scheduler().
MidiClockOut& get_midi_clock_out();

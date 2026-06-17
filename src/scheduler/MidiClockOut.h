/*
 * MidiClockOut.h — engine-side MIDI clock-OUT coordinator.
 *
 * Generation is driven from the render context (native audio thread, wasm
 * worklet, embedded timer) via generate(), which is the ONLY place that
 * enqueues — the EngineScheduler's slot pool is single-threaded (RT thread only).
 * Command handlers run on the NRT gateway thread and only record intent under a
 * mutex; generate() drains it. All timing comes from SuperClock so pulses stay
 * sample-locked to scsynth audio.
 *
 * Each clock-out port runs its OWN continuous 24-PPQN train at its own tempo
 * SOURCE, so any number of ports can broadcast independent clocks:
 *   - Fixed    — a tempo pushed from a thread's `use_bpm <n>`; free-running grid,
 *                re-anchored on a tempo change so the pulse phase stays continuous.
 *   - Link     — tracks the Link/session timeline live (read RT-safe from the
 *                SuperClockState mirror); phase-locked to Link.
 *   - Timeline — tracks a midi:<port> follower timeline's smoothed tempo (a MIDI
 *                clock regenerator). The timeline registry is off-RT, so the
 *                tempo is snapshotted into the port by refreshTimelineFollowers()
 *                on the NRT tempo-change callback; tempo-match, free-running.
 *
 * onBeat() (one beat = 24 pulses over a duration) backs the manual
 * `midi_clock_beat`; it schedules port-targeted one-shots independent of the
 * continuous trains.
 *
 * This file owns *when*; the Rust subsystem owns *how* (the port send).
 */
#pragma once

#include "MidiClockGen.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

class SuperClock;

class MidiClockOut {
public:
    static constexpr double kQuantum = 4.0;           // phase wrap only; absolute beat used
    static constexpr double kLookaheadSeconds = 0.01; // schedule pulses ~10 ms ahead of due

    enum class Source { Fixed, Link, Timeline };

    // ── Command threads (NRT gateway, and the MIDI dispatch thread for a
    //    deferred /midi/clock/beat) — record intent only, serialised by mLock.
    //    Idempotent: each call reconciles the port to the requested state (no-op
    //    if unchanged, re-anchors on a tempo/source change, never restarts/
    //    glitches). NOT the audio thread — only generate() enqueues. ───────────
    void onClockOutTempo(SuperClock& clock, const std::string& port, double bpm);    // fixed tempo
    void onClockOutFollow(SuperClock& clock, const std::string& port,
                          const std::string& timeline);   // "link" | "midi:<handle>"
    void onClockOutOff(const std::string& port);

    // Re-snapshot the tempo of every Timeline-following port from its (off-RT)
    // timeline. Call on the NRT tempo-change callback. NRT only.
    void refreshTimelineFollowers(SuperClock& clock);

    // Manual burst: one beat = 24 pulses spread over durationSeconds.
    void onBeat(SuperClock& clock, const std::string& port, double durationSeconds);

    // Stop everything (engine shutdown / test isolation). NRT only.
    void reset();

    // ── Render thread (RT) — the only enqueuer ───────────────────────────
    // Schedule every clock pulse / burst tick due within the look-ahead window.
    // try_lock — a block skipped on contention is caught up by the next call.
    void generate(SuperClock& clock, double nowNtp);

private:
    struct Port {
        std::string          name;
        std::vector<uint8_t> tickOsc;     // pre-encoded /midi/clock/tick <name>
        MidiClockGenerator   gen;         // this port's own pulse train
        Source               source{Source::Fixed};
        std::string          timeline;        // for Source::Timeline ("midi:<handle>"), re-resolved on refresh
        double               bpm{120.0};       // effective tempo (Fixed / Timeline)
        double               originNtp{0.0};   // beat-origin in NTP (Fixed / Timeline)
    };
    // A one-shot OSC packet to emit at an absolute NTP time (burst ticks).
    struct Pending {
        double               atNtp;
        std::vector<uint8_t> osc;
    };

    std::mutex           mLock;
    std::vector<Port>    mPorts;
    std::vector<Pending> mPending;

    Port* findPort(const std::string& name);
    Port& portRef(const std::string& name);                  // find-or-add; caller holds mLock
    void  removePort(const std::string& name);               // caller holds mLock
    // Re-anchor a port's (bpm, origin) at `nowNtp` to `bpm`, preserving its
    // current beat so the pulse phase stays continuous; (re)start its train.
    void  setPortTempo(Port& p, double nowNtp, double bpm);
    // Reconcile a port to a snapshot source (Fixed/Timeline): restart on a source
    // switch, then re-anchor to bpm. (Link is read live and uses gen.start.)
    void  applyTempoSource(Port& p, Source src, const std::string& timeline,
                           double nowNtp, double bpm);

    static std::vector<uint8_t> encodeClockTick(const std::string& port);
};

// Process-wide instance, shared by the render thread (generate) and the NRT
// command thread. Mirrors get_scheduler().
MidiClockOut& get_midi_clock_out();

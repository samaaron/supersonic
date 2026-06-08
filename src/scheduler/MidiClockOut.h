/*
 * MidiClockOut.h — engine-side MIDI clock-OUT coordinator.
 *
 * Generation is driven from the
 * render context (native audio thread, wasm worklet, embedded timer) via
 * generate(), which is the ONLY place that enqueues — the EventScheduler's slot
 * pool is single-threaded (RT thread only). Command handlers (onStart/onStop/
 * onContinue/onBeat) run on the NRT gateway thread and only record intent under
 * a mutex; generate() drains it. All timing comes from SuperClock: continuous
 * 24-PPQN pulses land on beat boundaries (MidiClockGenerator) and are scheduled
 * at SuperClock.timeAtBeat(), so the clock stays sample-locked to scsynth audio
 * and tracks tempo / Link.
 *
 * Each emitted pulse/transport is an OSC packet ("/midi/clock/tick", the
 * "/midi/out/" verbs) scheduled into the EventScheduler; the OUT-ring consumer (native dispatch
 * thread / wasm main thread / embedded MIDI task) delivers it through the Rust
 * subsystem's port send. This file owns *when*; the Rust subsystem owns *how*.
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

    // ── Command thread (NRT gateway) — record intent only ────────────────
    void onStart(SuperClock& clock, const std::string& port);     // continuous clock on; sends Start (0xFA)
    void onContinue(SuperClock& clock, const std::string& port);  // continuous clock on; sends Continue (0xFB)
    void onStop(SuperClock& clock, const std::string& port);      // port off; sends Stop (0xFC); stops gen if last
    void onBeat(SuperClock& clock, const std::string& port,
                double durationSeconds);                          // one beat = 24 pulses over the duration

    // Stop the clock, drop all ports + pending one-shots (engine shutdown / test
    // isolation). NRT only.
    void reset();

    // ── Render thread (RT) — the only enqueuer ───────────────────────────
    // Schedule every clock pulse / transport / burst tick due within the
    // look-ahead window into the EventScheduler. SuperClock-timed. try_lock —
    // a block skipped on contention is caught up by the next call.
    void generate(SuperClock& clock, double nowNtp);

private:
    struct Port {
        std::string          name;
        std::vector<uint8_t> tickOsc;   // pre-encoded /midi/clock/tick <name>
    };
    // A one-shot OSC packet to emit at an absolute NTP time (transport + bursts).
    struct Pending {
        double               atNtp;
        std::vector<uint8_t> osc;
    };

    std::mutex          mLock;
    MidiClockGenerator  mGen;
    std::vector<Port>   mPorts;     // ports receiving the continuous clock
    std::vector<Pending> mPending;  // transport + burst one-shots awaiting enqueue

    Port* findPort(const std::string& name);
    void  addPort(const std::string& name);          // caller holds mLock
    void  removePort(const std::string& name);       // caller holds mLock

    static std::vector<uint8_t> encodeClockTick(const std::string& port);
    static std::vector<uint8_t> encodeTransport(const char* addr, const std::string& port);
};

// Process-wide instance, shared by the render thread (generate) and the NRT
// command thread (on*). Mirrors get_event_scheduler().
MidiClockOut& get_midi_clock_out();

/*
 * test_midi_clock_out.cpp — the engine-side MIDI clock-OUT coordinator end to
 * end: a command (onClockOutTempo / onClockOutFollow / onBeat) + repeated
 * render-thread generate() calls, timed off a SuperClock, schedule the right
 * number of /midi/clock/tick events into the EngineScheduler at the per-port
 * tempo. Uses the process-wide scheduler + coordinator (real wiring).
 */
#include <catch2/catch_test_macros.hpp>

#include "RingTestUtils.h"
#include "scheduler/MidiClockOut.h"
#include "scheduler/EngineScheduler.h"
#include "src/SuperClock.h"
#include "src/shared_memory.h"

#include <cstdint>
#include <vector>

namespace {

// Bind a local SuperClockState and run the Link timeline at a fixed tempo (origin 0).
struct FixedClock {
    SuperClockState st{};
    SuperClock      clock;
    explicit FixedClock(double bpm) {
        clock.bindStateToShm(&st);
        st.bpm.store(supersonic::doubleToBits(bpm), std::memory_order_relaxed);
        st.beat_origin_ntp.store(supersonic::doubleToBits(0.0), std::memory_order_relaxed);
        st.is_playing.store(1u, std::memory_order_relaxed);
    }
};

constexpr double kBlock = 128.0 / 48000.0;   // one scsynth control block (~2.67 ms)

// Drop anything already pending in the process-wide scheduler, so a run counts
// only the events it schedules.
void clearScheduler(EngineScheduler& sched) {
    ring_test::drainDue(sched, INT64_MAX);
}

// Drive generate() across `seconds` of render blocks from `base`, then return
// every event the run scheduled (all are due by INT64_MAX).
std::vector<ring_test::Fired> runFor(MidiClockOut& clk, FixedClock& fc, double base, double seconds) {
    for (double t = 0.0; t <= seconds + 1e-9; t += kBlock) clk.generate(fc.clock, base + t);
    return ring_test::drainDue(get_scheduler(), INT64_MAX);
}

} // namespace

TEST_CASE("MidiClockOut fixed tempo schedules ~24 pulses per beat", "[midi_clock]") {
    FixedClock fc(120.0);                        // unused for Fixed, but a valid clock
    EngineScheduler& sched = get_scheduler();
    MidiClockOut&   clk   = get_midi_clock_out();
    clk.reset();
    const double base = fc.clock.now();
    clearScheduler(sched);

    clk.onClockOutTempo(fc.clock, "clk", 120.0);  // 120 BPM → 24 pulses in 0.5 s
    auto fired = runFor(clk, fc, base, 0.5);

    const int ticks = ring_test::countByAddr(fired, "/midi/clock/tick");
    CHECK(ticks >= 23);
    CHECK(ticks <= 27);
    // Clock-only: no transport bytes are emitted by a clock-out.
    CHECK(ring_test::countByAddr(fired, "/midi/out/start") == 0);
}

TEST_CASE("MidiClockOut following :link tracks the Link tempo", "[midi_clock]") {
    FixedClock fc(120.0);                         // Link timeline at 120 BPM
    EngineScheduler& sched = get_scheduler();
    MidiClockOut&   clk   = get_midi_clock_out();
    clk.reset();
    const double base = fc.clock.now();
    clearScheduler(sched);

    clk.onClockOutFollow(fc.clock, "clk", "link");
    auto fired = runFor(clk, fc, base, 0.5);

    const int ticks = ring_test::countByAddr(fired, "/midi/clock/tick");
    CHECK(ticks >= 23);
    CHECK(ticks <= 27);
}

TEST_CASE("MidiClockOut runs independent per-port tempos at once", "[midi_clock]") {
    FixedClock fc(120.0);
    EngineScheduler& sched = get_scheduler();
    MidiClockOut&   clk   = get_midi_clock_out();
    clk.reset();
    const double base = fc.clock.now();
    clearScheduler(sched);

    clk.onClockOutTempo(fc.clock, "a", 120.0);    // 24 pulses / 0.5 s
    clk.onClockOutTempo(fc.clock, "b", 240.0);    // 48 pulses / 0.5 s
    auto fired = runFor(clk, fc, base, 0.5);

    // Both trains share the /midi/clock/tick address (port is an arg), so the
    // aggregate ≈ 24 + 48 = 72 confirms the second port runs at twice the rate.
    const int ticks = ring_test::countByAddr(fired, "/midi/clock/tick");
    CHECK(ticks >= 66);
    CHECK(ticks <= 80);
}

TEST_CASE("MidiClockOut off stops a port's clock", "[midi_clock]") {
    FixedClock fc(120.0);
    EngineScheduler& sched = get_scheduler();
    MidiClockOut&   clk   = get_midi_clock_out();
    clk.reset();
    const double base = fc.clock.now();

    clk.onClockOutTempo(fc.clock, "clk", 120.0);
    runFor(clk, fc, base, 0.25);
    clk.onClockOutOff("clk");
    clearScheduler(sched);                         // count only what comes AFTER off
    auto fired = runFor(clk, fc, base + 0.3, 0.5);

    CHECK(ring_test::countByAddr(fired, "/midi/clock/tick") == 0);
}

TEST_CASE("MidiClockOut emits nothing with no clock-out ports", "[midi_clock]") {
    FixedClock fc(120.0);
    EngineScheduler& sched = get_scheduler();
    MidiClockOut&   clk   = get_midi_clock_out();
    clk.reset();
    const double base = fc.clock.now();
    clearScheduler(sched);

    auto fired = runFor(clk, fc, base, 0.5);   // no port enabled → no pulses
    CHECK(ring_test::countByAddr(fired, "/midi/clock/tick") == 0);
}

TEST_CASE("MidiClockOut beat-burst schedules 24 ticks over the duration", "[midi_clock]") {
    FixedClock fc(120.0);
    EngineScheduler& sched = get_scheduler();
    MidiClockOut&   clk   = get_midi_clock_out();
    clk.reset();
    clearScheduler(sched);
    const double base = fc.clock.now();

    clk.onBeat(fc.clock, "p", 0.5);              // one beat's worth (24 ticks) over 0.5 s
    auto fired = runFor(clk, fc, base, 0.5);

    CHECK(ring_test::countByAddr(fired, "/midi/clock/tick") == 24);
}

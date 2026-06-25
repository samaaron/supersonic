/*
 * test_midi_clock_out.cpp — the engine-side midi_clock_beat burst end to end:
 * onBeat() records one beat's worth of pulses; repeated render-thread generate()
 * calls schedule them into the EngineScheduler spread over the requested
 * duration. Uses the process-wide scheduler + coordinator (real wiring).
 */
#include <catch2/catch_test_macros.hpp>

#include "RingTestUtils.h"
#include "scheduler/MidiClockOut.h"
#include "scheduler/EngineScheduler.h"
#include "src/SuperClock.h"
#include "src/shared_memory.h"

#include <vector>

namespace {

// A minimal valid SuperClock for onBeat()'s now() read.
struct FixedClock {
    SuperClockState st{};
    SuperClock      clock;
    FixedClock() {
        clock.bindStateToShm(&st);
        st.bpm.store(supersonic::doubleToBits(120.0), std::memory_order_relaxed);
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
std::vector<ring_test::Fired> runFor(MidiClockOut& clk, double base, double seconds) {
    for (double t = 0.0; t <= seconds + 1e-9; t += kBlock) clk.generate(base + t);
    return ring_test::drainDue(get_scheduler(), INT64_MAX);
}

} // namespace

TEST_CASE("MidiClockOut beat-burst schedules 24 ticks over the duration", "[midi_clock]") {
    FixedClock fc;
    EngineScheduler& sched = get_scheduler();
    MidiClockOut&   clk   = get_midi_clock_out();
    clk.reset();
    clearScheduler(sched);
    const double base = fc.clock.now();

    clk.onBeat(fc.clock, "p", 0.5);              // one beat's worth (24 ticks) over 0.5 s
    auto fired = runFor(clk, base, 0.5);

    CHECK(ring_test::countByAddr(fired, "/midi/clock/tick") == 24);
}

TEST_CASE("MidiClockOut emits nothing with no pending burst", "[midi_clock]") {
    FixedClock fc;
    EngineScheduler& sched = get_scheduler();
    MidiClockOut&   clk   = get_midi_clock_out();
    clk.reset();
    clearScheduler(sched);
    const double base = fc.clock.now();

    auto fired = runFor(clk, base, 0.5);   // nothing scheduled → no ticks
    CHECK(ring_test::countByAddr(fired, "/midi/clock/tick") == 0);
}

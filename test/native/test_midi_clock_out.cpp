/*
 * test_midi_clock_out.cpp — the engine-side MIDI clock-OUT coordinator end to
 * end: a command (onClockOutTempo / onClockOutFollow / onBeat) + repeated
 * render-thread generate() calls, timed off a SuperClock, schedule the right
 * number of /midi/clock/tick events into the EventScheduler at the per-port
 * tempo. Uses the process-wide scheduler + coordinator (real wiring).
 */
#include <catch2/catch_test_macros.hpp>

#include "scheduler/MidiClockOut.h"
#include "scheduler/EventScheduler.h"
#include "src/SuperClock.h"
#include "src/shared_memory.h"

#include <cstdint>
#include <cstring>

namespace {

// Walk the OUT ring from `fromHead` to `toHead`, counting framed
// [dest:u32][osc] messages whose OSC address equals `addr`.
int countByAddr(EventScheduler& es, int32_t fromHead, int32_t toHead, const char* addr) {
    const uint8_t* buf  = es.outBuffer();
    const uint32_t size = es.outSize();
    auto at = [&](uint32_t p) { return buf[p % size]; };
    int count = 0;
    uint32_t pos = static_cast<uint32_t>(fromHead);
    while (pos != static_cast<uint32_t>(toHead)) {
        Message hdr{};
        for (uint32_t i = 0; i < sizeof(Message); ++i) reinterpret_cast<uint8_t*>(&hdr)[i] = at(pos + i);
        if (hdr.magic != 0xDEADBEEFu || hdr.length < sizeof(Message)) break;
        const uint32_t dataPos = pos + static_cast<uint32_t>(sizeof(Message));
        uint32_t dest = 0;
        for (uint32_t i = 0; i < sizeof(dest); ++i) reinterpret_cast<uint8_t*>(&dest)[i] = at(dataPos + i);
        char got[40] = {0};
        for (uint32_t i = 0; i < sizeof(got) - 1; ++i) {
            const char ch = static_cast<char>(at(dataPos + sizeof(dest) + i));
            got[i] = ch;
            if (ch == '\0') break;
        }
        if (dest == static_cast<uint32_t>(EventScheduler::DEST_MIDI) && std::strcmp(got, addr) == 0)
            ++count;
        pos = (pos + hdr.length) % size;
    }
    return count;
}

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

// Reset the shared OUT ring to empty and return the write head to walk from.
int32_t resetRing(EventScheduler& sched) {
    sched.tick(INT64_MAX);
    sched.outTail()->store(sched.outHead()->load());
    return sched.outHead()->load();
}

// Drive generate() across `seconds` of render blocks from `base`, then flush.
void runFor(MidiClockOut& clk, FixedClock& fc, double base, double seconds) {
    for (double t = 0.0; t <= seconds + 1e-9; t += kBlock) clk.generate(fc.clock, base + t);
    get_event_scheduler().tick(INT64_MAX);
}

} // namespace

TEST_CASE("MidiClockOut fixed tempo schedules ~24 pulses per beat", "[midi_clock]") {
    FixedClock fc(120.0);                        // unused for Fixed, but a valid clock
    EventScheduler& sched = get_event_scheduler();
    MidiClockOut&   clk   = get_midi_clock_out();
    clk.reset();
    const double base = fc.clock.now();
    const int32_t startHead = resetRing(sched);

    clk.onClockOutTempo(fc.clock, "clk", 120.0);  // 120 BPM → 24 pulses in 0.5 s
    runFor(clk, fc, base, 0.5);

    const int ticks = countByAddr(sched, startHead, sched.outHead()->load(), "/midi/clock/tick");
    CHECK(ticks >= 23);
    CHECK(ticks <= 27);
    // Clock-only: no transport bytes are emitted by a clock-out.
    CHECK(countByAddr(sched, startHead, sched.outHead()->load(), "/midi/out/start") == 0);
}

TEST_CASE("MidiClockOut following :link tracks the Link tempo", "[midi_clock]") {
    FixedClock fc(120.0);                         // Link timeline at 120 BPM
    EventScheduler& sched = get_event_scheduler();
    MidiClockOut&   clk   = get_midi_clock_out();
    clk.reset();
    const double base = fc.clock.now();
    const int32_t startHead = resetRing(sched);

    clk.onClockOutFollow(fc.clock, "clk", "link");
    runFor(clk, fc, base, 0.5);

    const int ticks = countByAddr(sched, startHead, sched.outHead()->load(), "/midi/clock/tick");
    CHECK(ticks >= 23);
    CHECK(ticks <= 27);
}

TEST_CASE("MidiClockOut runs independent per-port tempos at once", "[midi_clock]") {
    FixedClock fc(120.0);
    EventScheduler& sched = get_event_scheduler();
    MidiClockOut&   clk   = get_midi_clock_out();
    clk.reset();
    const double base = fc.clock.now();
    const int32_t startHead = resetRing(sched);

    clk.onClockOutTempo(fc.clock, "a", 120.0);    // 24 pulses / 0.5 s
    clk.onClockOutTempo(fc.clock, "b", 240.0);    // 48 pulses / 0.5 s
    runFor(clk, fc, base, 0.5);

    // Both trains share the /midi/clock/tick address (port is an arg), so the
    // aggregate ≈ 24 + 48 = 72 confirms the second port runs at twice the rate.
    const int ticks = countByAddr(sched, startHead, sched.outHead()->load(), "/midi/clock/tick");
    CHECK(ticks >= 66);
    CHECK(ticks <= 80);
}

TEST_CASE("MidiClockOut off stops a port's clock", "[midi_clock]") {
    FixedClock fc(120.0);
    EventScheduler& sched = get_event_scheduler();
    MidiClockOut&   clk   = get_midi_clock_out();
    clk.reset();
    const double base = fc.clock.now();

    clk.onClockOutTempo(fc.clock, "clk", 120.0);
    runFor(clk, fc, base, 0.25);
    clk.onClockOutOff("clk");
    const int32_t startHead = resetRing(sched);    // count only what comes AFTER off
    runFor(clk, fc, base + 0.3, 0.5);

    CHECK(countByAddr(sched, startHead, sched.outHead()->load(), "/midi/clock/tick") == 0);
}

TEST_CASE("MidiClockOut emits nothing with no clock-out ports", "[midi_clock]") {
    FixedClock fc(120.0);
    EventScheduler& sched = get_event_scheduler();
    MidiClockOut&   clk   = get_midi_clock_out();
    clk.reset();
    const double base = fc.clock.now();
    const int32_t startHead = resetRing(sched);

    runFor(clk, fc, base, 0.5);   // no port enabled → no pulses
    CHECK(countByAddr(sched, startHead, sched.outHead()->load(), "/midi/clock/tick") == 0);
}

TEST_CASE("MidiClockOut beat-burst schedules 24 ticks over the duration", "[midi_clock]") {
    FixedClock fc(120.0);
    EventScheduler& sched = get_event_scheduler();
    MidiClockOut&   clk   = get_midi_clock_out();
    clk.reset();
    const int32_t startHead = resetRing(sched);
    const double base = fc.clock.now();

    clk.onBeat(fc.clock, "p", 0.5);              // one beat's worth (24 ticks) over 0.5 s
    runFor(clk, fc, base, 0.5);

    CHECK(countByAddr(sched, startHead, sched.outHead()->load(), "/midi/clock/tick") == 24);
}

/*
 * test_midi_clock_out.cpp — the engine-side MIDI clock-OUT coordinator end to
 * end: a command (onStart/onBeat) + repeated render-thread generate() calls,
 * timed off a SuperClock, schedule the right number of /midi/clock/tick events
 * into the EventScheduler at the SuperClock-derived rate. Uses the process-wide
 * scheduler + coordinator (real wiring).
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

// Bind a local SuperClockState and run the clock at a fixed tempo (origin 0).
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

} // namespace

TEST_CASE("MidiClockOut schedules ~24 SuperClock-timed pulses per beat", "[midi_clock]") {
    FixedClock fc(120.0);                        // 120 BPM → 0.5 s/beat
    EventScheduler& sched = get_event_scheduler();
    MidiClockOut&   clk   = get_midi_clock_out();
    clk.reset();   // isolate from clock state any other test may have left
    const double base = fc.clock.now();          // anchor to the clock's NTP base
    const int32_t startHead = resetRing(sched);

    clk.onStart(fc.clock, "clk");
    for (double t = 0.0; t <= 0.5 + 1e-9; t += kBlock) clk.generate(fc.clock, base + t);
    clk.onStop(fc.clock, "clk");
    clk.generate(fc.clock, base + 0.6);          // drain the stop one-shot
    sched.tick(INT64_MAX);                        // flush everything enqueued

    const int32_t endHead = sched.outHead()->load();
    const int ticks  = countByAddr(sched, startHead, endHead, "/midi/clock/tick");
    const int starts = countByAddr(sched, startHead, endHead, "/midi/out/start");
    const int stops  = countByAddr(sched, startHead, endHead, "/midi/out/stop");

    // One beat at 24 PPQN ≈ 24 pulses (a couple extra from the ~10 ms look-ahead).
    CHECK(ticks >= 23);
    CHECK(ticks <= 27);
    CHECK(starts == 1);   // transport bytes go out the same scheduled path
    CHECK(stops == 1);
}

TEST_CASE("MidiClockOut emits nothing while stopped", "[midi_clock]") {
    FixedClock fc(120.0);
    EventScheduler& sched = get_event_scheduler();
    MidiClockOut&   clk   = get_midi_clock_out();
    clk.reset();   // isolate from clock state any other test may have left
    const double base = fc.clock.now();
    const int32_t startHead = resetRing(sched);

    // No onStart → generator idle; generate() must schedule no pulses.
    for (double t = 0.0; t <= 0.5; t += kBlock) clk.generate(fc.clock, base + t);
    sched.tick(INT64_MAX);

    CHECK(countByAddr(sched, startHead, sched.outHead()->load(), "/midi/clock/tick") == 0);
}

TEST_CASE("MidiClockOut beat-burst schedules 24 ticks over the duration", "[midi_clock]") {
    FixedClock fc(120.0);
    EventScheduler& sched = get_event_scheduler();
    MidiClockOut&   clk   = get_midi_clock_out();
    clk.reset();   // isolate from clock state any other test may have left
    const int32_t startHead = resetRing(sched);
    const double base = fc.clock.now();

    clk.onBeat(fc.clock, "p", 0.5);              // one beat's worth (24 ticks) over 0.5 s
    for (double t = 0.0; t <= 0.5 + 1e-9; t += kBlock) clk.generate(fc.clock, base + t);
    sched.tick(INT64_MAX);

    CHECK(countByAddr(sched, startHead, sched.outHead()->load(), "/midi/clock/tick") == 24);
}

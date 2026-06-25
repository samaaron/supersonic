/*
 * test_midi.cpp — the /midi/ subsystem routed through the engine ingress
 * (sendOSC -> ingest -> MidiControl -> Rust ss_midi subsystem -> egress).
 *
 * No MIDI hardware required: these pin that /midi commands reach the subsystem
 * and that its replies/pushes come back through the egress, that subscription
 * pushes a ports snapshot, and that the out/clock dispatch paths are robust with
 * no devices open (port lists are typically empty on a headless CI box). Actual
 * device loopback is covered by the Rust virtual-port test (run manually).
 */
#include <catch2/catch_test_macros.hpp>

#include "EngineFixture.h"
#include "OscTestUtils.h"
#include "RingTestUtils.h"
#include "WallClock.h"
#include "scheduler/EngineScheduler.h"
#include "scheduler/MidiClockOut.h"
#include "src/SuperClock.h"

#ifdef SUPERSONIC_MIDI

using ring_test::countByAddr;

// /schedule expects an NTP timetag in the engine's clock domain. The engine
// compares OSC timetags as int64, into which a present-day NTP value overflows
// — consistently, so ordering holds, but a small literal like 1.0 is NOT "the
// distant past": it compares as the far future and the event never fires.
// Always timetag relative to wall-clock now (wallClockNTP from WallClock.h).

TEST_CASE("/midi/ports/list replies through the engine", "[midi]") {
    EngineFixture fx;
    fx.clearReplies();
    fx.send(osc_test::message("/midi/ports/list"));

    OscReply r;
    REQUIRE(fx.waitForReply("/midi/ports.reply", r));
    // First arg is the input-port count: ≥ 0 even with no devices attached.
    CHECK(r.parsed().argInt(0) >= 0);
}

TEST_CASE("/midi/notify/subscribe pushes a ports snapshot", "[midi]") {
    EngineFixture fx;
    fx.clearReplies();
    fx.send(osc_test::message("/midi/notify/subscribe"));

    OscReply r;
    CHECK(fx.waitForReply("/midi/ports.reply", r));
}

TEST_CASE("/midi/refresh broadcasts a ports update", "[midi]") {
    EngineFixture fx;
    // A subscriber is needed for the broadcast to have an in-process audience.
    fx.send(osc_test::message("/midi/notify/subscribe"));
    fx.clearReplies();
    fx.send(osc_test::message("/midi/refresh"));

    OscReply r;
    CHECK(fx.waitForReply("/midi/ports", r));
}

TEST_CASE("/midi out + clock dispatch is robust with no devices open", "[midi]") {
    EngineFixture fx;

    // None of these have an open destination, so they are no-ops — but must not
    // crash the subsystem or the engine.
    osc_test::Builder noteOn;
    noteOn.begin("/midi/out/note_on")
        << "*" << static_cast<osc::int32>(1) << static_cast<osc::int32>(60)
        << static_cast<osc::int32>(100);
    fx.send(noteOn.end());

    osc_test::Builder beat;
    beat.begin("/midi/clock/beat") << "out" << 500.0f;
    fx.send(beat.end());

    osc_test::Builder sync;
    sync.begin("/midi/clock/sync") << "in" << static_cast<osc::int32>(1);
    fx.send(sync.end());

    // The engine is still alive and serving /midi afterwards.
    fx.clearReplies();
    fx.send(osc_test::message("/midi/ports/list"));
    OscReply r;
    CHECK(fx.waitForReply("/midi/ports.reply", r));
}

TEST_CASE("/midi/at schedules a wrapped event without crashing", "[midi]") {
    EngineFixture fx;

    // Inner event the scheduler will dispatch when due.
    osc_test::Builder inner;
    inner.begin("/midi/out/note_on")
        << "*" << static_cast<osc::int32>(1) << static_cast<osc::int32>(60)
        << static_cast<osc::int32>(100);
    osc_test::Packet innerPkt = inner.end();

    // Wrap as /schedule <d: ntpSeconds (just past → due immediately)> <b: inner OSC>.
    // The inner /midi/out is self-routing: on fire it re-ingests through the same
    // dispatch an immediate /midi/out hits.
    osc_test::Builder at;
    at.begin("/schedule")
        << (wallClockNTP() - 1.0)
        << osc::Blob(innerPkt.data.data(),
                     static_cast<osc::osc_bundle_element_size_t>(innerPkt.size()));
    fx.send(at.end());

    // Let the audio thread drain the scheduler and the dispatch thread run.
    fx.waitForBlocks(5);

    // Engine is still alive and serving /midi.
    fx.clearReplies();
    fx.send(osc_test::message("/midi/ports/list"));
    OscReply r;
    CHECK(fx.waitForReply("/midi/ports.reply", r));
}

TEST_CASE("/schedule-wrapped /midi/clock/beat reaches the engine clock-out", "[midi][midi_clock]") {
    // midi_clock_beat's real wire form: Sonic Pi never sends the verb as an
    // immediate command — MidiAPI#send_one wraps it in a timetagged /schedule, so
    // it must survive the schedule→fire→dispatch round trip and reach MidiClockOut
    // via the unified dispatch (same path an immediate /midi/clock/beat would hit).
    // The precise per-beat tick count is covered by test_midi_clock_out.cpp, which
    // drives MidiClockOut directly; fired output now flows through the private
    // control ring (not the outbound ring), so it isn't tapped here — this asserts
    // the integration path is live and crash-free.
    get_midi_clock_out().reset();
    EngineFixture fx;

    osc_test::Builder inner;
    inner.begin("/midi/clock/beat") << "clk" << 100.0f;   // 24 ticks over 100 ms
    osc_test::Packet innerPkt = inner.end();

    osc_test::Builder at;
    at.begin("/schedule")
        << (wallClockNTP() - 1.0)                               // just past → due immediately
        << osc::Blob(innerPkt.data.data(),
                     static_cast<osc::osc_bundle_element_size_t>(innerPkt.size()));
    fx.send(at.end());

    fx.waitForBlocks(10);   // beat fires → dispatch → MidiClockOut generates pulses

    // Engine is still alive and serving /midi after the deferred clock-out path ran.
    fx.clearReplies();
    fx.send(osc_test::message("/midi/ports/list"));
    OscReply r;
    CHECK(fx.waitForReply("/midi/ports.reply", r));
}

TEST_CASE("Engine init clears leftover MIDI clock bursts", "[midi][midi_clock]") {
    // MidiClockOut is a process-wide singleton shared across the whole test
    // binary. Tests that drive it directly (test_midi_clock_out.cpp) reset only
    // at their start, so they can leave a pending beat-burst queued. A fresh
    // engine must start with that queue cleared, or the stale ticks leak into
    // the shared OUT ring under a later fixture — making exact tick-count
    // assertions flaky on a slow box.
    SuperClock ghost;
    get_midi_clock_out().onBeat(ghost, "ghost", 1.0);   // pollute: a queued burst

    EngineFixture fx;                  // init() must reset the singleton
    fx.stopHeadlessDriver();           // test thread becomes the sole scheduler writer

    // Drive generation deterministically 10 s past the burst's origin: a
    // surviving burst schedules its ticks; a cleared queue emits none.
    EngineScheduler& es = get_scheduler();
    ring_test::drainDue(es, INT64_MAX);            // clear anything already pending
    get_midi_clock_out().generate(ghost.now() + 10.0);
    auto fired = ring_test::drainDue(es, INT64_MAX);

    CHECK(countByAddr(fired, "/midi/clock/tick") == 0);
}

#endif // SUPERSONIC_MIDI

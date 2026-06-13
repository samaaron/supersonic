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
#include "scheduler/EventScheduler.h"
#include "scheduler/MidiClockOut.h"
#include "src/SuperClock.h"

#ifdef SUPERSONIC_MIDI

using ring_test::countOutRingByAddr;

// /midi/at expects an NTP timetag in the engine's clock domain. The engine
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

    fx.send(osc_test::message("/midi/clock/start", "out"));

    osc_test::Builder beat;
    beat.begin("/midi/clock/beat") << "out" << 500.0f;
    fx.send(beat.end());

    fx.send(osc_test::message("/midi/clock/stop", "out"));

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

    // Wrap as /midi/at <d: ntpSeconds (just past → due immediately)> <b: inner OSC>.
    osc_test::Builder at;
    at.begin("/midi/at")
        << (wallClockNTP() - 1.0)
        << osc::Blob(innerPkt.data.data(),
                     static_cast<osc::osc_bundle_element_size_t>(innerPkt.size()));
    fx.send(at.end());

    // Let the audio thread tick the scheduler and the dispatch thread run.
    fx.waitForBlocks(5);

    // Engine is still alive and serving /midi.
    fx.clearReplies();
    fx.send(osc_test::message("/midi/ports/list"));
    OscReply r;
    CHECK(fx.waitForReply("/midi/ports.reply", r));
}

TEST_CASE("/midi/at-wrapped /midi/clock/beat drives the engine clock-out", "[midi][midi_clock]") {
    // midi_clock_beat's real wire form: Sonic Pi never sends the verb as an
    // immediate command — MidiAPI#send_one wraps it in a timetagged /midi/at,
    // so it must reach MidiClockOut via the deferred-dispatch path too.
    EngineFixture fx;
    EventScheduler& es = get_event_scheduler();
    const int32_t startHead = es.outHead()->load();

    osc_test::Builder inner;
    inner.begin("/midi/clock/beat") << "clk" << 100.0f;   // 24 ticks over 100 ms
    osc_test::Packet innerPkt = inner.end();

    osc_test::Builder at;
    at.begin("/midi/at")
        << (wallClockNTP() - 1.0)                               // just past → due immediately
        << osc::Blob(innerPkt.data.data(),
                     static_cast<osc::osc_bundle_element_size_t>(innerPkt.size()));
    fx.send(at.end());

    // Burst ticks enter the OUT ring as generate() reaches them on the audio
    // thread — poll until the full beat's worth has been enqueued.
    const bool all24 = fx.pollUntil([&] {
        return countOutRingByAddr(es, startHead, es.outHead()->load(), "/midi/clock/tick") >= 24;
    }, 3000);
    CHECK(all24);
    CHECK(countOutRingByAddr(es, startHead, es.outHead()->load(), "/midi/clock/tick") == 24);
}

TEST_CASE("Engine init clears leftover MIDI clock-out ports", "[midi][midi_clock]") {
    // MidiClockOut is a process-wide singleton shared across the whole test
    // binary. Tests that drive it directly (test_midi_clock_out.cpp) reset only
    // at their start, so they can leave a continuous port running. A fresh
    // engine must start with no clock-out ports, or that stale port floods the
    // shared OUT ring under any later fixture — making exact tick-count
    // assertions flaky on a slow box.
    SuperClock ghost;
    get_midi_clock_out().onClockOutTempo(ghost, "ghost", 120.0);   // pollute

    EngineFixture fx;                  // init() must reset the singleton
    fx.stopHeadlessDriver();           // test thread becomes the sole scheduler writer

    // Drive generation deterministically 10 s past the ghost's origin: a
    // surviving port floods the ring with ticks; a cleared one emits none.
    EventScheduler& es = get_event_scheduler();
    es.tick(INT64_MAX);
    es.outTail()->store(es.outHead()->load());
    const int32_t startHead = es.outHead()->load();
    get_midi_clock_out().generate(ghost, ghost.now() + 10.0);
    es.tick(INT64_MAX);

    CHECK(countOutRingByAddr(es, startHead, es.outHead()->load(), "/midi/clock/tick") == 0);
}

#endif // SUPERSONIC_MIDI

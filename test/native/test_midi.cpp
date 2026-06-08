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

#ifdef SUPERSONIC_MIDI

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

    // Wrap as /midi/at <d: ntpSeconds (past → due immediately)> <b: inner OSC>.
    osc_test::Builder at;
    at.begin("/midi/at")
        << 1.0
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

#endif // SUPERSONIC_MIDI

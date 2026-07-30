/*
 * test_control_endpoints.cpp — characterisation coverage for the /supersonic/*
 * and /clock/* control endpoints, driven through the engine ingress
 * (sendOSC -> ingest -> handler -> reply).
 *
 * The suite previously exercised only one /supersonic/ command and gated the
 * /clock/ commands behind SUPERSONIC_ENABLE_LINK + a spawned peer. These cases
 * pin "each control command reaches its handler and emits its reply" with no
 * device or Link peer required, so the handlers can be relocated out of the
 * transport into an engine module without a silent regression slipping through.
 */
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "EngineFixture.h"
#include "OscTestUtils.h"

#include <cmath>

namespace {

// Send a no-arg command through the ingress and require its reply comes back.
void expectReply(EngineFixture& fx, const char* cmd, const char* replyAddr) {
    fx.clearReplies();
    fx.send(osc_test::message(cmd));
    OscReply r;
    INFO("command " << cmd << " should reply with " << replyAddr);
    CHECK(fx.waitForReply(replyAddr, r));
}

} // namespace

// These read SuperClock / Link session state (local seqlock state when Link is
// off), so they reply deterministically without an enabled Link session.
TEST_CASE("link control commands route through the ingress to their handlers",
          "[control][link]") {
    EngineFixture fx;
    expectReply(fx, "/clock/tempo/get", "/clock/tempo.reply");
    expectReply(fx, "/clock/transport/get", "/clock/transport.reply");
    expectReply(fx, "/clock/transport/time/get", "/clock/transport/time.reply");
    expectReply(fx, "/clock/visibility/get", "/clock/visibility.reply");
    expectReply(fx, "/clock/enabled/get", "/clock/enabled.reply");
    expectReply(fx, "/clock/start_stop_sync/get", "/clock/start_stop_sync.reply");
    expectReply(fx, "/clock/peers/count/get", "/clock/peers/count.reply");
    expectReply(fx, "/clock/peer_name/get", "/clock/peer_name.reply");
    expectReply(fx, "/clock/time/now/get", "/clock/time/now.reply");
}

// Config::defaultBpm seeds the session tempo at init, so the engine opens at the
// embedder's tempo (Sonic Pi boots at 60) instead of the built-in default (120).
// Seeded at init — NOT a post-boot /clock/tempo/set — so bpm and beat_origin are
// consistent from the first read (a set re-anchors beat_origin asynchronously,
// leaving a window where the two disagree).
TEST_CASE("clock: engine opens at Config::defaultBpm", "[control][clock]") {
    auto cfg = EngineFixture::defaultConfig();
    cfg.defaultBpm = 60.0;
    EngineFixture fx(cfg);

    fx.send(osc_test::message("/clock/tempo/get"));
    OscReply r;
    REQUIRE(fx.waitForReply("/clock/tempo.reply", r));
    CHECK(r.parsed().argDouble(0) == Catch::Approx(60.0).epsilon(1e-6));
}

// transport/time must share the NTP wall-clock domain of its sibling /clock time
// RPCs (time/now, time_at_beat), so a client can treat every /clock reply time
// uniformly. It streams a raw Link-clock (mach) time otherwise — off by the
// Link<->NTP offset. NTP-1900 micros are ~3.99e15; the raw mach clock is ~1e12
// (µs since boot), so a 1e15 threshold cleanly separates the domains.
TEST_CASE("clock: transport/time is in the NTP wall-clock domain like its siblings",
          "[control][clock]") {
    EngineFixture fx;
    fx.send(osc_test::message("/clock/transport/time/get"));
    OscReply r;
    REQUIRE(fx.waitForReply("/clock/transport/time.reply", r));
    CHECK(r.parsed().argInt64(0) > 1'000'000'000'000'000LL);
}

// No override → the built-in 120 default is unchanged.
TEST_CASE("clock: default tempo is 120 when Config::defaultBpm is unset",
          "[control][clock]") {
    EngineFixture fx;  // defaultConfig(): defaultBpm == kDefaultBpm (120)
    fx.send(osc_test::message("/clock/tempo/get"));
    OscReply r;
    REQUIRE(fx.waitForReply("/clock/tempo.reply", r));
    CHECK(r.parsed().argDouble(0) == Catch::Approx(120.0).epsilon(1e-6));
}

// The optional <timeline> segment routes /clock/<tl>/<verb> to a timeline:
// omitted ⇒ link (flat reply, back-compat); "link" echoes into the reply
// address; "midi" with no clocking port resolves to a 60-BPM placeholder and
// still replies; "timelines/get" enumerates. midi:<handle> content (both port
// names) is covered by the SuperClock unit tests.
TEST_CASE("clock timeline routing and enumerate",
          "[control][clock][timeline]") {
    EngineFixture fx;
    expectReply(fx, "/clock/tempo/get",       "/clock/tempo.reply");        // flat (link)
    expectReply(fx, "/clock/link/tempo/get",  "/clock/link/tempo.reply");   // explicit link
    expectReply(fx, "/clock/midi/tempo/get",  "/clock/midi/tempo.reply");   // placeholder
    expectReply(fx, "/clock/midi/transport/get", "/clock/midi/transport.reply");
    expectReply(fx, "/clock/timelines/get",   "/clock/timelines.reply");    // enumerate
}

// /supersonic/notify is device-free (it registers a notify target and replies),
// so it pins the /supersonic dispatch + reply path headless. The device/driver
// commands need a real device manager and are covered by the device-management
// suites; here we just guarantee the handler is reached and replies.
TEST_CASE("supersonic control commands route through the ingress to their handlers",
          "[control][supersonic]") {
    EngineFixture fx;
    expectReply(fx, "/supersonic/notify", "/supersonic/notify.reply");
}

// Capability discovery: compile-time facts as name/value pairs. This test
// build compiles Link + synth + MIDI, so all three report 1.
TEST_CASE("clock: capabilities/get reports the compiled backends",
          "[control][clock]") {
    EngineFixture fx;
    fx.send(osc_test::message("/clock/capabilities/get"));
    OscReply r;
    REQUIRE(fx.waitForReply("/clock/capabilities.reply", r));
    auto p = r.parsed();
    REQUIRE(p.argCount() >= 6);
    CHECK(p.argString(0) == "link");
    CHECK(p.argInt(1) == 1);
    CHECK(p.argString(2) == "link_audio");
    CHECK(p.argInt(3) == 1);
    CHECK(p.argString(4) == "midi");
    CHECK(p.argInt(5) == 1);
}

// The combined RPCs answer time+beat+phase in one round-trip and agree with
// each other: beat_phase_at_time re-queried at the timestamp beat_phase_now
// returned must land on the same beat (same beat origin, same tempo).
TEST_CASE("clock: combined beat_phase RPCs", "[control][clock]") {
    EngineFixture fx;

    osc_test::Builder b;
    b.begin("/clock/rpc/beat_phase_now") << 4.0f;
    fx.send(b.end());
    OscReply r;
    REQUIRE(fx.waitForReply("/clock/rpc/beat_phase_now.reply", r));
    auto p = r.parsed();
    const int64_t t    = p.argInt64(0);
    const double beat  = p.argDouble(1);
    const double phase = p.argDouble(2);
    CHECK(t > 1'000'000'000'000'000LL);   // NTP-1900 micros domain
    CHECK(phase >= 0.0);
    CHECK(phase < 4.0);
    double expectPhase = std::fmod(beat, 4.0);
    if (expectPhase < 0.0) expectPhase += 4.0;
    CHECK(phase == Catch::Approx(expectPhase).margin(1e-6));

    osc_test::Builder b2;
    b2.begin("/clock/rpc/beat_phase_at_time")
        << static_cast<osc::int64>(t) << 4.0f;
    fx.send(b2.end());
    OscReply r2;
    REQUIRE(fx.waitForReply("/clock/rpc/beat_phase_at_time.reply", r2));
    CHECK(r2.parsed().argDouble(0) == Catch::Approx(beat).margin(0.01));
}

// rpc/time_at_beat is the only /clock verb that takes a BEAT as input, and a
// beat has to arrive exactly. OSC 1.0 guarantees only float32, whose ulp is
// 2**-8 beats once the session beat count passes 32768 — at 60bpm that is 3.9ms
// of wall time, so a float32 beat puts ~2ms of error into every beat->time
// conversion (i.e. every Spider sleep in clock bpm mode). Beats therefore
// arrive as int64 microbeats, which is Link's own representation
// (ableton/link/Beats.hpp) and matches how the sibling verbs carry time.
//
// Asserted as a difference between two nearby beats, so the check needs no
// knowledge of the session's beat origin: 0.001 beat at 60bpm is 1000us.
TEST_CASE("clock: rpc/time_at_beat carries a beat exactly as microbeats",
          "[control][clock]") {
    auto cfg = EngineFixture::defaultConfig();
    cfg.defaultBpm = 60.0;
    EngineFixture fx(cfg);

    // A beat magnitude where float32 is coarse: 40000 is in [2**15, 2**16).
    const int64_t beatA_ub = 40'000'000'000LL;   // 40000.000 beats
    const int64_t beatB_ub = 40'000'001'000LL;   // 40000.001 beats

    auto timeAtMicrobeats = [&](int64_t ub, const char* what) {
        fx.clearReplies();
        osc_test::Builder b;
        b.begin("/clock/rpc/time_at_beat")
            << static_cast<osc::int64>(ub) << 4.0f;
        fx.send(b.end());
        OscReply r;
        INFO(what);
        REQUIRE(fx.waitForReply("/clock/rpc/time_at_beat.reply", r));
        return r.parsed().argInt64(0);
    };

    const int64_t tA = timeAtMicrobeats(beatA_ub, "beat A");
    const int64_t tB = timeAtMicrobeats(beatB_ub, "beat B");

    CHECK(tA > 1'000'000'000'000'000LL);   // NTP-1900 micros domain
    // 0.001 beat at 60bpm == 1000us. A float32 beat would snap both requests
    // onto the 2**-8 grid and answer 0us or 3906us apart.
    CHECK(static_cast<double>(tB - tA) == Catch::Approx(1000.0).margin(20.0));
}

// The float32 form stays accepted: a Spider built before the microbeat change
// (or any third-party client using only the OSC 1.0 core types) must keep
// working rather than have its request silently dropped — an ignored request
// means no reply at all, so the client blocks until its RPC times out.
TEST_CASE("clock: rpc/time_at_beat still accepts a float32 beat",
          "[control][clock]") {
    auto cfg = EngineFixture::defaultConfig();
    cfg.defaultBpm = 60.0;
    EngineFixture fx(cfg);

    osc_test::Builder b;
    b.begin("/clock/rpc/time_at_beat") << 8.0f << 4.0f;
    fx.send(b.end());
    OscReply r;
    REQUIRE(fx.waitForReply("/clock/rpc/time_at_beat.reply", r));
    CHECK(r.parsed().argInt64(0) > 1'000'000'000'000'000LL);
}

// A /clock verb nothing owns must refuse explicitly (echoing the offending
// address) instead of vanishing, so clients can tell "unsupported" from
// "lost datagram".
TEST_CASE("clock: unknown verbs are refused explicitly", "[control][clock]") {
    EngineFixture fx;
    fx.send(osc_test::message("/clock/definitely/not/a/verb"));
    OscReply r;
    REQUIRE(fx.waitForReply("/clock/unsupported", r));
    CHECK(r.parsed().argString(0) == "/clock/definitely/not/a/verb");
}

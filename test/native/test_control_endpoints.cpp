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

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

// /supersonic/notify is device-free (it registers a notify target and replies),
// so it pins the /supersonic dispatch + reply path headless. The device/driver
// commands need a real device manager and are covered by the device-management
// suites; here we just guarantee the handler is reached and replies.
TEST_CASE("supersonic control commands route through the ingress to their handlers",
          "[control][supersonic]") {
    EngineFixture fx;
    expectReply(fx, "/supersonic/notify", "/supersonic/notify.reply");
}

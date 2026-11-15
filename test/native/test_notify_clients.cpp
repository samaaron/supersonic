/*
 * test_notify_clients.cpp — /notify with one client, and tokens.
 *
 * Clockwork has one client. A message may carry a token of the client's
 * choosing, and an answer to it carries the same token; a notification
 * answers no message and carries none.
 *
 * scsynth keeps a command's token on its reply address. /notify stores a reply
 * address as a registered user, and every /n_go, /n_end and /tr is sent to
 * each registered user. So the registration is keyed on the client alone:
 * with the token left in, a client that sent /notify twice with two tokens
 * would be two users and hear every notification twice, each marked as the
 * answer to a /notify.
 */
#include <catch2/catch_test_macros.hpp>

#include "EngineFixture.h"
#include "OscTestUtils.h"

namespace {

void sendWith(EngineFixture& fx, const osc_test::Packet& pkt, uint32_t token) {
    REQUIRE(fx.engine().ingest(pkt.ptr(), pkt.size(), token) == CLOCKWORK_OK);
}

size_t countOf(EngineFixture& fx, const char* address) {
    size_t n = 0;
    for (const auto& r : fx.allReplies()) if (r.address == address) ++n;
    return n;
}

} // namespace

TEST_CASE("notify: /notify is answered with its token, and a notification carries none",
          "[notify][token]") {
    EngineFixture fx;
    fx.clearReplies();

    sendWith(fx, osc_test::message("/notify", int32_t{1}), 5);
    OscReply done;
    REQUIRE(fx.waitForReply("/done", done));
    CHECK(done.parsed().argString(0) == "/notify");
    CHECK(done.token == 5);

    // A new group is announced to the registered client: /n_go answers no
    // message, whatever token the /g_new that caused it was sent with.
    sendWith(fx, osc_test::message("/g_new", int32_t{100}, int32_t{0}, int32_t{0}), 6);
    OscReply go;
    REQUIRE(fx.waitForReply("/n_go", go));
    CHECK(go.token == 0);
}

TEST_CASE("notify: asking again with another token is the same registration",
          "[notify][token]") {
    EngineFixture fx;
    fx.clearReplies();

    sendWith(fx, osc_test::message("/notify", int32_t{1}), 5);
    OscReply done;
    REQUIRE(fx.waitForReply("/done", done));
    sendWith(fx, osc_test::message("/notify", int32_t{1}), 7);
    REQUIRE(fx.waitForReply("/done", done));
    CHECK(done.token == 7);

    // One client, one registration: the group is announced once.
    fx.clearReplies();
    sendWith(fx, osc_test::message("/g_new", int32_t{100}, int32_t{0}, int32_t{0}), 0);
    OscReply go;
    REQUIRE(fx.waitForReply("/n_go", go));
    fx.waitForBlocks(16);
    CHECK(countOf(fx, "/n_go") <= 1);   // waitForReply took the one; no second arrived

    // And it is undone with any token.
    sendWith(fx, osc_test::message("/notify", int32_t{0}), 9);
    REQUIRE(fx.waitForReply("/done", done));
    CHECK(done.parsed().argString(0) == "/notify");
    CHECK(done.token == 9);
}

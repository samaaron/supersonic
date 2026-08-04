/*
 * test_notify_clients.cpp — /notify must be per-client.
 *
 * SuperSonic identifies a reply target by the origin token in
 * ReplyAddress::mReplyData; the network address scsynth compares upstream is a
 * zeroed placeholder shared by every client. While the comparison operators
 * ignored the token, hw->mUsers (a std::set<ReplyAddress>) could only ever hold
 * one entry — the first client to register — so every broadcast reply
 * (SendReply, /n_go, /n_end, /tr) went to that client alone, and a second
 * client's /notify was answered "already registered" without being added.
 *
 * Sonic Pi hits this whenever more than one component registers: the studio
 * blocks its cold-swap reinit on a SendReply from sonic-pi-server-info, and a
 * lost registration turns that into a five-second timeout and a dead mixer.
 */
#include <catch2/catch_test_macros.hpp>

#include <set>

#include "synth/common/SC_ReplyImpl.hpp"

namespace {
// Mirrors audio_processor.cpp's ring_reply(): everything zeroed except the
// origin token, which is what actually routes the reply.
ReplyAddress ringReply(uint32_t origin) {
    ReplyAddress r = {};
    r.mProtocol = kWeb;
    r.mReplyFunc = &null_reply_func;
    r.mReplyData = reinterpret_cast<void*>(static_cast<uintptr_t>(origin));
    return r;
}
} // namespace

TEST_CASE("NotifyClients: addresses differing only by token are distinct",
          "[notify]") {
    const ReplyAddress a = ringReply(1);
    const ReplyAddress b = ringReply(2);

    CHECK_FALSE(a == b);
    CHECK((a < b) != (b < a));   // strict weak ordering separates them
}

TEST_CASE("NotifyClients: the same token compares equal", "[notify]") {
    const ReplyAddress a = ringReply(7);
    const ReplyAddress b = ringReply(7);

    CHECK(a == b);
    CHECK_FALSE(a < b);
    CHECK_FALSE(b < a);
}

TEST_CASE("NotifyClients: a client set holds one entry per client", "[notify]") {
    std::set<ReplyAddress> users;
    users.insert(ringReply(1));
    users.insert(ringReply(2));
    users.insert(ringReply(3));

    REQUIRE(users.size() == 3);

    // Re-registering an existing client stays idempotent.
    users.insert(ringReply(2));
    CHECK(users.size() == 3);

    // Every registered client is found — this is the lookup NotifyCmd::Stage2
    // uses to decide "already registered", and the iteration NodeReplyMsg /
    // NodeEndMsg use to fan a reply out.
    CHECK(users.count(ringReply(1)) == 1);
    CHECK(users.count(ringReply(2)) == 1);
    CHECK(users.count(ringReply(3)) == 1);
}

TEST_CASE("NotifyClients: unsubscribing one client leaves the others",
          "[notify]") {
    std::set<ReplyAddress> users;
    users.insert(ringReply(1));
    users.insert(ringReply(2));

    users.erase(ringReply(1));

    CHECK(users.size() == 1);
    CHECK(users.count(ringReply(1)) == 0);
    CHECK(users.count(ringReply(2)) == 1);
}

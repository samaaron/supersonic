/*
 * test_reply_routing.cpp — the origin-token reply path.
 *
 * Replies/subscriptions are addressed by an origin token that is threaded from
 * the call ctx to each backend. Two halves are covered here:
 *   1. OriginTable — the (ip,port) ↔ stable token address book.
 *   2. OscEgress — that a per-call token is carried through to the transport for
 *      replies (dispatchEgress) and subscriptions.
 */
#include <catch2/catch_test_macros.hpp>

#include "native/OriginTable.h"
#include "native/OscEgress.h"
#include "native/IOscTransport.h"
#include "src/shared_memory.h"   // EGRESS_REPLY, EGRESS_SEND_TO_CALLER

#include <string>
#include <vector>

// ── OriginTable: client (ip,port) ↔ stable, non-zero token ──────────────────
TEST_CASE("OriginTable maps a client to a stable, non-zero token", "[origin]") {
    OriginTable t;
    const uint32_t a = t.intern("10.0.0.1", 4000);
    CHECK(a >= 1u);
    CHECK(t.intern("10.0.0.1", 4000) == a);   // same client → same token (stable)
    CHECK(t.intern("10.0.0.2", 4000) != a);   // different ip → different token
    CHECK(t.intern("10.0.0.1", 4001) != a);   // different port → different token
}

TEST_CASE("OriginTable resolves a token back to its client", "[origin]") {
    OriginTable t;
    const uint32_t tok = t.intern("192.168.1.5", 9000);
    std::string ip; int port = 0;
    REQUIRE(t.resolve(tok, ip, port));
    CHECK(ip == "192.168.1.5");
    CHECK(port == 9000);

    // token 0 (in-process caller) and unknown tokens don't resolve.
    CHECK_FALSE(t.resolve(0, ip, port));
    CHECK(port == 0);
    CHECK_FALSE(t.resolve(tok + 99, ip, port));
}

TEST_CASE("OriginTable evicts the least-recently-seen client when full", "[origin]") {
    OriginTable t;
    std::vector<uint32_t> toks;
    for (int i = 0; i < 1024; ++i)
        toks.push_back(t.intern("10.1." + std::to_string(i / 256) + "." +
                                std::to_string(i % 256), 5000));
    // Touch client 0 so it is no longer the LRU.
    REQUIRE(t.intern("10.1.0.0", 5000) == toks[0]);
    // A new distinct client overflows the table → evicts the current LRU
    // (client 1), never the just-touched client 0.
    const uint32_t extra = t.intern("10.9.9.9", 5000);
    std::string ip; int port = 0;
    CHECK(t.resolve(extra, ip, port));          // newcomer present
    CHECK(t.resolve(toks[0], ip, port));        // touched client survived
    CHECK_FALSE(t.resolve(toks[1], ip, port));  // LRU was evicted
}

// ── OscEgress: a per-call token reaches the transport ───────────────────────
namespace {
struct MockTransport : IOscTransport {
    uint32_t lastSendToken   = 0;
    bool     lastNetworkOnly = false;
    int      sendCount       = 0;
    uint32_t lastSubToken    = 0;

    bool send(uint32_t token, const uint8_t*, uint32_t, bool networkOnly) override {
        lastSendToken = token; lastNetworkOnly = networkOnly; ++sendCount; return true;
    }
    void broadcastNotify(const uint8_t*, uint32_t) override {}
    void broadcastLink(const uint8_t*, uint32_t) override {}
    bool hasNotifySubscribers() const override { return false; }
    bool subscribeNotify(uint32_t t) override { lastSubToken = t; return true; }
    void subscribeNotifyPort(int) override {}
    void unsubscribeNotify(uint32_t t) override { lastSubToken = t; }
    void clearNotify() override {}
    bool subscribeLink(uint32_t t) override { lastSubToken = t; return true; }
    void unsubscribeLink(uint32_t t) override { lastSubToken = t; }
    void broadcastMidi(const uint8_t*, uint32_t) override {}
    bool subscribeMidi(uint32_t t) override { lastSubToken = t; return true; }
    void unsubscribeMidi(uint32_t t) override { lastSubToken = t; }
    void broadcastGamepad(const uint8_t*, uint32_t) override {}
    bool subscribeGamepad(uint32_t t) override { lastSubToken = t; return true; }
    void unsubscribeGamepad(uint32_t t) override { lastSubToken = t; }
    void broadcastOsc(const uint8_t*, uint32_t) override {}
    bool subscribeOsc(uint32_t t) override { lastSubToken = t; return true; }
    void unsubscribeOsc(uint32_t t) override { lastSubToken = t; }
};
}  // namespace

TEST_CASE("OscEgress routes a subscription to the per-call token", "[egress]") {
    MockTransport mock;
    OscEgress eg;
    eg.init(&mock, nullptr);

    CHECK(eg.subscribeCaller(0x1111));
    CHECK(mock.lastSubToken == 0x1111u);
    CHECK(eg.subscribeCallerToMidiNotify(0x2222));
    CHECK(mock.lastSubToken == 0x2222u);
    CHECK(eg.subscribeCallerToOscNotify(0x3333));
    CHECK(mock.lastSubToken == 0x3333u);
    CHECK(eg.subscribeCallerToLinkNotify(0x4444));
    CHECK(mock.lastSubToken == 0x4444u);
}

TEST_CASE("OscEgress delivers a reply to its origin token", "[egress]") {
    MockTransport mock;
    OscEgress eg;
    eg.init(&mock, nullptr);

    const uint8_t osc[] = {'/', 'x', 0, 0};
    // REPLY → send to the origin, in-process observers included.
    eg.dispatchEgress(0xABCD, EGRESS_REPLY, osc, sizeof(osc));
    CHECK(mock.lastSendToken == 0xABCDu);
    CHECK(mock.lastNetworkOnly == false);

    // SEND_TO_CALLER → same routing, network peers only.
    eg.dispatchEgress(0x7777, EGRESS_SEND_TO_CALLER, osc, sizeof(osc));
    CHECK(mock.lastSendToken == 0x7777u);
    CHECK(mock.lastNetworkOnly == true);

    // Two distinct origins get distinct replies — no shared/stale origin state.
    CHECK(mock.sendCount == 2);
}

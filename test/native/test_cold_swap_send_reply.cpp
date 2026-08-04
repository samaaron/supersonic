/*
 * test_cold_swap_send_reply.cpp — SendReply UGen delivery across a cold swap.
 *
 * Sonic Pi's Studio re-reads the World's bus geometry after every cold swap
 * (studio.rb cold_swap_reinit! Phase 3) by creating the sonic-pi-server-info
 * synth and waiting for its SendReply message. If that reply never lands the
 * reinit aborts with a promise timeout, the mixer group is never rebuilt, and
 * the app is left with no audio until it is relaunched.
 *
 * These tests replay Spider's exact post-swap sequence: re-register /notify,
 * reload the synthdefs into the fresh World, then query. The pre-swap query is
 * the control — it isolates a failure to the rebuild rather than the harness.
 */
#include "EngineFixture.h"
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>

#ifndef SUPERSONIC_TEST_SYNTHDEFS_DIR
#define SUPERSONIC_TEST_SYNTHDEFS_DIR ""
#endif

static bool loadTestSynthDef(EngineFixture& fx, const std::string& name) {
    std::string path = std::string(SUPERSONIC_TEST_SYNTHDEFS_DIR) + "/" + name + ".scsyndef";
    std::filesystem::path fsPath(path);
    if (!std::filesystem::exists(fsPath)) return false;

    std::ifstream f(fsPath, std::ios::binary);
    if (!f) return false;

    std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)),
                               std::istreambuf_iterator<char>());
    if (data.empty()) return false;

    auto pkt = osc_test::messageWithBlob("/d_recv", data.data(), data.size());
    return fx.sendAndExpectDone(pkt);
}

// Spider's query shape: addAction head, target 0 (root group). Spider uses node
// id 1, which is free for it because clear_scsynth! frees the default group
// first; the fixture creates that group at boot, so ids here start above it.
static void queryServerInfo(EngineFixture& fx, int32_t nodeId) {
    osc_test::Builder b;
    auto& s = b.begin("/s_new");
    s << "sonic-pi-server-info" << nodeId << (int32_t)0 << (int32_t)0;
    fx.send(b.end());
}

TEST_CASE("ColdSwapSendReply: server-info reply arrives after a cold swap",
          "[ColdSwap][send_reply]") {
    EngineFixture fx;
    OscReply reply;

    REQUIRE(loadTestSynthDef(fx, "sonic-pi-server-info"));
    fx.send(osc_test::message("/notify", 1));
    REQUIRE(fx.waitForReply("/done", reply));
    fx.clearReplies();

    // Control: the same query answers before the swap.
    queryServerInfo(fx, 1000);
    REQUIRE(fx.waitForReply("/sonic-pi/server-info", reply));
    fx.send(osc_test::message("/n_free", (int32_t)1000));
    fx.clearReplies();

    auto result = fx.engine().switchDevice("", 44100);
    REQUIRE(result.success);
    REQUIRE(result.type == SwapType::Cold);
    fx.clearReplies();

    // Phase 1.5 — the rebuilt World starts with an empty subscriber list.
    fx.send(osc_test::message("/notify", 1));
    REQUIRE(fx.waitForReply("/done", reply));
    fx.clearReplies();

    // Phase 2 — synthdefs live in the World, so the rebuild dropped them.
    REQUIRE(loadTestSynthDef(fx, "sonic-pi-server-info"));
    fx.clearReplies();

    // Phase 3 — the query that hangs in the running app.
    queryServerInfo(fx, 1000);
    INFO("Studio cold_swap_reinit! Phase 3 blocks on this reply for 5s");
    INFO(fx.debugMessagesDump());
    REQUIRE(fx.waitForReply("/sonic-pi/server-info", reply));
}

// The app's swap was a channel-count change (16 out -> 2 out), not a rate
// change: switchDevice took the cold path via forceCold, leaving the rate at
// 48k. Same rebuild, different entry — pinned separately so a fix that only
// covers the rate path can't pass.
TEST_CASE("ColdSwapSendReply: server-info reply arrives after a forced cold swap",
          "[ColdSwap][send_reply]") {
    EngineFixture fx;
    OscReply reply;

    REQUIRE(loadTestSynthDef(fx, "sonic-pi-server-info"));
    fx.send(osc_test::message("/notify", 1));
    REQUIRE(fx.waitForReply("/done", reply));
    fx.clearReplies();

    auto result = fx.engine().switchDevice("", 0, 0, /*forceCold=*/true);
    REQUIRE(result.success);
    REQUIRE(result.type == SwapType::Cold);
    fx.clearReplies();

    fx.send(osc_test::message("/notify", 1));
    REQUIRE(fx.waitForReply("/done", reply));
    fx.clearReplies();

    REQUIRE(loadTestSynthDef(fx, "sonic-pi-server-info"));
    fx.clearReplies();

    queryServerInfo(fx, 1000);
    INFO(fx.debugMessagesDump());
    REQUIRE(fx.waitForReply("/sonic-pi/server-info", reply));
}

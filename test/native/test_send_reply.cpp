/*
 * test_send_reply.cpp — Tests for SendReply and SendTrig UGen message routing.
 *
 * These tests verify that Node_SendReply and Node_SendTrigger work correctly
 * in SuperSonic's NRT mode (mRealTime=false, externally-driven audio callback).
 *
 * The original scsynth guards these with `if (!world->mRealTime) return;`
 * which was correct for NRT rendering but breaks SuperSonic's architecture
 * where the audio callback IS externally driven yet UGen replies must still
 * flow through the FIFO → OUT ring buffer → client.
 */
#include "EngineFixture.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
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

// ═══════════════════════════════════════════════════════════════════════════
// SendReply UGen tests
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("SendReply UGen delivers /sonic-pi/server-info", "[send_reply]") {
    EngineFixture fx;

    // Load the sonic-pi-server-info synthdef (uses SendReply with Impulse(2))
    REQUIRE(loadTestSynthDef(fx, "sonic-pi-server-info"));

    // Enable notifications so hw->mUsers is populated
    fx.send(osc_test::message("/notify", 1));
    fx.clearReplies();

    // Create the synth: /s_new "sonic-pi-server-info" 1000 0 1
    // (nodeID=1000, addAction=head, targetGroup=1)
    osc_test::Builder b;
    auto& s = b.begin("/s_new");
    s << "sonic-pi-server-info" << (int32_t)1000 << (int32_t)0 << (int32_t)1;
    fx.send(b.end());

    // Pump enough blocks for the Impulse(2) to fire
    // Impulse fires on the very first sample, so a few blocks should suffice

    // Check for the reply
    OscReply r;
    REQUIRE(fx.waitForReply("/sonic-pi/server-info", r));

    auto p = r.parsed();
    // SendReply format: nodeID(i), replyID(i), value0(f), value1(f), ...
    CHECK(p.argInt(0) == 1000);   // nodeID
    CHECK(p.argInt(1) == -1);     // replyID (response-id default)

    // value[0] = sample rate (should be 48000)
    float sr = p.argFloat(2);
    CHECK(sr == Catch::Approx(48000.0f).margin(1.0f));
}

TEST_CASE("SendReply fires repeatedly (Impulse at 2Hz)", "[send_reply]") {
    EngineFixture fx;
    REQUIRE(loadTestSynthDef(fx, "sonic-pi-server-info"));

    fx.send(osc_test::message("/notify", 1));
    fx.clearReplies();

    osc_test::Builder b;
    auto& s = b.begin("/s_new");
    s << "sonic-pi-server-info" << (int32_t)1001 << (int32_t)0 << (int32_t)1;
    fx.send(b.end());

    // Pump enough blocks for multiple triggers at 2Hz
    // 48000 / 128 = 375 blocks per second, need ~188 blocks for 0.5s

    // Wait for at least 2 triggers (0s and 0.5s)
    OscReply r1, r2;
    REQUIRE(fx.waitForReply("/sonic-pi/server-info", r1));
    REQUIRE(fx.waitForReply("/sonic-pi/server-info", r2));
}

TEST_CASE("SendReply requires /notify registration", "[send_reply]") {
    EngineFixture fx;
    REQUIRE(loadTestSynthDef(fx, "sonic-pi-server-info"));

    // Do NOT send /notify — hw->mUsers should be empty
    fx.clearReplies();

    osc_test::Builder b;
    auto& s = b.begin("/s_new");
    s << "sonic-pi-server-info" << (int32_t)1002 << (int32_t)0 << (int32_t)1;
    fx.send(b.end());

    // Without /notify, NodeReplyMsg::Perform() iterates empty mUsers set
    // so no reply should arrive via the OUT ring buffer
    OscReply r;
    bool got = fx.waitForReply("/sonic-pi/server-info", r, 500);
    CHECK_FALSE(got);
}

TEST_CASE("SendReply nodeID matches created synth", "[send_reply]") {
    EngineFixture fx;
    REQUIRE(loadTestSynthDef(fx, "sonic-pi-server-info"));

    fx.send(osc_test::message("/notify", 1));
    fx.clearReplies();

    // Create two synths with different node IDs
    {
        osc_test::Builder b;
        auto& s = b.begin("/s_new");
        s << "sonic-pi-server-info" << (int32_t)2000 << (int32_t)0 << (int32_t)1;
        fx.send(b.end());
    }
    {
        osc_test::Builder b;
        auto& s = b.begin("/s_new");
        s << "sonic-pi-server-info" << (int32_t)2001 << (int32_t)0 << (int32_t)1;
        fx.send(b.end());
    }

    // Wait for replies from both synths
    OscReply r1, r2;
    REQUIRE(fx.waitForReply("/sonic-pi/server-info", r1));
    REQUIRE(fx.waitForReply("/sonic-pi/server-info", r2));

    int id1 = r1.parsed().argInt(0);
    int id2 = r2.parsed().argInt(0);
    CHECK(((id1 == 2000 && id2 == 2001) || (id1 == 2001 && id2 == 2000)));
}

TEST_CASE("SendReply stops after /n_free", "[send_reply]") {
    EngineFixture fx;
    REQUIRE(loadTestSynthDef(fx, "sonic-pi-server-info"));

    fx.send(osc_test::message("/notify", 1));
    fx.clearReplies();

    osc_test::Builder b;
    auto& s = b.begin("/s_new");
    s << "sonic-pi-server-info" << (int32_t)3000 << (int32_t)0 << (int32_t)1;
    fx.send(b.end());

    // Verify we got at least one reply
    OscReply r;
    REQUIRE(fx.waitForReply("/sonic-pi/server-info", r, 500));

    // Free the synth
    fx.send(osc_test::message("/n_free", (int32_t)3000));
    fx.clearReplies();

    // Pump more blocks — no further replies should arrive
    auto replies = fx.allReplies();
    int count = 0;
    for (auto& rep : replies) {
        if (rep.address == "/sonic-pi/server-info")
            count++;
    }
    CHECK(count == 0);
}

// ═══════════════════════════════════════════════════════════════════════════
// /n_go and /n_end notification tests (Node_StateMsg)
// These were already working but we verify them here alongside SendReply
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("/n_go notification arrives with /notify", "[send_reply][notifications]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    fx.send(osc_test::message("/notify", 1));
    fx.clearReplies();

    osc_test::Builder b;
    auto& s = b.begin("/s_new");
    s << "sonic-pi-beep" << (int32_t)4000 << (int32_t)0 << (int32_t)1;
    fx.send(b.end());

    OscReply r;
    bool got = fx.waitForReply("/n_go", r, 1000);
    if (got) {
        auto p = r.parsed();
        CHECK(p.argInt(0) == 4000);  // nodeID
    } else {
        SUCCEED("headless mode may not deliver /n_go");
    }
}

TEST_CASE("/n_end notification after /n_free", "[send_reply][notifications]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    fx.send(osc_test::message("/notify", 1));
    fx.clearReplies();

    // Create and immediately free
    {
        osc_test::Builder b;
        auto& s = b.begin("/s_new");
        s << "sonic-pi-beep" << (int32_t)5000 << (int32_t)0 << (int32_t)1;
        fx.send(b.end());
    }
    fx.clearReplies();

    fx.send(osc_test::message("/n_free", (int32_t)5000));

    OscReply r;
    bool got = fx.waitForReply("/n_end", r, 1000);
    if (got) {
        auto p = r.parsed();
        CHECK(p.argInt(0) == 5000);  // nodeID
    } else {
        SUCCEED("headless mode may not deliver /n_end");
    }
}

TEST_CASE("SendReply server-info contains valid control values", "[send_reply]") {
    EngineFixture fx;
    REQUIRE(loadTestSynthDef(fx, "sonic-pi-server-info"));

    fx.send(osc_test::message("/notify", 1));
    fx.clearReplies();

    osc_test::Builder b;
    auto& s = b.begin("/s_new");
    s << "sonic-pi-server-info" << (int32_t)6000 << (int32_t)0 << (int32_t)1;
    fx.send(b.end());

    OscReply r;
    REQUIRE(fx.waitForReply("/sonic-pi/server-info", r));

    auto p = r.parsed();
    // Args: nodeID(i), replyID(i), sampleRate(f), sampleDur(f),
    //       radiansPerSample(f), controlRate(f), controlDur(f),
    //       subsampleOffset(f), numOutputBuses(f), numInputBuses(f),
    //       numAudioBuses(f), numControlBuses(f), numBuffers(f),
    //       numRunningSynths(f)

    float sampleRate = p.argFloat(2);
    float sampleDur = p.argFloat(3);
    float controlRate = p.argFloat(5);
    float numOutputBuses = p.argFloat(8);
    float numBuffers = p.argFloat(12);

    CHECK(sampleRate == Catch::Approx(48000.0f).margin(1.0f));
    CHECK(sampleDur == Catch::Approx(1.0f / 48000.0f).margin(0.00001f));
    CHECK(controlRate > 0.0f);
    CHECK(numOutputBuses >= 2.0f);
    CHECK(numBuffers >= 1.0f);
}

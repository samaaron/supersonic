/*
 * test_completion_message.cpp — Tests for completion messages embedded in
 *                                /d_recv, /b_free, /b_zero
 */
#include "EngineFixture.h"
#include <fstream>
#include <filesystem>

// Helper: build a /d_recv message with a synthdef blob AND a completion message
static osc_test::Packet dRecvWithCompletion(const uint8_t* synthdefData, uint32_t synthdefSize,
                                             const osc_test::Packet& completion) {
    // /d_recv takes: blob(synthdef), [blob(completion_msg)]
    // We need to build this with the Builder since we have two blobs
    osc_test::Builder b;
    auto& s = b.begin("/d_recv");
    s << osc::Blob(synthdefData, static_cast<osc::osc_bundle_element_size_t>(synthdefSize))
      << osc::Blob(completion.ptr(), static_cast<osc::osc_bundle_element_size_t>(completion.size()));
    return b.end();
}

static std::vector<uint8_t> loadSynthDefBytes(const std::string& name) {
    std::string path = std::string(SUPERSONIC_SYNTHDEFS_DIR) + "/" + name + ".scsyndef";
    std::filesystem::path fsPath(path);
    std::ifstream f(fsPath, std::ios::binary);
    if (!f) return {};
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)),
                                 std::istreambuf_iterator<char>());
}

TEST_CASE("/d_recv completion message triggers embedded OSC", "[completion]") {
    EngineFixture fx;

    auto synthdefBytes = loadSynthDefBytes("sonic-pi-beep");
    REQUIRE(!synthdefBytes.empty());

    // The completion message will be /status — we should get /status.reply
    auto statusMsg = osc_test::message("/status");
    auto pkt = dRecvWithCompletion(synthdefBytes.data(),
                                    static_cast<uint32_t>(synthdefBytes.size()),
                                    statusMsg);

    fx.send(pkt);

    // Wait for /done from d_recv
    OscReply done;
    REQUIRE(fx.waitForReply("/done", done));

    // The completion message should also have triggered a /status.reply
    OscReply status;
    REQUIRE(fx.waitForReply("/status.reply", status));
}

TEST_CASE("/b_free completion message triggers embedded OSC", "[completion]") {
    EngineFixture fx;

    // Allocate a buffer
    fx.send(osc_test::message("/b_alloc", 0, 1024, 1));
    OscReply alloc;
    REQUIRE(fx.waitForReply("/done", alloc));
    fx.clearReplies();

    // Build /b_free with completion = /status
    auto statusMsg = osc_test::message("/status");
    {
        osc_test::Builder b;
        auto& s = b.begin("/b_free");
        s << (int32_t)0
          << osc::Blob(statusMsg.ptr(),
                       static_cast<osc::osc_bundle_element_size_t>(statusMsg.size()));
        fx.send(b.end());
    }

    OscReply done;
    REQUIRE(fx.waitForReply("/done", done));

    OscReply status;
    REQUIRE(fx.waitForReply("/status.reply", status));
}

TEST_CASE("/b_zero completion message triggers embedded OSC", "[completion]") {
    EngineFixture fx;

    // Allocate a buffer
    fx.send(osc_test::message("/b_alloc", 0, 1024, 1));
    OscReply alloc;
    REQUIRE(fx.waitForReply("/done", alloc));
    fx.clearReplies();

    // Build /b_zero with completion = /status
    auto statusMsg = osc_test::message("/status");
    {
        osc_test::Builder b;
        auto& s = b.begin("/b_zero");
        s << (int32_t)0
          << osc::Blob(statusMsg.ptr(),
                       static_cast<osc::osc_bundle_element_size_t>(statusMsg.size()));
        fx.send(b.end());
    }

    OscReply done;
    REQUIRE(fx.waitForReply("/done", done));

    OscReply status;
    REQUIRE(fx.waitForReply("/status.reply", status));

    // Clean up
    fx.send(osc_test::message("/b_free", 0));
}

TEST_CASE("/d_recv completion chains /s_new", "[completion]") {
    EngineFixture fx;

    auto synthdefBytes = loadSynthDefBytes("sonic-pi-beep");
    REQUIRE(!synthdefBytes.empty());

    // Completion message: /s_new "sonic-pi-beep" 1000 0 1
    osc_test::Builder cb;
    auto& cs = cb.begin("/s_new");
    cs << "sonic-pi-beep" << (int32_t)1000 << (int32_t)0 << (int32_t)1;
    auto sNewMsg = cb.end();

    auto pkt = dRecvWithCompletion(synthdefBytes.data(),
                                    static_cast<uint32_t>(synthdefBytes.size()),
                                    sNewMsg);
    fx.send(pkt);

    // Wait for /done from d_recv
    OscReply done;
    REQUIRE(fx.waitForReply("/done", done));

    // The completion should have created synth 1000
    fx.send(osc_test::message("/status"));
    OscReply status;
    REQUIRE(fx.waitForReply("/status.reply", status));
    CHECK(status.parsed().argInt(2) >= 1);  // numSynths >= 1

    fx.send(osc_test::message("/n_free", 1000));
}

TEST_CASE("/b_alloc completion message triggers /status", "[completion]") {
    EngineFixture fx;

    auto statusMsg = osc_test::message("/status");
    {
        osc_test::Builder b;
        auto& s = b.begin("/b_alloc");
        s << (int32_t)0 << (int32_t)1024 << (int32_t)1
          << osc::Blob(statusMsg.ptr(),
                       static_cast<osc::osc_bundle_element_size_t>(statusMsg.size()));
        fx.send(b.end());
    }

    OscReply done;
    REQUIRE(fx.waitForReply("/done", done));

    OscReply status;
    REQUIRE(fx.waitForReply("/status.reply", status));

    fx.send(osc_test::message("/b_free", 0));
}

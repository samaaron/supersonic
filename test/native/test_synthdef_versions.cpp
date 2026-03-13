/*
 * test_synthdef_versions.cpp — SynthDef version loading (v1/v2/v3 formats)
 *                               and corrupted synthdef resilience.
 */
#include "EngineFixture.h"
#include <fstream>
#include <filesystem>

// The test synthdefs are in supersonic/test/synthdefs/versions/
// They were pre-compiled at different synthdef format versions.
// Internal names: "test_simple" and "test_multi" (same across all versions).
#ifndef SUPERSONIC_TEST_SYNTHDEFS_DIR
#define SUPERSONIC_TEST_SYNTHDEFS_DIR ""
#endif

static bool loadTestSynthDef(EngineFixture& fx, const std::string& name) {
    std::string path = std::string(SUPERSONIC_TEST_SYNTHDEFS_DIR) + "/versions/" + name + ".scsyndef";
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

static osc_test::Packet sNew(const char* def, int32_t id, int32_t addAction, int32_t target) {
    osc_test::Builder b;
    auto& s = b.begin("/s_new");
    s << def << id << addAction << target;
    return b.end();
}

// =============================================================================
// VERSION LOADING
// =============================================================================

TEST_CASE("loads v1 format synthdef", "[synthdef_version]") {
    EngineFixture fx;
    REQUIRE(loadTestSynthDef(fx, "test_simple_v1"));

    fx.send(osc_test::message("/status"));
    OscReply r;
    REQUIRE(fx.waitForReply("/status.reply", r));
    CHECK(r.parsed().argInt(4) >= 1);
}

TEST_CASE("loads v2 format synthdef", "[synthdef_version]") {
    EngineFixture fx;
    REQUIRE(loadTestSynthDef(fx, "test_simple_v2"));

    fx.send(osc_test::message("/status"));
    OscReply r;
    REQUIRE(fx.waitForReply("/status.reply", r));
    CHECK(r.parsed().argInt(4) >= 1);
}

TEST_CASE("loads v3 format synthdef", "[synthdef_version]") {
    EngineFixture fx;
    REQUIRE(loadTestSynthDef(fx, "test_simple_v3"));

    fx.send(osc_test::message("/status"));
    OscReply r;
    REQUIRE(fx.waitForReply("/status.reply", r));
    CHECK(r.parsed().argInt(4) >= 1);
}

// =============================================================================
// SYNTH CREATION WITH VERSIONED DEFS
// =============================================================================

TEST_CASE("synth creation with v1 synthdef", "[synthdef_version]") {
    EngineFixture fx;
    REQUIRE(loadTestSynthDef(fx, "test_simple_v1"));

    fx.send(sNew("test_simple", 1000, 0, 1));
    fx.pump(8);

    fx.send(osc_test::message("/status"));
    OscReply r;
    REQUIRE(fx.waitForReply("/status.reply", r));
    CHECK(r.parsed().argInt(2) >= 1);  // numSynths

    fx.send(osc_test::message("/n_free", 1000));
    fx.pump(4);
}

TEST_CASE("synth creation with v2 synthdef", "[synthdef_version]") {
    EngineFixture fx;
    REQUIRE(loadTestSynthDef(fx, "test_simple_v2"));

    fx.send(sNew("test_simple", 1000, 0, 1));
    fx.pump(8);

    fx.send(osc_test::message("/status"));
    OscReply r;
    REQUIRE(fx.waitForReply("/status.reply", r));
    CHECK(r.parsed().argInt(2) >= 1);

    fx.send(osc_test::message("/n_free", 1000));
    fx.pump(4);
}

TEST_CASE("synth creation with v3 synthdef", "[synthdef_version]") {
    EngineFixture fx;
    REQUIRE(loadTestSynthDef(fx, "test_simple_v3"));

    fx.send(sNew("test_simple", 1000, 0, 1));
    fx.pump(8);

    fx.send(osc_test::message("/status"));
    OscReply r;
    REQUIRE(fx.waitForReply("/status.reply", r));
    CHECK(r.parsed().argInt(2) >= 1);

    fx.send(osc_test::message("/n_free", 1000));
    fx.pump(4);
}

// =============================================================================
// MULTI-OUTPUT SYNTHDEF VERSIONS
// =============================================================================

TEST_CASE("loads v1 multi-output synthdef", "[synthdef_version]") {
    EngineFixture fx;
    REQUIRE(loadTestSynthDef(fx, "test_multi_v1"));

    fx.send(sNew("test_multi", 1000, 0, 1));
    fx.pump(8);

    fx.send(osc_test::message("/status"));
    OscReply r;
    REQUIRE(fx.waitForReply("/status.reply", r));
    CHECK(r.parsed().argInt(2) >= 1);

    fx.send(osc_test::message("/n_free", 1000));
    fx.pump(4);
}

TEST_CASE("loads v2 multi-output synthdef", "[synthdef_version]") {
    EngineFixture fx;
    REQUIRE(loadTestSynthDef(fx, "test_multi_v2"));

    fx.send(sNew("test_multi", 1000, 0, 1));
    fx.pump(8);

    fx.send(osc_test::message("/status"));
    OscReply r;
    REQUIRE(fx.waitForReply("/status.reply", r));
    CHECK(r.parsed().argInt(2) >= 1);

    fx.send(osc_test::message("/n_free", 1000));
    fx.pump(4);
}

TEST_CASE("loads v3 multi-output synthdef", "[synthdef_version]") {
    EngineFixture fx;
    REQUIRE(loadTestSynthDef(fx, "test_multi_v3"));

    fx.send(sNew("test_multi", 1000, 0, 1));
    fx.pump(8);

    fx.send(osc_test::message("/status"));
    OscReply r;
    REQUIRE(fx.waitForReply("/status.reply", r));
    CHECK(r.parsed().argInt(2) >= 1);

    fx.send(osc_test::message("/n_free", 1000));
    fx.pump(4);
}

// =============================================================================
// SYNTHDEF COUNT
// =============================================================================

TEST_CASE("multiple synthdef versions increase count", "[synthdef_version]") {
    EngineFixture fx;

    fx.send(osc_test::message("/status"));
    OscReply before;
    REQUIRE(fx.waitForReply("/status.reply", before));
    int countBefore = before.parsed().argInt(4);

    REQUIRE(loadTestSynthDef(fx, "test_simple_v1"));
    REQUIRE(loadTestSynthDef(fx, "test_multi_v1"));

    fx.send(osc_test::message("/status"));
    OscReply after;
    REQUIRE(fx.waitForReply("/status.reply", after));
    int countAfter = after.parsed().argInt(4);

    CHECK(countAfter >= countBefore + 2);
}

TEST_CASE("reloading same synthdef replaces previous", "[synthdef_version]") {
    EngineFixture fx;

    fx.send(osc_test::message("/status"));
    OscReply before;
    REQUIRE(fx.waitForReply("/status.reply", before));
    int countBefore = before.parsed().argInt(4);

    // Load v1 then v2 of same internal name "test_simple"
    REQUIRE(loadTestSynthDef(fx, "test_simple_v1"));
    REQUIRE(loadTestSynthDef(fx, "test_simple_v2"));

    fx.send(osc_test::message("/status"));
    OscReply after;
    REQUIRE(fx.waitForReply("/status.reply", after));
    int countAfter = after.parsed().argInt(4);

    // Same internal name should replace, not double-count
    CHECK(countAfter == countBefore + 1);
}

// =============================================================================
// CORRUPTED SYNTHDEF RESILIENCE
// =============================================================================

TEST_CASE("corrupted synthdef does not crash engine", "[synthdef_version]") {
    EngineFixture fx;

    // Send garbage data as a synthdef
    std::vector<uint8_t> garbage(256, 0xAB);
    auto pkt = osc_test::messageWithBlob("/d_recv", garbage.data(), garbage.size());
    fx.send(pkt);
    fx.pump(16);

    // Engine should still work
    fx.send(osc_test::message("/status"));
    OscReply r;
    REQUIRE(fx.waitForReply("/status.reply", r));
}

TEST_CASE("valid synthdef loads after corrupted attempt", "[synthdef_version]") {
    EngineFixture fx;

    // Send garbage first
    std::vector<uint8_t> garbage(128, 0xFF);
    auto bad = osc_test::messageWithBlob("/d_recv", garbage.data(), garbage.size());
    fx.send(bad);
    fx.pump(16);

    // Now load a valid synthdef
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    // Create a synth with it
    fx.send(sNew("sonic-pi-beep", 1000, 0, 1));
    fx.pump(8);

    fx.send(osc_test::message("/status"));
    OscReply r;
    REQUIRE(fx.waitForReply("/status.reply", r));
    CHECK(r.parsed().argInt(2) >= 1);

    fx.send(osc_test::message("/n_free", 1000));
    fx.pump(4);
}

TEST_CASE("truncated synthdef header does not crash", "[synthdef_version]") {
    EngineFixture fx;

    // Send just "SCgf" header with truncated data
    std::vector<uint8_t> truncated = {'S', 'C', 'g', 'f', 0, 0, 0, 2};
    auto pkt = osc_test::messageWithBlob("/d_recv", truncated.data(), truncated.size());
    fx.send(pkt);
    fx.pump(16);

    fx.send(osc_test::message("/status"));
    OscReply r;
    REQUIRE(fx.waitForReply("/status.reply", r));
}

TEST_CASE("empty blob d_recv does not crash", "[synthdef_version]") {
    EngineFixture fx;

    std::vector<uint8_t> empty;
    auto pkt = osc_test::messageWithBlob("/d_recv", empty.data(), 0);
    fx.send(pkt);
    fx.pump(16);

    fx.send(osc_test::message("/status"));
    OscReply r;
    REQUIRE(fx.waitForReply("/status.reply", r));
}

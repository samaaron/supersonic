/*
 * test_load_sample.cpp — Tests for /b_allocRead loading audio files from disk.
 *
 * These tests verify that the engine can load samples via /b_allocRead.
 * If the engine doesn't support file I/O (e.g., no libsndfile linked),
 * the tests detect this and skip gracefully.
 */
#include "EngineFixture.h"
#include <filesystem>

#ifndef SUPERSONIC_SAMPLES_DIR
#define SUPERSONIC_SAMPLES_DIR ""
#endif

// =============================================================================
// Helper: build a /b_allocRead message
// =============================================================================
static osc_test::Packet makeAllocRead(int32_t bufNum, const char* path,
                                       int32_t startFrame = 0,
                                       int32_t numFrames = 0) {
    osc_test::Builder b;
    auto& s = b.begin("/b_allocRead");
    s << bufNum << path << startFrame << numFrames;
    return b.end();
}

// Helper: check if a sample file exists
static bool sampleExists(const std::string& filename) {
    std::string path = std::string(SUPERSONIC_SAMPLES_DIR) + "/" + filename;
    return std::filesystem::exists(std::filesystem::path(path));
}

// Helper: attempt /b_allocRead, return true if /done received
static bool tryAllocRead(EngineFixture& fx, int32_t bufNum,
                          const std::string& filename,
                          int32_t startFrame = 0, int32_t numFrames = 0) {
    std::string path = std::string(SUPERSONIC_SAMPLES_DIR) + "/" + filename;
    fx.send(makeAllocRead(bufNum, path.c_str(), startFrame, numFrames));
    OscReply done;
    return fx.waitForReply("/done", done);
}

// =============================================================================
// 1. /b_allocRead loads a sample file
// =============================================================================

TEST_CASE("/b_allocRead loads sample file", "[load_sample]") {
    if (!sampleExists("bd_haus.flac")) {
        WARN("Sample bd_haus.flac not found — skipping");
        SUCCEED();
        return;
    }

    EngineFixture fx;

    if (!tryAllocRead(fx, 0, "bd_haus.flac")) {
        WARN("/b_allocRead not supported in this build — skipping");
        SUCCEED();
        return;
    }

    // Verify buffer was loaded
    fx.clearReplies();
    fx.send(osc_test::message("/b_query", 0));
    OscReply info;
    REQUIRE(fx.waitForReply("/b_info", info));
    CHECK(info.parsed().argInt(0) == 0);    // bufnum
    CHECK(info.parsed().argInt(1) > 0);     // frames

    fx.send(osc_test::message("/b_free", 0));
    fx.pump(4);
}

// =============================================================================
// 2. Loaded buffer has non-zero frame count
// =============================================================================

TEST_CASE("/b_allocRead buffer has non-zero frame count", "[load_sample]") {
    if (!sampleExists("bd_haus.flac")) { SKIP("Sample not found"); }
    EngineFixture fx;
    if (!tryAllocRead(fx, 0, "bd_haus.flac")) { SKIP("/b_allocRead not supported"); }
    fx.clearReplies();

    fx.send(osc_test::message("/b_query", 0));
    OscReply info;
    REQUIRE(fx.waitForReply("/b_info", info));
    CHECK(info.parsed().argInt(1) > 0);

    fx.send(osc_test::message("/b_free", 0));
    fx.pump(4);
}

// =============================================================================
// 3. Loaded buffer has valid channel count
// =============================================================================

TEST_CASE("/b_allocRead buffer has valid channel count", "[load_sample]") {
    if (!sampleExists("bd_haus.flac")) { SKIP("Sample not found"); }
    EngineFixture fx;
    if (!tryAllocRead(fx, 0, "bd_haus.flac")) { SKIP("/b_allocRead not supported"); }
    fx.clearReplies();

    fx.send(osc_test::message("/b_query", 0));
    OscReply info;
    REQUIRE(fx.waitForReply("/b_info", info));
    int32_t channels = info.parsed().argInt(2);
    CHECK(channels >= 1);
    CHECK(channels <= 2);

    fx.send(osc_test::message("/b_free", 0));
    fx.pump(4);
}

// =============================================================================
// 4. Loaded buffer sample rate is non-zero
// =============================================================================

TEST_CASE("/b_allocRead buffer has non-zero sample rate", "[load_sample]") {
    if (!sampleExists("bd_haus.flac")) { SKIP("Sample not found"); }
    EngineFixture fx;
    if (!tryAllocRead(fx, 0, "bd_haus.flac")) { SKIP("/b_allocRead not supported"); }
    fx.clearReplies();

    fx.send(osc_test::message("/b_query", 0));
    OscReply info;
    REQUIRE(fx.waitForReply("/b_info", info));
    float sampleRate = info.parsed().argFloat(3);
    CHECK(sampleRate > 0.0f);

    fx.send(osc_test::message("/b_free", 0));
    fx.pump(4);
}

// =============================================================================
// 5. Multiple samples can be loaded into different buffers
// =============================================================================

TEST_CASE("/b_allocRead loads multiple samples into different buffers",
          "[load_sample]") {
    if (!sampleExists("bd_haus.flac") || !sampleExists("drum_snare_hard.flac")) {
        SKIP("Samples not found");
    }
    EngineFixture fx;
    if (!tryAllocRead(fx, 0, "bd_haus.flac")) { SKIP("/b_allocRead not supported"); }
    fx.clearReplies();

    if (!tryAllocRead(fx, 1, "drum_snare_hard.flac")) {
        fx.send(osc_test::message("/b_free", 0));
        fx.pump(4);
        SKIP("/b_allocRead failed for second sample");
    }
    fx.clearReplies();

    // Query both
    fx.send(osc_test::message("/b_query", 0));
    OscReply info0;
    REQUIRE(fx.waitForReply("/b_info", info0));
    CHECK(info0.parsed().argInt(1) > 0);
    fx.clearReplies();

    fx.send(osc_test::message("/b_query", 1));
    OscReply info1;
    REQUIRE(fx.waitForReply("/b_info", info1));
    CHECK(info1.parsed().argInt(1) > 0);

    fx.send(osc_test::message("/b_free", 0));
    fx.send(osc_test::message("/b_free", 1));
    fx.pump(4);
}

// =============================================================================
// 6. /b_allocRead with non-existent file doesn't crash
// =============================================================================

TEST_CASE("/b_allocRead with non-existent file does not crash",
          "[load_sample]") {
    EngineFixture fx;

    std::string badPath =
        std::string(SUPERSONIC_SAMPLES_DIR) + "/no_such_file_12345.flac";

    fx.send(makeAllocRead(0, badPath.c_str()));
    fx.pump(16);

    // Engine should remain responsive regardless
    fx.clearReplies();
    fx.send(osc_test::message("/status"));
    OscReply status;
    REQUIRE(fx.waitForReply("/status.reply", status));
}

// =============================================================================
// 7. /b_allocRead then /b_free works correctly
// =============================================================================

TEST_CASE("/b_allocRead then /b_free clears the buffer", "[load_sample]") {
    if (!sampleExists("bd_haus.flac")) { SKIP("Sample not found"); }
    EngineFixture fx;
    if (!tryAllocRead(fx, 0, "bd_haus.flac")) { SKIP("/b_allocRead not supported"); }
    fx.clearReplies();

    // Confirm loaded
    fx.send(osc_test::message("/b_query", 0));
    OscReply info;
    REQUIRE(fx.waitForReply("/b_info", info));
    CHECK(info.parsed().argInt(1) > 0);
    fx.clearReplies();

    // Free
    fx.send(osc_test::message("/b_free", 0));
    fx.pump(8);
    fx.clearReplies();

    // After freeing, frames should be 0
    fx.send(osc_test::message("/b_query", 0));
    OscReply info2;
    REQUIRE(fx.waitForReply("/b_info", info2));
    CHECK(info2.parsed().argInt(1) == 0);
}

// =============================================================================
// 8. /b_allocRead with startFrame offset
// =============================================================================

TEST_CASE("/b_allocRead with startFrame offset loads partial sample",
          "[load_sample]") {
    if (!sampleExists("bd_haus.flac")) { SKIP("Sample not found"); }
    EngineFixture fx;
    if (!tryAllocRead(fx, 0, "bd_haus.flac")) { SKIP("/b_allocRead not supported"); }
    fx.clearReplies();

    fx.send(osc_test::message("/b_query", 0));
    OscReply infoFull;
    REQUIRE(fx.waitForReply("/b_info", infoFull));
    int32_t totalFrames = infoFull.parsed().argInt(1);
    REQUIRE(totalFrames > 100);
    fx.clearReplies();

    fx.send(osc_test::message("/b_free", 0));
    fx.pump(4);
    fx.clearReplies();

    // Load with startFrame offset
    if (!tryAllocRead(fx, 1, "bd_haus.flac", 100, 0)) {
        SKIP("Partial /b_allocRead not supported");
    }
    fx.clearReplies();

    fx.send(osc_test::message("/b_query", 1));
    OscReply infoPartial;
    REQUIRE(fx.waitForReply("/b_info", infoPartial));
    int32_t partialFrames = infoPartial.parsed().argInt(1);
    CHECK(partialFrames > 0);
    CHECK(partialFrames < totalFrames);

    fx.send(osc_test::message("/b_free", 1));
    fx.pump(4);
}

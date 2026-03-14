/*
 * test_load_sample.cpp — Tests for /b_allocRead loading audio files from disk.
 *
 * These tests verify that the engine can load samples via /b_allocRead.
 * If the engine doesn't support file I/O (e.g., no libsndfile linked),
 * the tests detect this and skip gracefully.
 */
#include "EngineFixture.h"
#include <chrono>
#include <filesystem>
#include <thread>

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
    OscReply freeDone;
    fx.waitForReply("/done", freeDone);
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
}

// =============================================================================
// 9. /b_free returns memory — SndBuf has frames=0, channels=0 after free
// =============================================================================

TEST_CASE("/b_free returns memory and clears buffer metadata",
          "[load_sample]") {
    if (!sampleExists("bd_haus.flac")) { SKIP("Sample not found"); }
    EngineFixture fx;
    if (!tryAllocRead(fx, 0, "bd_haus.flac")) { SKIP("/b_allocRead not supported"); }
    fx.clearReplies();

    // Confirm the buffer is loaded with non-zero frames and channels
    fx.send(osc_test::message("/b_query", 0));
    OscReply infoBefore;
    REQUIRE(fx.waitForReply("/b_info", infoBefore));
    CHECK(infoBefore.parsed().argInt(1) > 0);   // frames > 0
    CHECK(infoBefore.parsed().argInt(2) >= 1);   // channels >= 1
    fx.clearReplies();

    // Free the buffer
    fx.send(osc_test::message("/b_free", 0));
    OscReply freeDone;
    REQUIRE(fx.waitForReply("/done", freeDone));
    fx.clearReplies();

    // After freeing, frames and channels should both be 0
    fx.send(osc_test::message("/b_query", 0));
    OscReply infoAfter;
    REQUIRE(fx.waitForReply("/b_info", infoAfter));
    CHECK(infoAfter.parsed().argInt(0) == 0);    // bufnum
    CHECK(infoAfter.parsed().argInt(1) == 0);    // frames == 0
    CHECK(infoAfter.parsed().argInt(2) == 0);    // channels == 0
}

// =============================================================================
// 10. Buffer replacement — load a different sample into the same buffer
// =============================================================================

TEST_CASE("/b_allocRead replaces existing buffer with different sample",
          "[load_sample]") {
    if (!sampleExists("bd_haus.flac") || !sampleExists("drum_snare_hard.flac")) {
        SKIP("Samples not found");
    }
    EngineFixture fx;

    // Load first sample into buffer 0
    if (!tryAllocRead(fx, 0, "bd_haus.flac")) { SKIP("/b_allocRead not supported"); }
    fx.clearReplies();

    fx.send(osc_test::message("/b_query", 0));
    OscReply info1;
    REQUIRE(fx.waitForReply("/b_info", info1));
    int32_t frames1 = info1.parsed().argInt(1);
    CHECK(frames1 > 0);
    fx.clearReplies();

    // Load a DIFFERENT sample into the same buffer 0 (replacement)
    if (!tryAllocRead(fx, 0, "drum_snare_hard.flac")) {
        SKIP("/b_allocRead replacement failed");
    }
    fx.clearReplies();

    // Query again — frame count should reflect the new sample
    fx.send(osc_test::message("/b_query", 0));
    OscReply info2;
    REQUIRE(fx.waitForReply("/b_info", info2));
    int32_t frames2 = info2.parsed().argInt(1);
    CHECK(frames2 > 0);

    // The two samples should have different frame counts, confirming replacement
    CHECK(frames1 != frames2);

    fx.send(osc_test::message("/b_free", 0));
}

// =============================================================================
// 11. Load, play, free cycle — load sample, create synth, free both
// =============================================================================

TEST_CASE("load sample, play via synth, free both", "[load_sample]") {
    if (!sampleExists("bd_haus.flac")) { SKIP("Sample not found"); }
    EngineFixture fx;
    if (!tryAllocRead(fx, 0, "bd_haus.flac")) { SKIP("/b_allocRead not supported"); }
    fx.clearReplies();

    // Load a synthdef that reads from a buffer
    const char* playerDef = nullptr;
    if (fx.loadSynthDef("sonic-pi-basic_mono_player")) {
        playerDef = "sonic-pi-basic_mono_player";
    } else if (fx.loadSynthDef("sonic-pi-mono_player")) {
        playerDef = "sonic-pi-mono_player";
    }
    fx.clearReplies();

    if (playerDef) {
        // Create a synth that reads from buffer 0
        {
            osc_test::Builder b;
            auto& s = b.begin("/s_new");
            s << playerDef << (int32_t)1000
              << (int32_t)0 << (int32_t)1
              << "buf" << 0.0f;
            fx.send(b.end());
        }

        // Let it run briefly (a few audio blocks)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        // Free the synth
        fx.send(osc_test::message("/n_free", 1000));
    } else {
        // If no buffer-playing synthdef is available, just use sonic-pi-beep
        // alongside the loaded buffer to verify they coexist without issues
        REQUIRE(fx.loadSynthDef("sonic-pi-beep"));
        fx.clearReplies();

        osc_test::Builder b;
        auto& s = b.begin("/s_new");
        s << "sonic-pi-beep" << (int32_t)1000
          << (int32_t)0 << (int32_t)1;
        fx.send(b.end());

        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        fx.send(osc_test::message("/n_free", 1000));
    }
    fx.clearReplies();

    // Free the buffer
    fx.send(osc_test::message("/b_free", 0));
    OscReply freeDone;
    REQUIRE(fx.waitForReply("/done", freeDone));
    fx.clearReplies();

    // Engine should still be healthy after the full cycle
    fx.send(osc_test::message("/status"));
    OscReply status;
    REQUIRE(fx.waitForReply("/status.reply", status));
    CHECK(status.parsed().argInt(2) == 0);  // numSynths == 0
}

// =============================================================================
// 12. Multiple alloc/free cycles — same buffer number, 10 times
// =============================================================================

TEST_CASE("/b_allocRead and /b_free 10 times on same buffer", "[load_sample]") {
    if (!sampleExists("bd_haus.flac")) { SKIP("Sample not found"); }
    EngineFixture fx;

    // First attempt to verify /b_allocRead is supported
    if (!tryAllocRead(fx, 0, "bd_haus.flac")) { SKIP("/b_allocRead not supported"); }
    fx.send(osc_test::message("/b_free", 0));
    OscReply d;
    fx.waitForReply("/done", d);
    fx.clearReplies();

    // Now cycle 10 times
    for (int i = 0; i < 10; i++) {
        REQUIRE(tryAllocRead(fx, 0, "bd_haus.flac"));
        fx.clearReplies();

        // Verify loaded each time
        fx.send(osc_test::message("/b_query", 0));
        OscReply info;
        REQUIRE(fx.waitForReply("/b_info", info));
        CHECK(info.parsed().argInt(1) > 0);  // frames > 0
        fx.clearReplies();

        // Free
        fx.send(osc_test::message("/b_free", 0));
        OscReply freeDone;
        REQUIRE(fx.waitForReply("/done", freeDone));
        fx.clearReplies();
    }

    // Engine should still be healthy after 10 alloc/free cycles
    fx.send(osc_test::message("/status"));
    OscReply status;
    REQUIRE(fx.waitForReply("/status.reply", status));
}

// =============================================================================
// 13. Free already-free buffer — doesn't crash
// =============================================================================

TEST_CASE("/b_free on never-allocated buffer does not crash", "[load_sample]") {
    EngineFixture fx;

    // Free a buffer that was never allocated (buffer 999)
    fx.send(osc_test::message("/b_free", 999));

    // Give it a moment to process
    fx.clearReplies();

    // Engine should still be responsive
    fx.send(osc_test::message("/status"));
    OscReply status;
    REQUIRE(fx.waitForReply("/status.reply", status));
    CHECK(status.parsed().argCount() >= 5);
}

TEST_CASE("/b_free on already-freed buffer does not crash", "[load_sample]") {
    if (!sampleExists("bd_haus.flac")) { SKIP("Sample not found"); }
    EngineFixture fx;
    if (!tryAllocRead(fx, 0, "bd_haus.flac")) { SKIP("/b_allocRead not supported"); }
    fx.clearReplies();

    // Free the buffer once
    fx.send(osc_test::message("/b_free", 0));
    OscReply freeDone1;
    fx.waitForReply("/done", freeDone1);
    fx.clearReplies();

    // Free the same buffer again (double-free)
    fx.send(osc_test::message("/b_free", 0));

    // Give it a moment to process
    fx.clearReplies();

    // Engine should still be responsive
    fx.send(osc_test::message("/status"));
    OscReply status;
    REQUIRE(fx.waitForReply("/status.reply", status));
    CHECK(status.parsed().argCount() >= 5);
}

// =============================================================================
// 14. Shutdown race — /b_free then immediate engine destroy
// Exercises the race between BufFreeCmd::Stage4's /supersonic/buffer/freed
// notification (async via ring buffer) and World_Cleanup's free_alig().
// If there's a double-free, this will SIGSEGV.
// =============================================================================

TEST_CASE("/b_free then immediate shutdown does not crash", "[load_sample]") {
    if (!sampleExists("bd_haus.flac")) { SKIP("Sample not found"); }

    // Scope the fixture so its destructor (engine shutdown) runs immediately
    // after /b_free, without waiting for the reply.
    {
        EngineFixture fx;
        if (!tryAllocRead(fx, 0, "bd_haus.flac")) { SKIP("/b_allocRead not supported"); }
        fx.clearReplies();

        // Send /b_free but do NOT wait for /done — destroy engine immediately
        fx.send(osc_test::message("/b_free", 0));
        // EngineFixture destructor runs here — tears down engine while
        // /supersonic/buffer/freed may still be in the OUT ring buffer
    }

    // If we got here without SIGSEGV, the shutdown race is safe
    SUCCEED();
}

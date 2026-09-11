/*
 * test_load_sample.cpp — samples, loaded by the CLIENT into the inbox lane
 * and handed over as assets (SampleLane.h), then /b_read and /b_write.
 *
 * The engine does not read files any more: a client decodes and commits, as
 * the web client always did and as SuperSonic's socket front does for a
 * sender that still says /b_allocRead. Every case that used to send that verb
 * loads through the lane instead — the same buffer, the same frames, the same
 * /b_query answers — and /b_allocRead itself is now refused by the engine
 * with a pointer to where the work belongs.
 */
#include "EngineFixture.h"
#include "SampleLane.h"
#include "clockwork_audio_file.h"
#include <catch2/catch_approx.hpp>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <thread>
#include <unistd.h>
#include <vector>
#include <algorithm>
#include <cmath>

#ifndef CLOCKWORK_SAMPLES_DIR
#define CLOCKWORK_SAMPLES_DIR ""
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
    std::string path = std::string(CLOCKWORK_SAMPLES_DIR) + "/" + filename;
    return std::filesystem::exists(std::filesystem::path(path));
}

// Helper: load a sample as a client does — decode, stage in the lane, commit
// as an asset keyed by the buffer number. True once the engine has it.
static bool tryAllocRead(EngineFixture& fx, int32_t bufNum,
                          const std::string& filename,
                          int32_t startFrame = 0, int32_t numFrames = 0) {
    std::string path = std::string(CLOCKWORK_SAMPLES_DIR) + "/" + filename;
    return sample_lane::load(fx, bufNum, path, startFrame, numFrames);
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

    REQUIRE(tryAllocRead(fx, 0, "bd_haus.flac"));

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
    REQUIRE(tryAllocRead(fx, 0, "bd_haus.flac"));
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
    REQUIRE(tryAllocRead(fx, 0, "bd_haus.flac"));
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
    REQUIRE(tryAllocRead(fx, 0, "bd_haus.flac"));
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
    REQUIRE(tryAllocRead(fx, 0, "bd_haus.flac"));
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

TEST_CASE("a non-existent file fails on the client's side; the engine never hears of it",
          "[load_sample]") {
    EngineFixture fx;

    std::string badPath =
        std::string(CLOCKWORK_SAMPLES_DIR) + "/no_such_file_12345.flac";
    CHECK_FALSE(sample_lane::load(fx, 0, badPath));

    // Engine should remain responsive regardless
    fx.clearReplies();
    fx.send(osc_test::message("/status"));
    OscReply status;
    REQUIRE(fx.waitForReply("/status.reply", status));
}

TEST_CASE("/b_allocRead sent to the engine is refused, naming where the work belongs",
          "[load_sample]") {
    EngineFixture fx;
    std::string path = std::string(CLOCKWORK_SAMPLES_DIR) + "/bd_haus.flac";
    fx.send(makeAllocRead(0, path.c_str()));
    OscReply fail;
    REQUIRE(fx.waitForReply("/fail", fail));
    CHECK(fail.parsed().argString(0) == "/b_allocRead");
    CHECK(fail.parsed().argString(1).find("asset") != std::string::npos);
    // And nothing was loaded.
    fx.clearReplies();
    fx.send(osc_test::message("/b_query", 0));
    OscReply info;
    REQUIRE(fx.waitForReply("/b_info", info));
    CHECK(info.parsed().argInt(1) == 0);
}

// =============================================================================
// 7. /b_allocRead then /b_free works correctly
// =============================================================================

TEST_CASE("/b_allocRead then /b_free clears the buffer", "[load_sample]") {
    if (!sampleExists("bd_haus.flac")) { SKIP("Sample not found"); }
    EngineFixture fx;
    REQUIRE(tryAllocRead(fx, 0, "bd_haus.flac"));
    fx.clearReplies();

    // Confirm loaded
    fx.send(osc_test::message("/b_query", 0));
    OscReply info;
    REQUIRE(fx.waitForReply("/b_info", info));
    CHECK(info.parsed().argInt(1) > 0);
    fx.clearReplies();

    // Free. /b_free is asynchronous: the buffer is swapped out on the audio
    // thread once the off-thread stage has run, and scsynth says so with
    // /done /b_free — which is what a client waits for before it asks.
    fx.send(osc_test::message("/b_free", 0));
    REQUIRE(fx.waitForDone("/b_free"));
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
    REQUIRE(tryAllocRead(fx, 0, "bd_haus.flac"));
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
    REQUIRE(tryAllocRead(fx, 1, "bd_haus.flac", 100, 0));
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
    REQUIRE(tryAllocRead(fx, 0, "bd_haus.flac"));
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
    REQUIRE(tryAllocRead(fx, 0, "bd_haus.flac"));
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
    REQUIRE(tryAllocRead(fx, 0, "bd_haus.flac"));
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
    REQUIRE(tryAllocRead(fx, 0, "bd_haus.flac"));
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
    REQUIRE(tryAllocRead(fx, 0, "bd_haus.flac"));
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
        REQUIRE(tryAllocRead(fx, 0, "bd_haus.flac"));
        fx.clearReplies();

        // Send /b_free but do NOT wait for /done — destroy engine immediately
        fx.send(osc_test::message("/b_free", 0));
        // EngineFixture destructor runs here — tears down engine while
        // /supersonic/buffer/freed may still be in the OUT ring buffer
    }

    // If we got here without SIGSEGV, the shutdown race is safe
    SUCCEED();
}

// =============================================================================
// The frames are the file's, and /b_read and /b_write speak the same codecs
// =============================================================================

namespace {

// The first `count` interleaved samples of a buffer, through /b_getn.
static std::vector<float> firstSamples(EngineFixture& fx, int32_t bufNum, int32_t count) {
    fx.clearReplies();
    osc_test::Builder b;
    auto& s = b.begin("/b_getn");
    s << bufNum << int32_t{0} << count;
    fx.send(b.end());
    OscReply r;
    REQUIRE(fx.waitForReply("/b_setn", r));
    auto p = r.parsed();
    REQUIRE(p.argInt(2) == count);
    std::vector<float> out;
    for (int32_t i = 0; i < count; ++i) out.push_back(p.argFloat(3 + i));
    return out;
}

// The file as the codec alone reads it: the oracle the engine is held to.
struct Decoded {
    ClockworkAudioInfo info{};
    std::vector<float> samples;
};
static Decoded decodeFile(const std::string& path) {
    Decoded d;
    d.info.struct_bytes = sizeof(d.info);
    float* frames = nullptr;
    REQUIRE(clockwork_audio_decode_file(path.c_str(), &d.info, &frames) == CLOCKWORK_OK);
    d.samples.assign(frames, frames + d.info.frames * d.info.channels);
    clockwork_audio_free(frames);
    return d;
}

static std::string tempPath(const char* stem, const char* ext) {
    static int n = 0;
    return (std::filesystem::temp_directory_path() /
            (std::string(stem) + "-" + std::to_string(::getpid()) + "-" + std::to_string(++n) + "." + ext)).string();
}

} // namespace

TEST_CASE("/b_allocRead: the buffer holds the file's frames, at the file's rate", "[load_sample]") {
    if (!sampleExists("bd_haus.flac")) { SKIP("Sample not found"); }
    const std::string path = std::string(CLOCKWORK_SAMPLES_DIR) + "/bd_haus.flac";
    const Decoded file = decodeFile(path);

    EngineFixture fx;
    REQUIRE(tryAllocRead(fx, 0, "bd_haus.flac"));
    fx.clearReplies();

    fx.send(osc_test::message("/b_query", 0));
    OscReply info;
    REQUIRE(fx.waitForReply("/b_info", info));
    CHECK(info.parsed().argInt(1) == (int32_t)file.info.frames);
    CHECK(info.parsed().argInt(2) == (int32_t)file.info.channels);
    CHECK(info.parsed().argFloat(3) == Catch::Approx((float)file.info.sample_rate));

    // Not the first few, which a drum hit can leave near zero: a run from a
    // little way in, where the samples are unmistakably the file's.
    const int32_t from = 2000, count = 16;
    fx.clearReplies();
    {
        osc_test::Builder b;
        auto& s = b.begin("/b_getn");
        s << int32_t{0} << from << count;
        fx.send(b.end());
    }
    OscReply r;
    REQUIRE(fx.waitForReply("/b_setn", r));
    auto p = r.parsed();
    REQUIRE(p.argInt(2) == count);
    for (int32_t i = 0; i < count; ++i)
        CHECK(p.argFloat(3 + i) == Catch::Approx(file.samples[from + i]).margin(1e-6f));

    // A partial load starts where it was asked to: its first samples are the
    // file's from that frame, not from the top.
    const int32_t ch = (int32_t)file.info.channels;
    REQUIRE(tryAllocRead(fx, 1, "bd_haus.flac", from / ch, 0));
    const auto partial = firstSamples(fx, 1, count);
    for (int32_t i = 0; i < count; ++i)
        CHECK(partial[i] == Catch::Approx(file.samples[(from / ch) * ch + i]).margin(1e-6f));

    fx.send(osc_test::message("/b_free", 0));
    fx.send(osc_test::message("/b_free", 1));
}

TEST_CASE("the file verbs sent to the engine are refused, each in its own name", "[load_sample]") {
    // The engine has no files: /b_read, /b_readChannel, /b_allocReadChannel
    // and /b_write are the client's (SuperSonic's front). What reaches the
    // engine under those names is refused, naming the verb and the buffer.
    EngineFixture fx;
    fx.send(osc_test::message("/b_alloc", 3, (int32_t)100, (int32_t)1));
    REQUIRE(fx.waitForDone("/b_alloc"));
    struct V { const char* verb; osc_test::Packet pkt; };
    osc_test::Builder b1; b1.begin("/b_read") << int32_t{3} << "x.wav" << int32_t{0} << int32_t{-1} << int32_t{0} << int32_t{0};
    osc_test::Builder b2; b2.begin("/b_readChannel") << int32_t{3} << "x.wav" << int32_t{0} << int32_t{-1} << int32_t{0} << int32_t{0} << int32_t{0};
    osc_test::Builder b3; b3.begin("/b_allocReadChannel") << int32_t{3} << "x.wav" << int32_t{0} << int32_t{0} << int32_t{0};
    osc_test::Builder b4; b4.begin("/b_write") << int32_t{3} << "x.wav" << "wav" << "int16" << int32_t{-1} << int32_t{0} << int32_t{0};
    const V verbs[] = { {"/b_read", b1.end()}, {"/b_readChannel", b2.end()}, {"/b_allocReadChannel", b3.end()}, {"/b_write", b4.end()} };
    for (const auto& v : verbs) {
        fx.clearReplies();
        fx.send(v.pkt);
        OscReply fail;
        REQUIRE(fx.waitForReply("/fail", fail));
        CHECK(fail.parsed().argString(0) == v.verb);
        CHECK(fail.parsed().argInt(2) == 3);
    }
    fx.send(osc_test::message("/b_free", 3));
}

TEST_CASE("/supersonic/buffer/read: frames from the lane into an allocated buffer, offsets honoured, rate set",
          "[load_sample][lane]") {
    if (!sampleExists("bd_haus.flac")) { SKIP("Sample not found"); }
    const std::string path = std::string(CLOCKWORK_SAMPLES_DIR) + "/bd_haus.flac";
    const Decoded file = decodeFile(path);
    REQUIRE(file.info.frames > 1200);
    const int32_t ch = (int32_t)file.info.channels;

    EngineFixture fx;
    fx.send(osc_test::message("/b_alloc", 3, (int32_t)1000, ch));
    REQUIRE(fx.waitForDone("/b_alloc"));

    // The client's part: file frames [100, 600) laid out in the lane.
    auto* lane = const_cast<uint8_t*>(fx.engine().guestInbox());
    const uint32_t off = 4096;
    std::memcpy(lane + off, file.samples.data() + (size_t)100 * ch, (size_t)500 * ch * sizeof(float));

    // Into buffer frame 200: 500 frames asked, the buffer's end clamps to 800? No —
    // 1000 - 200 = 800 frames to the end, so all 500 land.
    fx.clearReplies();
    {
        osc_test::Builder b;
        b.begin("/supersonic/buffer/read") << int32_t{3} << int32_t{200} << (int32_t)off << int32_t{500} << ch << 44100.0f;
        fx.send(b.end());
    }
    REQUIRE(fx.waitForDone("/supersonic/buffer/read"));
    const auto got = firstSamples(fx, 3, (200 + 8) * ch);
    for (int32_t i = 0; i < 200 * ch; ++i) CHECK(got[i] == 0.0f);
    for (int32_t i = 0; i < 8 * ch; ++i)
        CHECK(got[200 * ch + i] == Catch::Approx(file.samples[100 * ch + i]).margin(1e-6f));
    fx.clearReplies();
    fx.send(osc_test::message("/b_query", 3));
    OscReply info;
    REQUIRE(fx.waitForReply("/b_info", info));
    CHECK(info.parsed().argFloat(3) == 44100.0f);

    // Clamped to the buffer's end: asked past it, only what fits is copied.
    fx.clearReplies();
    {
        osc_test::Builder b;
        b.begin("/supersonic/buffer/read") << int32_t{3} << int32_t{900} << (int32_t)off << int32_t{500} << ch << 0.0f;
        fx.send(b.end());
    }
    REQUIRE(fx.waitForDone("/supersonic/buffer/read"));
    fx.send(osc_test::message("/b_free", 3));
}

TEST_CASE("/supersonic/buffer/read: a channel mismatch, a range outside the lane and an empty buffer are refused",
          "[load_sample][lane]") {
    EngineFixture fx;
    fx.send(osc_test::message("/b_alloc", 4, (int32_t)100, (int32_t)2));
    REQUIRE(fx.waitForDone("/b_alloc"));
    auto refused = [&](int32_t bufnum, int32_t bufOff, int32_t laneOff, int32_t frames, int32_t ch, const char* expect) {
        fx.clearReplies();
        osc_test::Builder b;
        b.begin("/supersonic/buffer/read") << bufnum << bufOff << laneOff << frames << ch << 0.0f;
        fx.send(b.end());
        OscReply fail;
        REQUIRE(fx.waitForReply("/fail", fail));
        CHECK(fail.parsed().argString(0) == "/supersonic/buffer/read");
        CHECK(fail.parsed().argString(1).find(expect) != std::string::npos);
        CHECK(fail.parsed().argInt(2) == bufnum);
    };
    refused(4, 0, 0, 10, 1, "Channel mismatch");
    refused(4, 0, (int32_t)fx.engine().guestInboxBytes() - 8, 10, 2, "not inside the inbox");
    refused(4, 0, -16, 10, 2, "not inside the inbox");
    refused(5, 0, 0, 10, 2, "not allocated");
    fx.send(osc_test::message("/b_free", 4));
}

TEST_CASE("/supersonic/buffer/publish: the buffer's frames land in the outbox, and the reply says what did",
          "[load_sample][lane]") {
    if (!sampleExists("bd_haus.flac")) { SKIP("Sample not found"); }
    const std::string path = std::string(CLOCKWORK_SAMPLES_DIR) + "/bd_haus.flac";
    const Decoded file = decodeFile(path);
    const int32_t ch = (int32_t)file.info.channels;
    EngineFixture fx;
    REQUIRE(tryAllocRead(fx, 0, "bd_haus.flac"));
    REQUIRE(fx.engine().guestOutbox() != nullptr);

    fx.clearReplies();
    {
        osc_test::Builder b;
        b.begin("/supersonic/buffer/publish") << int32_t{0} << int32_t{100} << int32_t{50} << int32_t{8192};
        fx.send(b.end());
    }
    OscReply pub;
    REQUIRE(fx.waitForReply("/supersonic/buffer/published", pub));
    CHECK(pub.parsed().argInt(0) == 0);
    CHECK(pub.parsed().argInt(1) == ch);
    CHECK(pub.parsed().argInt(2) == 50);
    CHECK(pub.parsed().argFloat(3) == (float)file.info.sample_rate);
    const auto* out = reinterpret_cast<const float*>(fx.engine().guestOutbox() + 8192);
    for (int32_t i = 0; i < 50 * ch; ++i)
        CHECK(out[i] == Catch::Approx(file.samples[100 * ch + i]).margin(1e-6f));

    // To the end (-1), clamped by the buffer; past the end is nothing.
    fx.clearReplies();
    {
        osc_test::Builder b;
        b.begin("/supersonic/buffer/publish") << int32_t{0} << (int32_t)(file.info.frames - 10) << int32_t{-1} << int32_t{8192};
        fx.send(b.end());
    }
    REQUIRE(fx.waitForReply("/supersonic/buffer/published", pub));
    CHECK(pub.parsed().argInt(2) == 10);

    // A range outside the outbox, and a buffer with nothing in it, are refused.
    fx.clearReplies();
    {
        osc_test::Builder b;
        b.begin("/supersonic/buffer/publish") << int32_t{0} << int32_t{0} << int32_t{50} << (int32_t)(fx.engine().guestOutboxBytes() - 8);
        fx.send(b.end());
    }
    OscReply fail;
    REQUIRE(fx.waitForReply("/fail", fail));
    CHECK(fail.parsed().argString(0) == "/supersonic/buffer/publish");
    fx.clearReplies();
    {
        osc_test::Builder b;
        b.begin("/supersonic/buffer/publish") << int32_t{7} << int32_t{0} << int32_t{50} << int32_t{0};
        fx.send(b.end());
    }
    REQUIRE(fx.waitForReply("/fail", fail));
    CHECK(fail.parsed().argInt(2) == 7);
    fx.send(osc_test::message("/b_free", 0));
}

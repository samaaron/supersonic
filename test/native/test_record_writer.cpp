/*
 * test_record_writer.cpp — RecordWriter round-trip: samples pushed through
 * the audio-thread API must land in the file, bit-faithful for float WAV.
 * (Deeper than the engine-level recording tests, which only prove header
 * finalisation — this pins the FIFO/interleave/drain path with real data.)
 */
#include <catch2/catch_test_macros.hpp>
#include "RecordWriter.h"
#include <filesystem>
#include <cmath>

TEST_CASE("RecordWriter round-trips samples to a readable WAV",
          "[recording]") {
    auto tempDir = std::filesystem::temp_directory_path() / "supersonic_test";
    std::filesystem::create_directories(tempDir);
    auto path = (tempDir / "rw_roundtrip.wav").string();

    juce::TimeSliceThread thread{"rw-test-io"};
    thread.startThread();

    constexpr int kChans = 2, kBlock = 128, kBlocks = 40;
    {
        RecordWriter w(path, "wav", 32, 48000.0, kChans, thread, 48000);
        REQUIRE(w.openedOk());

        float left[kBlock], right[kBlock];
        const float* chans[kChans] = { left, right };
        for (int b = 0; b < kBlocks; ++b) {
            for (int i = 0; i < kBlock; ++i) {
                left[i]  = static_cast<float>(b * kBlock + i) / 100000.0f;
                right[i] = -left[i];
            }
            REQUIRE(w.write(chans, kBlock));
        }
    }   // destructor drains + closes

    SF_INFO info{};
    SNDFILE* f = sf_open(path.c_str(), SFM_READ, &info);
    REQUIRE(f != nullptr);
    CHECK(info.channels == kChans);
    CHECK(info.samplerate == 48000);
    REQUIRE(info.frames == kBlock * kBlocks);

    std::vector<float> data(static_cast<size_t>(info.frames) * kChans);
    REQUIRE(sf_readf_float(f, data.data(), info.frames) == info.frames);
    sf_close(f);

    for (int i = 0; i < kBlock * kBlocks; ++i) {
        const float expect = static_cast<float>(i) / 100000.0f;
        REQUIRE(data[static_cast<size_t>(i) * 2]     == expect);
        REQUIRE(data[static_cast<size_t>(i) * 2 + 1] == -expect);
    }

    thread.stopThread(1000);
    std::filesystem::remove_all(tempDir);
}

TEST_CASE("RecordWriter refuses unsupported format combinations",
          "[recording]") {
    auto tempDir = std::filesystem::temp_directory_path() / "supersonic_test";
    std::filesystem::create_directories(tempDir);
    juce::TimeSliceThread thread{"rw-test-io2"};

    RecordWriter flac32((tempDir / "bad.flac").string(), "flac", 32,
                        48000.0, 2, thread, 4096);
    CHECK_FALSE(flac32.openedOk());

    RecordWriter noDir("/nonexistent-dir-xyz/out.wav", "wav", 16,
                       48000.0, 2, thread, 4096);
    CHECK_FALSE(noDir.openedOk());

    std::filesystem::remove_all(tempDir);
}

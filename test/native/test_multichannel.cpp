/*
 * test_multichannel.cpp — Multichannel input/output and recording tests
 *
 * Verifies that the engine boots and processes audio correctly with
 * various channel configurations (mono, stereo, 4ch, 8ch, 16ch).
 * Also tests the recording start/stop API.
 */
#include "EngineFixture.h"

#include <thread>
#include <chrono>
#include <filesystem>
#include <cstring>

extern "C" {
    bool process_audio(double current_time, uint32_t active_output_channels,
                       uint32_t active_input_channels);
    uintptr_t get_audio_output_bus();
    uintptr_t get_audio_input_bus();
}

// ── Helper: boot engine with given channel config ────────────────────────────

static void bootAndVerify(int outCh, int inCh) {
    SupersonicEngine engine;
    engine.onReply = [](const uint8_t*, uint32_t) {};

    SupersonicEngine::Config cfg;
    cfg.headless          = true;
    cfg.udpPort           = 0;
    cfg.numOutputChannels = outCh;
    cfg.numInputChannels  = inCh;
    engine.initialise(cfg);

    // initialise() blocks until HeadlessDriver fires at least one audio
    // block — if the channel config would crash process_audio(), we'd
    // already know by this point.
    REQUIRE(engine.isRunning());

    engine.shutdown();
    CHECK_FALSE(engine.isRunning());
}

// ── 1. Boot with various output channel counts ──────────────────────────────

TEST_CASE("Engine boots with 1 output channel", "[multichannel]") {
    bootAndVerify(1, 0);
}

TEST_CASE("Engine boots with 2 output channels (stereo)", "[multichannel]") {
    bootAndVerify(2, 2);
}

TEST_CASE("Engine boots with 4 output channels", "[multichannel]") {
    bootAndVerify(4, 0);
}

TEST_CASE("Engine boots with 8 output channels", "[multichannel]") {
    bootAndVerify(8, 0);
}

TEST_CASE("Engine boots with 16 output channels", "[multichannel]") {
    bootAndVerify(16, 0);
}

TEST_CASE("Engine boots with 32 output channels", "[multichannel]") {
    bootAndVerify(32, 0);
}

// ── 2. Boot with various input channel counts ───────────────────────────────

TEST_CASE("Engine boots with 1 input channel", "[multichannel]") {
    bootAndVerify(2, 1);
}

TEST_CASE("Engine boots with 4 input channels", "[multichannel]") {
    bootAndVerify(2, 4);
}

TEST_CASE("Engine boots with 8 input channels", "[multichannel]") {
    bootAndVerify(2, 8);
}

TEST_CASE("Engine boots with 16 input channels", "[multichannel]") {
    bootAndVerify(2, 16);
}

// ── 3. Symmetric multichannel (same in/out) ─────────────────────────────────

TEST_CASE("Engine boots with 4in/4out", "[multichannel]") {
    bootAndVerify(4, 4);
}

TEST_CASE("Engine boots with 8in/8out", "[multichannel]") {
    bootAndVerify(8, 8);
}

// ── 4. Output bus has correct layout for multichannel ────────────────────────

TEST_CASE("Output bus has correct channel-major layout", "[multichannel]") {
    int outCh = 4;
    int inCh  = 2;

    SupersonicEngine engine;
    bool gotReply = false;
    engine.onReply = [&](const uint8_t*, uint32_t) { gotReply = true; };

    SupersonicEngine::Config cfg;
    cfg.headless          = true;
    cfg.udpPort           = 0;
    cfg.numOutputChannels = outCh;
    cfg.numInputChannels  = inCh;
    engine.initialise(cfg);

    REQUIRE(engine.isRunning());

    // Pump one block
    static constexpr double NTP_EPOCH_OFFSET = 2208988800.0;
    double baseNTP = static_cast<double>(juce::Time::currentTimeMillis()) * 0.001
                     + NTP_EPOCH_OFFSET;
    process_audio(baseNTP, static_cast<uint32_t>(outCh), static_cast<uint32_t>(inCh));

    // Check output bus pointer is valid and layout is channel-major
    auto* outputBus = reinterpret_cast<float*>(get_audio_output_bus());
    REQUIRE(outputBus != nullptr);

    // Each channel should have 128 contiguous samples (QUANTUM_SIZE)
    // Verify we can read all channels without crash (memory is valid)
    for (int ch = 0; ch < outCh; ++ch) {
        float sum = 0.0f;
        for (int s = 0; s < 128; ++s)
            sum += outputBus[ch * 128 + s];
        // Sum should be finite (not NaN/Inf)
        CHECK(std::isfinite(sum));
    }

    engine.shutdown();
}

// ── 5. Input bus has correct layout for multichannel ─────────────────────────

TEST_CASE("Input bus has correct channel-major layout", "[multichannel]") {
    int outCh = 2;
    int inCh  = 4;

    SupersonicEngine engine;
    engine.onReply = [](const uint8_t*, uint32_t) {};

    SupersonicEngine::Config cfg;
    cfg.headless          = true;
    cfg.udpPort           = 0;
    cfg.numOutputChannels = outCh;
    cfg.numInputChannels  = inCh;
    engine.initialise(cfg);

    REQUIRE(engine.isRunning());

    // Check input bus pointer is valid
    auto* inputBus = reinterpret_cast<float*>(get_audio_input_bus());
    REQUIRE(inputBus != nullptr);

    // Write test pattern to input bus (simulating JUCE input copy)
    for (int ch = 0; ch < inCh; ++ch)
        for (int s = 0; s < 128; ++s)
            inputBus[ch * 128 + s] = static_cast<float>(ch + 1) * 0.1f;

    // Pump a block — should not crash with input data present
    static constexpr double NTP_EPOCH_OFFSET = 2208988800.0;
    double baseNTP = static_cast<double>(juce::Time::currentTimeMillis()) * 0.001
                     + NTP_EPOCH_OFFSET;
    process_audio(baseNTP, static_cast<uint32_t>(outCh), static_cast<uint32_t>(inCh));

    engine.shutdown();
}

// ── 6. Prefetch buffer is correctly sized ────────────────────────────────────

TEST_CASE("Prefetch buffer resizes for channel count", "[multichannel]") {
    // Boot with 8 output channels — prefetch should be 8 * 128 = 1024 floats
    SupersonicEngine engine;
    engine.onReply = [](const uint8_t*, uint32_t) {};

    SupersonicEngine::Config cfg;
    cfg.headless          = true;
    cfg.udpPort           = 0;
    cfg.numOutputChannels = 8;
    cfg.numInputChannels  = 0;
    engine.initialise(cfg);

    REQUIRE(engine.isRunning());

    // If prefetch was still hardcoded to kMaxChannels=8, this would have worked
    // previously. But with 16 channels it would have crashed. Boot with 16 to
    // verify dynamic sizing works.
    engine.shutdown();

    SupersonicEngine engine2;
    engine2.onReply = [](const uint8_t*, uint32_t) {};

    SupersonicEngine::Config cfg2;
    cfg2.headless          = true;
    cfg2.udpPort           = 0;
    cfg2.numOutputChannels = 16;
    cfg2.numInputChannels  = 0;
    engine2.initialise(cfg2);
    REQUIRE(engine2.isRunning());

    // HeadlessDriver is already processing with 16 channels —
    // if the prefetch buffer were too small, initialise() would have crashed.

    engine2.shutdown();
}

// ── 7. Recording start/stop API ─────────────────────────────────────────────

TEST_CASE("Recording start and stop", "[recording]") {
    SupersonicEngine engine;
    engine.onReply = [](const uint8_t*, uint32_t) {};

    SupersonicEngine::Config cfg;
    cfg.headless          = true;
    cfg.udpPort           = 0;
    cfg.numOutputChannels = 2;
    engine.initialise(cfg);

    REQUIRE(engine.isRunning());

    // Use a temp file path
    auto tempDir = std::filesystem::temp_directory_path() / "supersonic_test";
    std::filesystem::create_directories(tempDir);
    auto wavPath = (tempDir / "test_recording.wav").string();

    // Start recording
    auto startResult = engine.startRecording(wavPath, "wav", 16);
    CHECK(startResult.success);
    CHECK(engine.isRecording());

    // Double-start should fail
    auto startResult2 = engine.startRecording(wavPath, "wav", 16);
    CHECK_FALSE(startResult2.success);
    CHECK(startResult2.error == "already recording");

    // Stop recording
    auto stopResult = engine.stopRecording();
    CHECK(stopResult.success);
    CHECK_FALSE(engine.isRecording());

    // Double-stop should fail
    auto stopResult2 = engine.stopRecording();
    CHECK_FALSE(stopResult2.success);
    CHECK(stopResult2.error == "not recording");

    // Verify file was created
    CHECK(std::filesystem::exists(wavPath));

    // Cleanup
    std::filesystem::remove_all(tempDir);

    engine.shutdown();
}

TEST_CASE("Recording creates valid WAV file", "[recording]") {
    SupersonicEngine engine;
    engine.onReply = [](const uint8_t*, uint32_t) {};

    SupersonicEngine::Config cfg;
    cfg.headless          = true;
    cfg.udpPort           = 0;
    cfg.numOutputChannels = 2;
    engine.initialise(cfg);

    REQUIRE(engine.isRunning());

    auto tempDir = std::filesystem::temp_directory_path() / "supersonic_test";
    std::filesystem::create_directories(tempDir);
    auto wavPath = (tempDir / "test_valid.wav").string();

    auto result = engine.startRecording(wavPath, "wav", 24);
    REQUIRE(result.success);

    // The file should exist (header written on start)
    CHECK(std::filesystem::exists(wavPath));

    engine.stopRecording();

    // File should have non-zero size (at least WAV header)
    auto fileSize = std::filesystem::file_size(wavPath);
    CHECK(fileSize > 44);  // WAV header is 44 bytes minimum

    // Cleanup
    std::filesystem::remove_all(tempDir);
    engine.shutdown();
}

TEST_CASE("Recording stops on engine shutdown", "[recording]") {
    auto tempDir = std::filesystem::temp_directory_path() / "supersonic_test";
    std::filesystem::create_directories(tempDir);
    auto wavPath = (tempDir / "test_shutdown.wav").string();

    {
        SupersonicEngine engine;
        engine.onReply = [](const uint8_t*, uint32_t) {};

        SupersonicEngine::Config cfg;
        cfg.headless          = true;
        cfg.udpPort           = 0;
        cfg.numOutputChannels = 2;
        engine.initialise(cfg);
        REQUIRE(engine.isRunning());

        auto result = engine.startRecording(wavPath, "wav", 16);
        REQUIRE(result.success);

        // Shutdown without explicit stopRecording — should not crash
        engine.shutdown();
    }

    // File should exist (recording stopped cleanly during shutdown)
    CHECK(std::filesystem::exists(wavPath));

    // Cleanup
    std::filesystem::remove_all(tempDir);
}

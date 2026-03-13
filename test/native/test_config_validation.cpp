/*
 * test_config_validation.cpp — SupersonicEngine::Config edge cases
 *
 * Tests that the engine boots and shuts down cleanly with various Config
 * parameter combinations, and verifies default values are correct.
 */
#include "EngineFixture.h"

#include <thread>
#include <chrono>

// ── 1. Default config values ────────────────────────────────────────────────

TEST_CASE("Default Config values are correct", "[config]") {
    SupersonicEngine::Config cfg;

    CHECK(cfg.sampleRate             == 48000);
    CHECK(cfg.bufferSize             == 0);   // 0 = auto (smallest multiple of 128)
    CHECK(cfg.udpPort                == 57110);
    CHECK(cfg.preschedulerLookaheadS == 0.500);
    CHECK(cfg.maxNodes               == 1024);
    CHECK(cfg.numBuffers             == 1024);
    CHECK(cfg.numOutputChannels      == 2);
    CHECK(cfg.numInputChannels       == 2);
    CHECK(cfg.maxGraphDefs           == 512);
    CHECK(cfg.maxWireBufs            == 64);
    CHECK(cfg.numControlBusChannels  == 16384);
    CHECK(cfg.realTimeMemorySize     == 8192);
    CHECK(cfg.numRGens               == 64);
    CHECK(cfg.headless               == false);
}

// ── 2. Minimum viable config ────────────────────────────────────────────────

TEST_CASE("Engine boots with minimum viable config", "[config]") {
    SupersonicEngine engine;
    bool gotReply = false;
    engine.onReply = [&](const uint8_t*, uint32_t) { gotReply = true; };

    SupersonicEngine::Config cfg;
    cfg.headless    = true;
    cfg.udpPort     = 0;
    cfg.maxNodes    = 4;
    cfg.numBuffers  = 4;
    cfg.maxGraphDefs = 4;
    cfg.maxWireBufs  = 4;
    cfg.numRGens     = 4;
    engine.initialise(cfg);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    CHECK(engine.isRunning());
    engine.shutdown();
    CHECK_FALSE(engine.isRunning());
}

// ── 3. Large config values ──────────────────────────────────────────────────

TEST_CASE("Engine boots with large config values", "[config]") {
    SupersonicEngine engine;
    engine.onReply = [](const uint8_t*, uint32_t) {};

    SupersonicEngine::Config cfg;
    cfg.headless   = true;
    cfg.udpPort    = 0;
    cfg.maxNodes   = 4096;
    cfg.numBuffers = 4096;
    engine.initialise(cfg);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    CHECK(engine.isRunning());
    engine.shutdown();
    CHECK_FALSE(engine.isRunning());
}

// ── 4. Sample rate 44100 ────────────────────────────────────────────────────

TEST_CASE("Engine boots at 44100 Hz", "[config]") {
    SupersonicEngine engine;
    engine.onReply = [](const uint8_t*, uint32_t) {};

    SupersonicEngine::Config cfg;
    cfg.headless   = true;
    cfg.udpPort    = 0;
    cfg.sampleRate = 44100;
    engine.initialise(cfg);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    CHECK(engine.isRunning());
    engine.shutdown();
    CHECK_FALSE(engine.isRunning());
}

TEST_CASE("Engine boots at 96000 Hz", "[config]") {
    SupersonicEngine engine;
    engine.onReply = [](const uint8_t*, uint32_t) {};

    SupersonicEngine::Config cfg;
    cfg.headless   = true;
    cfg.udpPort    = 0;
    cfg.sampleRate = 96000;
    engine.initialise(cfg);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    CHECK(engine.isRunning());
    engine.shutdown();
    CHECK_FALSE(engine.isRunning());
}

// ── 5. Buffer sizes ─────────────────────────────────────────────────────────

TEST_CASE("Engine boots with bufferSize=64", "[config]") {
    SupersonicEngine engine;
    engine.onReply = [](const uint8_t*, uint32_t) {};

    SupersonicEngine::Config cfg;
    cfg.headless   = true;
    cfg.udpPort    = 0;
    cfg.bufferSize = 64;
    engine.initialise(cfg);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    CHECK(engine.isRunning());
    engine.shutdown();
    CHECK_FALSE(engine.isRunning());
}

TEST_CASE("Engine boots with bufferSize=256", "[config]") {
    SupersonicEngine engine;
    engine.onReply = [](const uint8_t*, uint32_t) {};

    SupersonicEngine::Config cfg;
    cfg.headless   = true;
    cfg.udpPort    = 0;
    cfg.bufferSize = 256;
    engine.initialise(cfg);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    CHECK(engine.isRunning());
    engine.shutdown();
    CHECK_FALSE(engine.isRunning());
}

TEST_CASE("Engine boots with bufferSize=512", "[config]") {
    SupersonicEngine engine;
    engine.onReply = [](const uint8_t*, uint32_t) {};

    SupersonicEngine::Config cfg;
    cfg.headless   = true;
    cfg.udpPort    = 0;
    cfg.bufferSize = 512;
    engine.initialise(cfg);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    CHECK(engine.isRunning());
    engine.shutdown();
    CHECK_FALSE(engine.isRunning());
}

// ── 6. headless=true (default test mode) ────────────────────────────────────

TEST_CASE("headless=true skips audio device", "[config]") {
    EngineFixture fx;

    // Engine is running in headless mode — verify it responds to /status
    fx.send(osc_test::message("/status"));
    OscReply r;
    REQUIRE(fx.waitForReply("/status.reply", r));

    auto p = r.parsed();
    CHECK(p.argCount() >= 5);
}

// ── 7. udpPort=0 disables UDP listener ──────────────────────────────────────

TEST_CASE("udpPort=0 disables UDP listener", "[config]") {
    SupersonicEngine engine;
    engine.onReply = [](const uint8_t*, uint32_t) {};

    SupersonicEngine::Config cfg;
    cfg.headless = true;
    cfg.udpPort  = 0;
    engine.initialise(cfg);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Engine should still be running, just without UDP
    CHECK(engine.isRunning());

    engine.shutdown();
    CHECK_FALSE(engine.isRunning());
}

// ── 8. Mono output (numOutputChannels=1) ────────────────────────────────────

TEST_CASE("Engine boots with mono output", "[config]") {
    SupersonicEngine engine;
    engine.onReply = [](const uint8_t*, uint32_t) {};

    SupersonicEngine::Config cfg;
    cfg.headless          = true;
    cfg.udpPort           = 0;
    cfg.numOutputChannels = 1;
    engine.initialise(cfg);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    CHECK(engine.isRunning());
    engine.shutdown();
    CHECK_FALSE(engine.isRunning());
}

// ── 9. No input channels ───────────────────────────────────────────────────

TEST_CASE("Engine boots with zero input channels", "[config]") {
    SupersonicEngine engine;
    engine.onReply = [](const uint8_t*, uint32_t) {};

    SupersonicEngine::Config cfg;
    cfg.headless         = true;
    cfg.udpPort          = 0;
    cfg.numInputChannels = 0;
    engine.initialise(cfg);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    CHECK(engine.isRunning());
    engine.shutdown();
    CHECK_FALSE(engine.isRunning());
}

// ── 10. Prescheduler lookahead variations ───────────────────────────────────

TEST_CASE("Engine boots with short prescheduler lookahead", "[config]") {
    SupersonicEngine engine;
    engine.onReply = [](const uint8_t*, uint32_t) {};

    SupersonicEngine::Config cfg;
    cfg.headless               = true;
    cfg.udpPort                = 0;
    cfg.preschedulerLookaheadS = 0.050;
    engine.initialise(cfg);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    CHECK(engine.isRunning());
    engine.shutdown();
    CHECK_FALSE(engine.isRunning());
}

TEST_CASE("Engine boots with long prescheduler lookahead", "[config]") {
    SupersonicEngine engine;
    engine.onReply = [](const uint8_t*, uint32_t) {};

    SupersonicEngine::Config cfg;
    cfg.headless               = true;
    cfg.udpPort                = 0;
    cfg.preschedulerLookaheadS = 2.0;
    engine.initialise(cfg);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    CHECK(engine.isRunning());
    engine.shutdown();
    CHECK_FALSE(engine.isRunning());
}

// ── 11. numControlBusChannels variations ────────────────────────────────────

TEST_CASE("Engine boots with small numControlBusChannels", "[config]") {
    SupersonicEngine engine;
    engine.onReply = [](const uint8_t*, uint32_t) {};

    SupersonicEngine::Config cfg;
    cfg.headless              = true;
    cfg.udpPort               = 0;
    cfg.numControlBusChannels = 128;
    engine.initialise(cfg);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    CHECK(engine.isRunning());
    engine.shutdown();
    CHECK_FALSE(engine.isRunning());
}

// ── 12. realTimeMemorySize variations ───────────────────────────────────────

TEST_CASE("Engine boots with small realTimeMemorySize", "[config]") {
    SupersonicEngine engine;
    engine.onReply = [](const uint8_t*, uint32_t) {};

    SupersonicEngine::Config cfg;
    cfg.headless           = true;
    cfg.udpPort            = 0;
    cfg.realTimeMemorySize = 256;
    engine.initialise(cfg);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    CHECK(engine.isRunning());
    engine.shutdown();
    CHECK_FALSE(engine.isRunning());
}

TEST_CASE("Engine boots with large realTimeMemorySize", "[config]") {
    SupersonicEngine engine;
    engine.onReply = [](const uint8_t*, uint32_t) {};

    SupersonicEngine::Config cfg;
    cfg.headless           = true;
    cfg.udpPort            = 0;
    cfg.realTimeMemorySize = 32768;
    engine.initialise(cfg);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    CHECK(engine.isRunning());
    engine.shutdown();
    CHECK_FALSE(engine.isRunning());
}

/*
 * test_engine_state.cpp — EngineState lifecycle tests.
 *
 * Verifies the engine state machine: Stopped → Booting → Running → Stopped.
 */
#include <catch2/catch_test_macros.hpp>
#include "EngineFixture.h"
#include "src/engine_state.h"

// ── State transitions ──────────────────────────────────────────────────────

TEST_CASE("EngineState: starts Stopped before initialise", "[EngineState]") {
    SupersonicEngine engine;
    CHECK(engine.engineState() == EngineState::Stopped);
}

TEST_CASE("EngineState: Running after initialise", "[EngineState]") {
    SupersonicEngine engine;
    engine.onReply = [](const uint8_t*, uint32_t) {};
    SupersonicEngine::Config cfg;
    cfg.headless = true;
    cfg.udpPort  = 0;
    engine.initialise(cfg);
    CHECK(engine.engineState() == EngineState::Running);
    engine.shutdown();
}

TEST_CASE("EngineState: Stopped after shutdown", "[EngineState]") {
    SupersonicEngine engine;
    engine.onReply = [](const uint8_t*, uint32_t) {};
    SupersonicEngine::Config cfg;
    cfg.headless = true;
    cfg.udpPort  = 0;
    engine.initialise(cfg);
    REQUIRE(engine.engineState() == EngineState::Running);
    engine.shutdown();
    CHECK(engine.engineState() == EngineState::Stopped);
}

// ── engineStateToString ─────────────────────────────────────────────────────

TEST_CASE("EngineState: engineStateToString covers all states", "[EngineState]") {
    CHECK(std::string(engineStateToString(EngineState::Booting))    == "booting");
    CHECK(std::string(engineStateToString(EngineState::Running))    == "running");
    CHECK(std::string(engineStateToString(EngineState::Restarting)) == "restarting");
    CHECK(std::string(engineStateToString(EngineState::Stopped))    == "stopped");
    CHECK(std::string(engineStateToString(EngineState::Error))      == "error");
}

// ── Fixture-based: verify engine is Running and responds after boot ─────────

TEST_CASE("EngineState: fixture engine is Running", "[EngineState]") {
    EngineFixture fix;
    CHECK(fix.engine().engineState() == EngineState::Running);

    // Verify it actually works
    fix.send(osc_test::message("/status"));
    OscReply r;
    REQUIRE(fix.waitForReply("/status.reply", r));
}

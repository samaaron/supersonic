/*
 * test_scsynth_heap.cpp — scsynth's real-time pool, carved from the engine's
 * heap.
 *
 * What only the scsynth guest shows: its pool comes out of the engine's heap,
 * so a pool the heap cannot hold must stop the boot with scsynth's own reason,
 * and a heap sized for the pool must boot it. The engine's own state machine
 * is Clockwork's to test (clockwork/test/test_engine_state.cpp).
 */
#include <catch2/catch_test_macros.hpp>
#include "EngineFixture.h"
#include "engine_state.h"
#include "scsynth_options.h"   // scsynth_heap_bytes

// Regression: Sonic Pi asks for a 128 MB pool (-m 131072). scsynth takes it
// from the engine's heap, 64 MB by default, so World_New failed — and the
// engine went on "running" with no World in it, the reason that reached a
// client being "World_New returned null". Now a pool the heap cannot hold is
// an engine in error with scsynth's own reason, and a heap sized for the pool
// (scsynth_heap_bytes — what the native host derives from -m) boots it.

TEST_CASE("ScsynthHeap: a pool larger than the heap is an error carrying scsynth's reason",
          "[ScsynthHeap]") {
    auto cfg = EngineFixture::defaultConfig();
    setGuestOption(cfg, "realTimeMemorySize", 131072);
    EngineFixture fix(cfg);
    CHECK(fix.engine().engineState() == EngineState::Error);

    fix.send(osc_test::message("/clockwork/notify"));
    OscReply ack, state;
    REQUIRE(fix.waitForReply("/clockwork/notify.reply", ack));
    REQUIRE(fix.waitForReply("/clockwork/statechange", state));
    const auto s = state.parsed();
    REQUIRE(s.argCount() >= 2);
    CHECK(s.argString(0) == "error");
    INFO("reason: " << s.argString(1));
    CHECK(s.argString(1).find("RT pool of 134217728 bytes") != std::string::npos);
}

TEST_CASE("ScsynthHeap: a heap sized for the pool boots scsynth", "[ScsynthHeap]") {
    auto cfg = EngineFixture::defaultConfig();
    setGuestOption(cfg, "realTimeMemorySize", 131072);
    cfg.heapBytes = scsynth_heap_bytes(131072);
    EngineFixture fix(cfg);
    CHECK(fix.engine().engineState() == EngineState::Running);
    fix.send(osc_test::message("/status"));
    OscReply r;
    REQUIRE(fix.waitForReply("/status.reply", r));
}

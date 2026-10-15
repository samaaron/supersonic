/*
 * test_scsynth_heap.cpp — scsynth's real-time pool, in the memory the engine
 * sizes for it.
 *
 * What only the scsynth guest shows: its pool is laid out in its fast
 * memory, and the guest says how much that takes (dsp_memory_bytes). The
 * engine's side of it — a span the system cannot provide, a guest that
 * declares nothing — is Clockwork's to test
 * (clockwork/test/test_engine_state.cpp).
 */
#include <catch2/catch_test_macros.hpp>
#include "EngineFixture.h"
#include "engine_state.h"

// Regression: Sonic Pi asks for a 128 MB pool (-m 131072). scsynth took it
// from the engine's heap, 64 MB by default, so World_New failed and scsynth
// never started. scsynth now says what memory its configuration needs
// (dsp_memory_bytes, by scsynth_memory_bytes), and the engine gives a span
// that holds it on every host: a native server, the NIF, or anything else
// embedding the engine, with no host left to size it.
TEST_CASE("ScsynthHeap: scsynth's pool is held by the memory the engine sizes for it", "[ScsynthHeap]") {
    auto cfg = EngineFixture::defaultConfig();
    setGuestOption(cfg, "realTimeMemorySize", 131072);
    EngineFixture fix(cfg);
    CHECK(fix.engine().engineState() == EngineState::Running);
    fix.send(osc_test::message("/status"));
    OscReply r;
    REQUIRE(fix.waitForReply("/status.reply", r));
}

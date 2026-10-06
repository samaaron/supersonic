/*
 * test_clear_sched.cpp — /clearSched drops the bundles scheduled ahead.
 *
 * scsynth's /clearSched empties its scheduler. SuperSonic's scsynth has none:
 * Clockwork's timed store holds the bundles, and /clearSched reached scsynth's
 * empty stub (SC_CoreAudio.h, ClearSched) and did nothing. Sonic Pi clears
 * with it on Stop and when the engine rebuilds after a device change, so the
 * notes it had already sent ahead (its schedule-ahead, about half a second)
 * still played: after Stop, and into the empty engine after a cold swap,
 * where they failed ("SynthDef not found", seen 2026-10-05 16:31).
 *
 * The case is the clock (manual pump, freewheel): nothing comes due unless the
 * case renders it, so the moment a bundle falls due is exact however busy the
 * machine. The bundle is dated by the wall clock, as a client dates it, and
 * the case renders ten seconds, well past it however long the boot took.
 */
#include "EngineFixture.h"
#include "OscBuilder.h"
#include "clock/clock_math.h"   // wallClockNTP

namespace {

// Blocks of 128 samples at 48 kHz.
uint32_t blocksForMs(int ms) {
    return static_cast<uint32_t>(static_cast<int64_t>(ms) * 48000 / 1000 / 128);
}

ClockworkEngine::Config theCaseKeepsTheClock() {
    auto cfg = EngineFixture::defaultConfig();
    cfg.manualAudioPump = true;
    cfg.freewheelClock  = true;
    return cfg;
}

// A note sent half a second ahead, as Sonic Pi sends its notes.
void beepAhead(EngineFixture& fx, int32_t node) {
    fx.engine().sendBundle(wallClockNTP() + 0.5, {
        OscBuilder::message("/s_new", "sonic-pi-beep", node, 0, 0, "release", 0.01f)
    });
}

}  // namespace

TEST_CASE("ClearSched: a bundle scheduled ahead plays when nothing clears it",
          "[osc][scheduling]") {
    EngineFixture fx(theCaseKeepsTheClock());
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));
    fx.send(osc_test::message("/notify", 1));
    fx.clearReplies();

    beepAhead(fx, 1001);
    fx.pumpBlock(blocksForMs(10000));
    OscReply started;
    CHECK(fx.waitForReply("/n_go", started, 200));
}

TEST_CASE("ClearSched: /clearSched drops the bundles scheduled ahead",
          "[osc][scheduling]") {
    EngineFixture fx(theCaseKeepsTheClock());
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));
    fx.send(osc_test::message("/notify", 1));
    fx.clearReplies();

    beepAhead(fx, 1002);
    fx.send(osc_test::message("/clearSched"));
    fx.pumpBlock(blocksForMs(10000));
    OscReply started;
    INFO(fx.debugMessagesDump());
    CHECK_FALSE(fx.waitForReply("/n_go", started, 200));
}

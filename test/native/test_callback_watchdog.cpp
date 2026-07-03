/*
 * test_callback_watchdog.cpp — Callback-starvation watchdog.
 *
 * Guards the deaf-server failure mode: the audio source's thread stops
 * delivering callbacks (spinning inside the OS layer or dead) while the
 * engine still believes the source is active. Synth commands are drained
 * only by process_audio on the audio thread, so a stalled source turns the
 * whole server deaf — commands pile up in the IN ring forever — while the
 * control socket stays superficially alive.
 *
 * The watchdog samples JuceAudioCallback::processCount; when it freezes for
 * longer than the stall window (and no swap/reopen is in flight) it must
 * restart the audio source, after which queued commands drain and the
 * engine answers again.
 */
#include <catch2/catch_test_macros.hpp>
#include "EngineFixture.h"
#include "OscBuilder.h"
#include <chrono>
#include <mutex>
#include <thread>

namespace {

SupersonicEngine::Config watchdogConfig() {
    auto cfg = EngineFixture::defaultConfig();
    cfg.callbackWatchdog = true;
    cfg.watchdogStallMs  = 250;  // fast for tests; production default is much larger
    cfg.watchdogPollMs   = 50;
    return cfg;
}

} // namespace

TEST_CASE("Watchdog: silent during normal operation", "[Watchdog]") {
    EngineFixture fix(watchdogConfig());

    // Tick healthily across several stall windows — the watchdog must not fire.
    REQUIRE(fix.waitForBlocks(200, 3000));
    REQUIRE(fix.engine().watchdogRecoveryCount() == 0);

    OscReply r;
    fix.send(osc_test::message("/status"));
    REQUIRE(fix.waitForReply("/status.reply", r));
}

TEST_CASE("Watchdog: restarts a stalled audio source and the engine answers again",
          "[Watchdog]") {
    EngineFixture fix(watchdogConfig());

    // Kill the driver thread behind the engine's back: mActiveSource stays
    // Headless — exactly the wedge state (source believed live, no ticks).
    fix.stopHeadlessDriver();

    // Deaf: the command lands in the IN ring with nothing draining it.
    OscReply r;
    fix.send(osc_test::message("/status"));
    REQUIRE_FALSE(fix.waitForReply("/status.reply", r, 200));

    // The watchdog must notice the frozen processCount and restart the source…
    REQUIRE(fix.pollUntil([&] {
        return fix.engine().watchdogRecoveryCount() >= 1;
    }, 5000));

    // …after which the queued command drains and the engine answers.
    REQUIRE(fix.waitForReply("/status.reply", r, 2000));
    REQUIRE(fix.engine().audioSource()
            == SupersonicEngine::AudioSource::Headless);
}

TEST_CASE("Watchdog: holds fire while a device swap is in flight", "[Watchdog]") {
    EngineFixture fix(watchdogConfig());

    // Simulate a swap in flight, then stall the source underneath it.
    auto gate = fix.engine().testHoldSwapGate();
    fix.stopHeadlessDriver();

    // Stalled far past the window, but the gate is held — no recovery.
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    REQUIRE(fix.engine().watchdogRecoveryCount() == 0);

    // Swap "finishes" — recovery may now proceed.
    gate.unlock();
    REQUIRE(fix.pollUntil([&] {
        return fix.engine().watchdogRecoveryCount() >= 1;
    }, 5000));

    OscReply r;
    fix.send(osc_test::message("/status"));
    REQUIRE(fix.waitForReply("/status.reply", r, 2000));
}

TEST_CASE("SwapGate: bounded acquisition fails while held, succeeds after release",
          "[Watchdog][SwapGate]") {
    // The primitive setDeviceMode's system-default reinit uses to serialise
    // against in-flight swaps (the unguarded interleave was the wedge's root
    // cause; the JUCE-manager race itself needs real devices, so the gate is
    // what's testable headlessly).
    EngineFixture fix;  // watchdog off

    auto held = fix.engine().testHoldSwapGate();
    std::unique_lock<std::mutex> lk;
    REQUIRE_FALSE(fix.engine().tryAcquireSwapGate(lk, 3, 10));
    REQUIRE_FALSE(lk.owns_lock());

    held.unlock();
    REQUIRE(fix.engine().tryAcquireSwapGate(lk, 3, 10));
    REQUIRE(lk.owns_lock());
}

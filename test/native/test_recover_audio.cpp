//--
// This file is part of Sonic Pi: http://sonic-pi.net
// Full project source: https://github.com/samaaron/sonic-pi
// License: https://github.com/samaaron/sonic-pi/blob/main/LICENSE.md
//
// Copyright 2026 by Sam Aaron (http://sam.aaron.name).
// All rights reserved.
//
// Permission is granted for use, copying, modification, and
// distribution of modified versions of this work as long as this
// notice is included.
//++

// Tests for the post-hibernate audio recovery (recoverAudio). recoverAudio runs
// on a dedicated worker thread (requestAudioRecovery launches it), so these just
// kick off a recovery and wait for its /supersonic/devices/reopen.done — no
// message-loop pump, which is why they run on every platform. Two invariants are
// exercised here:
//   * the mDeviceManager reset() must not race concurrent device queries
//     ([race], caught by the TSan build), and
//   * a recovery must never strand the engine with no audio source.

#include <catch2/catch_test_macros.hpp>
#include "EngineFixture.h"
#include <atomic>
#include <chrono>
#include <thread>

namespace {

SupersonicEngine::Config recoverConfig() {
    auto cfg = EngineFixture::defaultConfig();
    // Non-manual-pump: a real audio source (headless driver) actually runs, so
    // audioSource() is meaningful and recoverAudio's stop/start hits real state.
    cfg.callbackWatchdog = true;
    cfg.watchdogStallMs  = 250;
    cfg.watchdogPollMs   = 50;
    return cfg;
}

} // namespace

// Sanity/enabler: requestAudioRecovery launches recoverAudio on its worker
// thread, which always ends by broadcasting /supersonic/devices/reopen.done.
// If this fails, the harness can't drive recovery and the tests below are moot.
TEST_CASE("Recover: requestAudioRecovery runs recoverAudio and reports reopen.done",
          "[Recover]") {
    EngineFixture fix(recoverConfig());

    std::string reason;
    REQUIRE(fix.engine().requestAudioRecovery(reason));
    REQUIRE(reason == "started");

    OscReply r;
    REQUIRE(fix.waitForReply("/supersonic/devices/reopen.done", r, 5000));
}

// Use-after-free guard: recoverAudio -> recreateDeviceManager resets
// mDeviceManager, while device queries (currentDevice / listDevices /
// currentDriver / listDrivers — run on the NRT control thread and the
// debounce-switch thread in production) dereference mDeviceManager from another
// thread. Reproduce that shape here. Machine-independent: the reset() happens
// regardless of whether a device opens. Under the TSan build this reports the
// data race if the swap gate doesn't serialise the reset against the readers;
// on a normal build it is a latent UAF.
TEST_CASE("Recover: mDeviceManager reset races concurrent device queries",
          "[Recover][race]") {
    EngineFixture fix(recoverConfig());

    // Every public reader that dereferences mDeviceManager must take the swap
    // gate, else recreateDeviceManager()'s reset() races it. Each reader runs in
    // its OWN thread: a gated reader BLOCKS while recovery holds the gate, so a
    // single loop mixing device and driver readers would let the (gated) device
    // calls fence the driver calls out of the reset window and mask the race.
    std::atomic<bool> stop{false};
    std::thread deviceReader([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            (void)fix.engine().currentDevice();
            (void)fix.engine().listDevices(false);
        }
    });
    std::thread driverReader([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            (void)fix.engine().currentDriver();
            (void)fix.engine().listDrivers();
        }
    });

    // A single recovery (one mDeviceManager.reset()) racing the reader is enough
    // for TSan to flag it; do a couple, each waited to completion then spaced
    // past the 3s recovery cooldown.
    for (int i = 0; i < 2; ++i) {
        std::string reason;
        fix.engine().requestAudioRecovery(reason);   // launches the worker
        OscReply r;
        fix.waitForReply("/supersonic/devices/reopen.done", r, 3000);
        std::this_thread::sleep_for(std::chrono::milliseconds(3200));
    }

    stop.store(true, std::memory_order_relaxed);
    deviceReader.join();
    driverReader.join();
    SUCCEED();   // TSan is the oracle; assertion-free by design
}

// A recovery that cannot open a real device must not leave this explicit-headless
// fixture sourceless: recoverAudio() calls startAudioSource(), which restarts the
// headless driver (or promotes to a real device on a box with hardware), so
// audioSource() is never None afterwards. (A non-headless build parks at None by
// design and the watchdog keeps retrying — see test_no_audio_device.)
TEST_CASE("Recover: recovery never strands the engine with no audio source",
          "[Recover]") {
    EngineFixture fix(recoverConfig());
    REQUIRE(fix.engine().audioSource() != SupersonicEngine::AudioSource::None);

    std::string reason;
    fix.engine().requestAudioRecovery(reason);

    OscReply r;
    REQUIRE(fix.waitForReply("/supersonic/devices/reopen.done", r, 5000));
    CHECK(fix.engine().audioSource() != SupersonicEngine::AudioSource::None);
}

/*
 * test_swap_characterization.cpp — pins the device swap subsystem's CURRENT
 * behaviour against fake devices (FakeAudioDevice.h), warts included.
 *
 * These are characterization tests in the Feathers sense: they are the
 * safety net under the switchDevice/enableInputChannels/switchDriver/
 * recovery refactor. When a later phase changes one of these behaviours
 * ON PURPOSE, update the pinned expectation in the same commit and say
 * why in its message. A surprise failure here means the refactor changed
 * behaviour it didn't mean to.
 *
 * Everything runs against a real juce::AudioDeviceManager whose only
 * device types are fakes — manager logic (name validation, setup
 * resolution, type switching) is JUCE's own. First suite in the repo to
 * exercise these paths off-hardware; before the deviceManagerFactory
 * seam every engine device test ran with a null manager.
 */
#include <catch2/catch_test_macros.hpp>
#include "EngineFixture.h"
#include "FakeAudioDevice.h"
#include "OscBuilder.h"

using fake_audio::fakeEngineConfig;
using fake_audio::makeFactory;
using fake_audio::makeSimpleSystem;
using fake_audio::FakeDeviceSpec;
using fake_audio::FakeSystem;

// ── Boot ────────────────────────────────────────────────────────────────────

TEST_CASE("Swap: -H boot opens the named fake device and ticks",
          "[SwapChar][boot]") {
    auto sys = makeSimpleSystem();
    EngineFixture fix(fakeEngineConfig(sys, "Fake Speakers"));

    auto cur = fix.engine().currentDevice();
    REQUIRE(cur.name == "Fake Speakers");

    // The engine is live: OSC round-trips via real fake-device callbacks.
    OscReply reply;
    fix.send(osc_test::message("/status"));
    REQUIRE(fix.waitForReply("/status.reply", reply));
}

// ── Named device switch ─────────────────────────────────────────────────────

TEST_CASE("Swap: named same-driver switch succeeds and reports the device",
          "[SwapChar]") {
    auto sys = makeSimpleSystem();
    EngineFixture fix(fakeEngineConfig(sys, "Fake Speakers"));

    auto r = fix.engine().switchDevice("Fake Interface", 0, 0, false, "__none__");
    REQUIRE(r.success);
    REQUIRE(r.deviceName == "Fake Interface");
    REQUIRE(fix.engine().currentDevice().name == "Fake Interface");
}

TEST_CASE("Swap: unknown output name is refused before any mutation",
          "[SwapChar]") {
    auto sys = makeSimpleSystem();
    EngineFixture fix(fakeEngineConfig(sys, "Fake Speakers"));

    auto r = fix.engine().switchDevice("Ghost Device", 0, 0, false, "__none__");
    REQUIRE(!r.success);
    REQUIRE(!r.error.empty());
    // The device we were on is untouched.
    REQUIRE(fix.engine().currentDevice().name == "Fake Speakers");
}

TEST_CASE("Swap: rate change forces a cold swap at the requested rate",
          "[SwapChar]") {
    auto sys = makeSimpleSystem();
    EngineFixture fix(fakeEngineConfig(sys, "Fake Speakers"));

    auto r = fix.engine().switchDevice("", 44100, 0, false, "__none__");
    REQUIRE(r.success);
    REQUIRE(r.type == SwapType::Cold);
    REQUIRE(r.sampleRate == 44100.0);

    // Engine still responsive after the world rebuild.
    OscReply reply;
    fix.send(osc_test::message("/status"));
    REQUIRE(fix.waitForReply("/status.reply", reply));
}

// ── Input enable / disable round-trip ───────────────────────────────────────

TEST_CASE("Swap: inputs enable via saved device name, disable via __none__",
          "[SwapChar][inputs]") {
    auto sys = makeSimpleSystem();
    EngineFixture fix(fakeEngineConfig(sys, "Fake Speakers"));

    // Seed mLastInputDeviceName with an explicit full-duplex switch.
    auto r = fix.engine().switchDevice("Fake Interface", 0, 0, false,
                                       "Fake Interface");
    REQUIRE(r.success);
    REQUIRE(r.inputDeviceName == "Fake Interface");

    // Disable...
    auto off = fix.engine().enableInputChannels(0);
    REQUIRE(off.success);

    // ...and re-enable through the saved name — no live CoreAudio needed.
    auto on = fix.engine().enableInputChannels(2);
    REQUIRE(on.success);
    REQUIRE(on.inputDeviceName == "Fake Interface");
}

// ── Input open failure degrades to output-only ──────────────────────────────

TEST_CASE("Swap: input-side open failure keeps the output and flags the input",
          "[SwapChar][inputs]") {
    auto sys = makeSimpleSystem();
    EngineFixture fix(fakeEngineConfig(sys, "Fake Speakers"));

    sys->device("Fake Microphone")->failInputOpen = true;

    auto r = fix.engine().switchDevice("Fake Speakers", 0, 0, false,
                                       "Fake Microphone");
    REQUIRE(r.success);           // output survived
    REQUIRE(r.inputUnavailable);  // mic reported unavailable, not silent
    REQUIRE(fix.engine().currentDevice().name == "Fake Speakers");
}

// ── Driver switch ───────────────────────────────────────────────────────────

TEST_CASE("Swap: driver switch lands on the target driver's default device",
          "[SwapChar][driver]") {
    auto sys = makeSimpleSystem();
    auto second = std::make_shared<FakeDeviceSpec>();
    second->name = "Other Card";
    sys->types.push_back({ "OtherDriver", { second }, 0 });

    EngineFixture fix(fakeEngineConfig(sys, "Fake Speakers"));
    REQUIRE(fix.engine().currentDriver() == "FakeDriver");

    auto r = fix.engine().switchDriver("OtherDriver");
    REQUIRE(r.success);
    REQUIRE(fix.engine().currentDriver() == "OtherDriver");
    REQUIRE(fix.engine().currentDevice().name == "Other Card");
}

// ── Reopen / recovery ───────────────────────────────────────────────────────

TEST_CASE("Swap: /supersonic/devices/reopen recovers through the factory seam",
          "[SwapChar][recovery]") {
    auto sys = makeSimpleSystem();
    EngineFixture fix(fakeEngineConfig(sys, "Fake Speakers"));

    // The recovery worker tears the manager down and rebuilds it via
    // makeDeviceManager() — with the factory seam that means fresh fakes,
    // exactly like recovery after wake rebuilds against real hardware.
    OscReply reply;
    fix.send(osc_test::message("/supersonic/devices/reopen"));
    REQUIRE(fix.waitForReply("/supersonic/devices/reopen.done", reply, 10000));

    auto args = reply.parsed();
    REQUIRE(args.argCount() >= 1);
    REQUIRE(args.argInt(0) == 1);   // success

    // Engine is live on a (re-created) fake device afterwards.
    fix.clearReplies();
    fix.send(osc_test::message("/status"));
    REQUIRE(fix.waitForReply("/status.reply", reply));
}

// ── Device-mutation phase ───────────────────────────────────────────────────

TEST_CASE("Swap: devicePhase is Idle at rest, Swapping under a held swap",
          "[SwapChar][phase]") {
    auto sys = makeSimpleSystem();
    EngineFixture fix(fakeEngineConfig(sys, "Fake Speakers"));
    using Phase = SupersonicEngine::DevicePhase;

    REQUIRE(fix.engine().devicePhase() == Phase::Idle);
    {
        auto hold = fix.engine().testHoldSwapGate();
        REQUIRE(fix.engine().devicePhase() == Phase::Swapping);
    }
    REQUIRE(fix.engine().devicePhase() == Phase::Idle);

    auto r = fix.engine().switchDevice("Fake Interface", 0, 0, false, "__none__");
    REQUIRE(r.success);
    REQUIRE(fix.engine().devicePhase() == Phase::Idle);   // guard restored it
}

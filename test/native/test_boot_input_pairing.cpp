/*
 * test_boot_input_pairing.cpp — the -H boot must never activate an input
 * device it wasn't explicitly given (#3554).
 *
 * With inputs enabled and a blank input name, JUCE's initialise fills the
 * name with the type default (insertDefaultDeviceNames). On macOS that
 * implicit pick opened the default mic through JUCE's Combiner — the
 * intermittent RC6 boot SIGSEGV / distorted-audio reports. The engine's
 * contract: input pairing is always an explicit engine decision (same-device
 * full duplex at open, or aggregate promotion after it).
 */
#include "EngineFixture.h"
#include "FakeAudioDevice.h"
#include "OscTestUtils.h"
#include <catch2/catch_test_macros.hpp>

using fake_audio::fakeEngineConfig;
using fake_audio::makeSimpleSystem;

TEST_CASE("Boot: -H output with inputs enabled activates no implicit input",
          "[DeviceSelection][boot]") {
    auto sys = makeSimpleSystem();
    auto cfg = fakeEngineConfig(sys, "Fake Speakers");
    cfg.numInputChannels = 2;
    EngineFixture fix(cfg);

    auto cur = fix.engine().currentDevice();
    REQUIRE(cur.name == "Fake Speakers");
    // Uniform contract on every platform: a -H boot never opens an
    // implicit input. Input pairing belongs to the explicit paths —
    // aggregate promotion on macOS (#3554), the preferred-input pairing
    // block elsewhere.
    REQUIRE(cur.activeInputChannels == 0);

    OscReply reply;
    fix.send(osc_test::message("/status"));
    REQUIRE(fix.waitForReply("/status.reply", reply));
}

TEST_CASE("Boot: -H full-duplex device with matching input pref keeps its input",
          "[DeviceSelection][boot]") {
    auto sys = makeSimpleSystem();
    auto cfg = fakeEngineConfig(sys, "Fake Interface");
    cfg.numInputChannels = 2;
    cfg.inputDevice = "Fake Interface";
    EngineFixture fix(cfg);

    auto cur = fix.engine().currentDevice();
    REQUIRE(cur.name == "Fake Interface");
    REQUIRE(cur.activeInputChannels > 0);
}

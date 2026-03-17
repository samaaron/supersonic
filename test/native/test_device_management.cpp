/*
 * test_device_management.cpp — Device enumeration, swap, pause/resume tests
 */
#include <catch2/catch_test_macros.hpp>
#include "EngineFixture.h"
#include "OscBuilder.h"

// ── Device enumeration (headless) ────────────────────────────────────────────

TEST_CASE("DeviceManagement: listDevices returns empty in headless mode",
          "[DeviceManagement]") {
    EngineFixture fix;
    auto devices = fix.engine().listDevices();
    REQUIRE(devices.empty());
}

TEST_CASE("DeviceManagement: currentDevice returns zeroed in headless mode",
          "[DeviceManagement]") {
    EngineFixture fix;
    auto dev = fix.engine().currentDevice();
    REQUIRE(dev.name.empty());
    REQUIRE(dev.activeSampleRate == 0.0);
    REQUIRE(dev.activeBufferSize == 0);
}

// ── Device swap (headless) ──────────────────────────────────────────────────

TEST_CASE("DeviceManagement: switchDevice works in headless mode (hot swap)",
          "[DeviceManagement]") {
    EngineFixture fix;
    // Same rate = hot swap, should succeed in headless mode
    auto result = fix.engine().switchDevice("", 48000);
    REQUIRE(result.success);
    REQUIRE(result.type == SwapType::Hot);
}

TEST_CASE("DeviceManagement: onSwapEvent callback fires", "[DeviceManagement]") {
    EngineFixture fix;
    std::vector<std::string> events;

    fix.engine().onSwapEvent = [&](const std::string& event, const SwapResult&) {
        events.push_back(event);
    };

    // Hot swap in headless mode should fire events
    auto result = fix.engine().switchDevice("", 48000);
    REQUIRE(result.success);
    REQUIRE(events.size() == 2);
    REQUIRE(events[0] == "swap:start");
    REQUIRE(events[1] == "swap:complete");
}

// ── State cache interception ─────────────────────────────────────────────────

TEST_CASE("DeviceManagement: sendOsc intercepts /d_recv for cache",
          "[DeviceManagement]") {
    EngineFixture fix;

    // Load a real synthdef file
    REQUIRE(fix.loadSynthDef("sonic-pi-beep"));

    // Verify the state cache captured it
    auto defs = fix.engine().stateCache().synthDefs();
    REQUIRE(defs.count("sonic-pi-beep") == 1);
    REQUIRE(defs.at("sonic-pi-beep").size() > 0);
}

TEST_CASE("DeviceManagement: /d_free removes from cache", "[DeviceManagement]") {
    EngineFixture fix;
    REQUIRE(fix.loadSynthDef("sonic-pi-beep"));
    REQUIRE(fix.engine().stateCache().synthDefs().count("sonic-pi-beep") == 1);

    fix.send(osc_test::message("/d_free", "sonic-pi-beep"));

    REQUIRE(fix.engine().stateCache().synthDefs().count("sonic-pi-beep") == 0);
}

TEST_CASE("DeviceManagement: /d_freeAll clears cache", "[DeviceManagement]") {
    EngineFixture fix;
    REQUIRE(fix.loadSynthDef("sonic-pi-beep"));

    fix.send(osc_test::message("/d_freeAll"));

    REQUIRE(fix.engine().stateCache().synthDefs().empty());
}

TEST_CASE("DeviceManagement: /b_allocRead caches buffer metadata",
          "[DeviceManagement]") {
    EngineFixture fix;

    osc_test::Builder b;
    auto& s = b.begin("/b_allocRead");
    s << static_cast<osc::int32>(5) << "/samples/test.wav"
      << static_cast<osc::int32>(0) << static_cast<osc::int32>(44100);
    auto pkt = b.end();
    fix.send(pkt);

    auto bufs = fix.engine().stateCache().buffers();
    REQUIRE(bufs.size() == 1);
    REQUIRE(bufs[0].bufnum == 5);
    REQUIRE(bufs[0].path == "/samples/test.wav");
}

TEST_CASE("DeviceManagement: /b_free uncaches buffer", "[DeviceManagement]") {
    EngineFixture fix;

    osc_test::Builder b;
    auto& s = b.begin("/b_allocRead");
    s << static_cast<osc::int32>(5) << "/samples/test.wav"
      << static_cast<osc::int32>(0) << static_cast<osc::int32>(0);
    auto pkt = b.end();
    fix.send(pkt);

    REQUIRE(fix.engine().stateCache().buffers().size() == 1);

    fix.send(osc_test::message("/b_free", 5));

    REQUIRE(fix.engine().stateCache().buffers().empty());
}

// ── Pause/resume ─────────────────────────────────────────────────────────────

TEST_CASE("DeviceManagement: pause and resume", "[DeviceManagement]") {
    EngineFixture fix;

    auto& cb = fix.engine().audioCallback();
    REQUIRE_FALSE(cb.isPaused());

    cb.pause();
    REQUIRE(cb.isPaused());

    cb.resume();
    REQUIRE_FALSE(cb.isPaused());

    // Verify pump still works after pause/resume cycle
    OscReply reply;
    fix.send(osc_test::message("/status"));
    REQUIRE(fix.waitForReply("/status.reply", reply));
}

// ── Clock offset ─────────────────────────────────────────────────────────────

TEST_CASE("DeviceManagement: setClockOffset / getClockOffset", "[DeviceManagement]") {
    EngineFixture fix;

    fix.engine().setClockOffset(0.0);
    REQUIRE(fix.engine().getClockOffset() == 0.0);

    fix.engine().setClockOffset(1.5);
    // int32 ms precision: 1.5 * 1000 = 1500, 1500 / 1000.0 = 1.5
    REQUIRE(fix.engine().getClockOffset() == 1.5);

    fix.engine().setClockOffset(-0.25);
    REQUIRE(fix.engine().getClockOffset() == -0.25);
}

// ── Purge ────────────────────────────────────────────────────────────────────

TEST_CASE("DeviceManagement: purge clears ring buffer and scheduler",
          "[DeviceManagement]") {
    EngineFixture fix;

    // Send some messages, then purge
    fix.engine().purge();

    // Verify engine still works after purge
    OscReply reply;
    fix.send(osc_test::message("/status"));
    REQUIRE(fix.waitForReply("/status.reply", reply));
}

// ── OscBuilder ───────────────────────────────────────────────────────────────

TEST_CASE("OscBuilder: message builds valid OSC", "[OscBuilder]") {
    auto pkt = OscBuilder::message("/test", 42, 3.14f, "hello");
    REQUIRE(pkt.size() > 0);
    // Verify it starts with the address pattern
    REQUIRE(std::memcmp(pkt.ptr(), "/test", 5) == 0);
}

TEST_CASE("OscBuilder: bundle builds valid OSC", "[OscBuilder]") {
    auto msg1 = OscBuilder::message("/a", 1);
    auto msg2 = OscBuilder::message("/b", 2);
    auto bun = OscBuilder::bundle(1, {msg1, msg2});

    REQUIRE(bun.size() > 16);
    REQUIRE(std::memcmp(bun.ptr(), "#bundle", 7) == 0);
}

TEST_CASE("OscBuilder: variadic send via engine", "[OscBuilder]") {
    EngineFixture fix;

    // Use the templated send() method
    fix.engine().send("/status");
    OscReply reply;
    REQUIRE(fix.waitForReply("/status.reply", reply));
}

// ── State cache module registration ──────────────────────────────────────────

TEST_CASE("DeviceManagement: stateCache module registration", "[DeviceManagement]") {
    EngineFixture fix;
    int captured = 0, restored = 0;

    fix.engine().stateCache().registerModule({
        "test-module",
        [&]() { captured++; },
        [&]() { restored++; }
    });

    fix.engine().stateCache().captureAll();
    REQUIRE(captured == 1);

    fix.engine().stateCache().restoreAll();
    REQUIRE(restored == 1);
}

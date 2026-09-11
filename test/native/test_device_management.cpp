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

TEST_CASE("DeviceManagement: listDrivers returns empty in headless mode",
          "[DeviceManagement]") {
    EngineFixture fix;
    auto drivers = fix.engine().listDrivers();
    REQUIRE(drivers.empty());
}

TEST_CASE("DeviceManagement: currentDriver returns empty in headless mode",
          "[DeviceManagement]") {
    EngineFixture fix;
    auto driver = fix.engine().currentDriver();
    REQUIRE(driver.empty());
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

// ── /d_recv, /d_free and /d_freeAll interception — REMOVED ──────────────────
//
// Three cases here asserted that ClockworkEngine watched those three addresses go
// past and kept the synthdefs they carried in a StateCache. That whole seam is
// gone: DspInfo no longer carries definition_verb/forget_verb/forget_all_verb,
// there is no dsp_definition_name(), and StateCache itself has been deleted —
// it asked clockwork to know which of a guest's messages carry state worth
// keeping, which is a question it cannot answer.
//
// What replaced it is not a smaller cache. A guest that must survive its own
// destruction writes into DspConfig::persistent, which clockwork carries
// across a rebuild without reading; everything else lives in
// DspConfig::guest_memory and is cleared at each dsp_new. Covered by
// clockwork/test/test_dsp_regions.cpp and test_rebuild_contract.cpp.
//
// Restore across a device swap is the client's, by design — see the cold-swap
// resume in ClockworkEngine.cpp and SuperSonic::restoreClientState(), covered by
// recover.spec.mjs and load_sample.spec.mjs.

// ── Per-driver device table broadcast ───────────────────────────────────────

TEST_CASE("DeviceManagement: device report carries a well-formed per-driver table",
          "[DeviceManagement]") {
    EngineFixture fix;
    fix.clearReplies();
    // Subscribing a reply port makes the engine broadcast the report set.
    fix.send(osc_test::message("/clockwork/devices/report",
                               static_cast<int32_t>(1)));

    // The grouped table is broadcast alongside the flat report. Headless:
    // no device manager, so both driver fields are empty and there are no
    // driver groups — but the header must still parse counts-first.
    OscReply table;
    REQUIRE(fix.waitForReply("/clockwork/device-table", table));
    CHECK(table.parsed().argString(0) == "");   // currentDriver
    CHECK(table.parsed().argString(1) == "");   // intendedDriver
    CHECK(table.parsed().argInt(2) == 0);       // numDrivers

    // The legacy flat report still goes out unchanged.
    OscReply flat;
    CHECK(fix.waitForReply("/clockwork/devices", flat));
}

// ── /b_allocRead and /b_free caching — REMOVED ──────────────────────────────
//
// Two cases here asserted that clockwork's StateCache remembered buffer
// metadata as /b_allocRead and /b_free went past. It did so by matching those
// two addresses inside ClockworkEngine — scsynth vocabulary in a host whose whole
// premise is not having any — and storing them in a struct that was
// /b_allocRead's signature transcribed field by field.
//
// The cache had no reader. Restore across a device swap is the client's, by
// design: see the cold-swap resume in ClockworkEngine.cpp, which says so, and
// SuperSonic::restoreClientState(), which does it. Covered by
// recover.spec.mjs and load_sample.spec.mjs.
//
// A guest-agnostic buffer cache is buildable, but nothing needs it yet, and
// the version that was here could not have been made correct by adjusting it.
// The definition cache this comment once pointed to as the model has since
// been removed for the same reason — see the /d_recv note below.

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

// ── State cache module registration — REMOVED ───────────────────────────────
//
// StateCache::registerModule had no caller outside this test for the whole of
// its life, so captureAll() walked an empty vector on every cold swap. Removed
// with the class.

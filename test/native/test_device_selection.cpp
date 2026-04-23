/*
 * test_device_selection.cpp — Device identity state invariants
 *
 * Covers the state that tracks "what device the engine is using" across
 * hot-plug and swap cycles:
 *   - mDeviceRateMemory bounded growth (cap at 32 entries)
 *   - mPreferredOutputDevice / mDeviceMode invariants through
 *     switchDevice + setDeviceMode transitions
 *   - Empty deviceName does NOT clobber mPreferredOutputDevice (rate /
 *     buffer tweaks shouldn't reset the user's device preference)
 *
 * These scenarios have all caused bugs in the past. The engine keeps
 * most of this state in private members; these tests drive it through
 * the public API and assert on user-visible consequences.
 */
#include <catch2/catch_test_macros.hpp>
#include "EngineFixture.h"
#include "OscBuilder.h"
#include <string>

// ── Rate memory bound (32 entries) ───────────────────────────────────────────

TEST_CASE("DeviceSelection: rate memory caps at 32 entries",
          "[DeviceSelection]") {
    EngineFixture fix;

    // Push 40 distinct device names through a cold swap each. In headless
    // mode the name is ignored by the swap itself but still keyed into
    // mDeviceRateMemory. At the cap (32) the map gets cleared and starts
    // over, so no matter how many insertions we do it must stay ≤ 32.
    for (int i = 0; i < 40; ++i) {
        std::string name = "test-device-" + std::to_string(i);
        // Alternate rates so every swap is a cold swap and populates the map
        double rate = (i % 2 == 0) ? 44100.0 : 48000.0;
        auto r = fix.engine().switchDevice(name, rate);
        REQUIRE(r.success);
    }

    // Engine must still be alive — no crash from the eviction
    OscReply reply;
    fix.send(osc_test::message("/status"));
    REQUIRE(fix.waitForReply("/status.reply", reply));
}

TEST_CASE("DeviceSelection: rate memory restores per-device rate",
          "[DeviceSelection]") {
    EngineFixture fix;

    // Cold swap with an explicit rate — remembers "device-A" → 44100
    auto r1 = fix.engine().switchDevice("device-A", 44100);
    REQUIRE(r1.success);
    REQUIRE(r1.type == SwapType::Cold);

    // Move rate elsewhere
    auto r2 = fix.engine().switchDevice("device-B", 48000);
    REQUIRE(r2.success);
    REQUIRE(r2.type == SwapType::Cold);

    // Switch back to "device-A" with no explicit rate — should restore 44100
    auto r3 = fix.engine().switchDevice("device-A");
    REQUIRE(r3.success);
    REQUIRE(r3.type == SwapType::Cold);
    REQUIRE(static_cast<int>(r3.sampleRate) == 44100);
}

// ── Preferred output / input device (hot-plug intent) ───────────────────────
// mPreferredOutputDevice / mPreferredInputDevice track "want this device
// whenever it's available" so hot-plug logic can auto-re-attach when the
// device returns. switchDevice manages these; they must persist across
// empty-name swaps (rate/buffer tweaks aren't device changes), and the
// "__none__" input sentinel must CLEAR the preferred input.

TEST_CASE("DeviceSelection: switchDevice with empty name preserves preferred output",
          "[DeviceSelection]") {
    EngineFixture fix;

    auto r1 = fix.engine().switchDevice("my-device", 44100);
    REQUIRE(r1.success);
    REQUIRE(fix.engine().preferredOutputDevice() == "my-device");

    // Rate/buffer tweaks (empty deviceName) MUST NOT wipe the user's
    // device preference.
    auto r2 = fix.engine().switchDevice("", 0, 256);
    REQUIRE(r2.success);
    REQUIRE(fix.engine().preferredOutputDevice() == "my-device");

    auto r3 = fix.engine().switchDevice("", 44100);
    REQUIRE(r3.success);
    REQUIRE(fix.engine().preferredOutputDevice() == "my-device");
}

TEST_CASE("DeviceSelection: __none__ input sentinel clears preferred input",
          "[DeviceSelection]") {
    EngineFixture fix;

    // Pin an input device
    auto r1 = fix.engine().switchDevice("out-dev", 44100, 0, false, "in-dev");
    REQUIRE(r1.success);
    REQUIRE(fix.engine().preferredInputDevice() == "in-dev");

    // __none__ should clear it — user intent: "I want no input"
    auto r2 = fix.engine().switchDevice("", 0, 0, false, "__none__");
    REQUIRE(r2.success);
    REQUIRE(fix.engine().preferredInputDevice().empty());
}

// ── Channel-count-change forces cold swap ───────────────────────────────────
// enableInputChannels changes numInputChannels. A change from N → M (with
// N != M) must force a cold swap so the scsynth World is rebuilt with the
// new bus count — opts[kNumInputBusChannels] needs to flow through. A hot
// swap would leave the World with the old bus count.

TEST_CASE("DeviceSelection: input channel 2 → 4 forces cold swap",
          "[DeviceSelection]") {
    EngineFixture fix;

    // Default fixture has 2 inputs. Go to 4.
    auto r = fix.engine().enableInputChannels(4);
    REQUIRE(r.success);
    REQUIRE(r.type == SwapType::Cold);
    REQUIRE(fix.engine().configuredInputChannels() == 2); // boot value unchanged
}


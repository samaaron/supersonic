/*
 * test_device_policy.cpp — Pure-function tests for device-management policies
 *
 * The big switchDevice / changeListenerCallback functions interleave
 * CoreAudio / JUCE / scsynth concerns with several narrow policy
 * decisions. Those decisions are extracted as pure static functions so
 * they can be tested directly without a real audio device:
 *
 *   - resolveWirelessExitRate: should we restore the pre-wireless rate
 *     when leaving AirPlay / Bluetooth?
 *   - decideHotplugAction: given visible devices, should we re-attach
 *     to the preferred output or re-aggregate to pick up an input?
 *
 * Each scenario here corresponds to a bug we've fixed or a behaviour
 * we want to lock in going forward.
 */
#include <catch2/catch_test_macros.hpp>
#include "DevicePolicy.h"
#include <string>
#include <vector>

// =============================================================================
// resolveWirelessExitRate
// =============================================================================

static double resolveRate(double requested, int preWireless, double current,
                          bool curIsWireless, bool targetIsWireless) {
    return sonicpi::device::resolveWirelessExitRate(
        requested, preWireless, current, curIsWireless, targetIsWireless);
}

TEST_CASE("WirelessExit: caller-supplied rate is never overridden",
          "[WirelessExit]") {
    // Even if EVERY other condition says we should restore, a caller-
    // specified rate wins. No silent rewrites.
    REQUIRE(resolveRate(96000, 48000, 44100, true, false) == 96000);
    REQUIRE(resolveRate(44100, 48000, 44100, true, false) == 44100);
}

TEST_CASE("WirelessExit: no preWirelessRate memory = nothing to restore",
          "[WirelessExit]") {
    REQUIRE(resolveRate(0, 0, 44100, true, false)  == 0);
    REQUIRE(resolveRate(0, -1, 44100, true, false) == 0);
}

TEST_CASE("WirelessExit: current is not wireless = no restoration",
          "[WirelessExit]") {
    // E.g. MacBook Speakers → Display Speakers — neither wireless,
    // no detour to restore from.
    REQUIRE(resolveRate(0, 48000, 44100, false, false) == 0);
}

TEST_CASE("WirelessExit: target IS wireless = no restoration",
          "[WirelessExit]") {
    // Wireless → wireless (e.g. AirPlay → Bluetooth) — stay at
    // negotiated rate; don't restore the pre-detour rate mid-wireless.
    REQUIRE(resolveRate(0, 48000, 44100, true, true) == 0);
}

TEST_CASE("WirelessExit: AirPlay 44.1 → MBP Speakers restores 48k",
          "[WirelessExit]") {
    // The canonical scenario: MBP Speakers at 48k, user detoured
    // through AirPlay (forced 44.1), now switching back. 48000 wins.
    REQUIRE(resolveRate(0, 48000, 44100, true, false) == 48000);
}

// =============================================================================
// decideHotplugAction
// =============================================================================

using HD = sonicpi::device::HotplugDecision;

static HD decide(const std::string& prefOut,
                 const std::string& prefIn,
                 const std::string& currentOut,
                 int inChan,
                 std::vector<std::string> visible) {
    return sonicpi::device::decideHotplugAction(
        prefOut, prefIn, currentOut, inChan, visible);
}

TEST_CASE("Hotplug: no preferences = no action", "[Hotplug]") {
    auto d = decide("", "", "MacBook Pro Speakers", 0,
                    {"MacBook Pro Speakers", "MacBook Pro Microphone"});
    REQUIRE_FALSE(d.switchOutput);
    REQUIRE_FALSE(d.switchInput);
}

TEST_CASE("Hotplug: preferred output not yet visible = no action",
          "[Hotplug]") {
    // User's USB interface is unplugged; we're on system default. No
    // auto-switch until it appears.
    auto d = decide("MOTU UltraLite", "", "MacBook Pro Speakers", 0,
                    {"MacBook Pro Speakers", "MacBook Pro Microphone"});
    REQUIRE_FALSE(d.switchOutput);
    REQUIRE_FALSE(d.switchInput);
}

TEST_CASE("Hotplug: preferred output returned = switch output", "[Hotplug]") {
    auto d = decide("MOTU UltraLite", "", "MacBook Pro Speakers", 0,
                    {"MacBook Pro Speakers", "MacBook Pro Microphone",
                     "MOTU UltraLite"});
    REQUIRE(d.switchOutput);
    REQUIRE(d.outputName == "MOTU UltraLite");
    REQUIRE(d.inputName.empty());
}

TEST_CASE("Hotplug: preferred output + preferred input both return",
          "[Hotplug]") {
    // USB interface with its own mic came back — aggregate with both.
    auto d = decide("MOTU UltraLite", "MOTU UltraLite Mic",
                    "MacBook Pro Speakers", 0,
                    {"MacBook Pro Speakers", "MOTU UltraLite",
                     "MOTU UltraLite Mic"});
    REQUIRE(d.switchOutput);
    REQUIRE(d.outputName == "MOTU UltraLite");
    REQUIRE(d.inputName  == "MOTU UltraLite Mic");
}

TEST_CASE("Hotplug: already on preferred output = no action", "[Hotplug]") {
    auto d = decide("MOTU UltraLite", "", "MOTU UltraLite", 0,
                    {"MOTU UltraLite"});
    REQUIRE_FALSE(d.switchOutput);
    REQUIRE_FALSE(d.switchInput);
}

TEST_CASE("Hotplug: preferred input returns while output matches",
          "[Hotplug]") {
    // MBP output already correct, mic just reappeared and we're running
    // with 0 input channels — re-aggregate to pick it up.
    auto d = decide("", "MacBook Pro Microphone",
                    "MacBook Pro Speakers", 0,
                    {"MacBook Pro Speakers", "MacBook Pro Microphone"});
    REQUIRE_FALSE(d.switchOutput);
    REQUIRE(d.switchInput);
    REQUIRE(d.inputName == "MacBook Pro Microphone");
}

TEST_CASE("Hotplug: preferred input returns but input already active = no action",
          "[Hotplug]") {
    // We're already recording from an input (channels > 0). Don't
    // interrupt to re-aggregate.
    auto d = decide("", "MacBook Pro Microphone",
                    "MacBook Pro Speakers", 2,
                    {"MacBook Pro Speakers", "MacBook Pro Microphone"});
    REQUIRE_FALSE(d.switchOutput);
    REQUIRE_FALSE(d.switchInput);
}

TEST_CASE("Hotplug: output switch dominates when both would apply",
          "[Hotplug]") {
    // If the output needs to switch, the re-aggregate is rolled into
    // the same swap — don't schedule both.
    auto d = decide("MOTU UltraLite", "MOTU UltraLite Mic",
                    "MacBook Pro Speakers", 0,
                    {"MacBook Pro Speakers",
                     "MOTU UltraLite", "MOTU UltraLite Mic"});
    REQUIRE(d.switchOutput);
    REQUIRE_FALSE(d.switchInput);
}

TEST_CASE("Hotplug: JUCE device-name suffixes match through deviceNameMatches",
          "[Hotplug]") {
    // JUCE sometimes reports devices as "MOTU UltraLite (2)" when more
    // than one is present. The match helper accepts suffix variants;
    // decideHotplugAction must use it so re-attach works.
    auto d = decide("MOTU UltraLite", "", "MacBook Pro Speakers", 0,
                    {"MacBook Pro Speakers", "MOTU UltraLite (2)"});
    REQUIRE(d.switchOutput);
    REQUIRE(d.outputName == "MOTU UltraLite");
}

TEST_CASE("Hotplug: preferred input visible but preferred output NOT visible "
          "= only input re-aggregate",
          "[Hotplug]") {
    // Output device is gone but input came back. Shouldn't try to
    // switch to a missing output — just re-aggregate for the input.
    auto d = decide("MOTU UltraLite", "MacBook Pro Microphone",
                    "MacBook Pro Speakers", 0,
                    {"MacBook Pro Speakers", "MacBook Pro Microphone"});
    REQUIRE_FALSE(d.switchOutput);
    REQUIRE(d.switchInput);
    REQUIRE(d.inputName == "MacBook Pro Microphone");
}

// =============================================================================
// resolveJuceDeviceName — CoreAudio raw name → JUCE-disambiguated form
//
// JUCE appends " (N)" to device names when CoreAudio has duplicates.
// A CoreAudio-sourced raw name needs to be translated before handing it
// to JUCE's setAudioDeviceSetup, or we get "No such device" errors.
// =============================================================================

static std::string resolveName(const std::string& raw,
                               std::vector<std::string> visible) {
    return sonicpi::device::resolveJuceDeviceName(raw, visible);
}

TEST_CASE("JuceName: empty input returned unchanged", "[JuceName]") {
    REQUIRE(resolveName("", {"MacBook Pro Speakers"}) == "");
}

TEST_CASE("JuceName: exact match returns as-is", "[JuceName]") {
    // Name is already in JUCE's list — no translation needed.
    REQUIRE(resolveName("MacBook Pro Speakers",
                        {"MacBook Pro Speakers"})
            == "MacBook Pro Speakers");
}

TEST_CASE("JuceName: raw name resolves to first (N) form",
          "[JuceName]") {
    // Two duplicate CoreAudio devices — JUCE disambiguated them with
    // (1) and (2). CoreAudio reports the raw name; we pick (1).
    REQUIRE(resolveName("DMP-A6(Kitchen)",
                        {"DMP-A6(Kitchen) (1)", "DMP-A6(Kitchen) (2)"})
            == "DMP-A6(Kitchen) (1)");
}

TEST_CASE("JuceName: already-suffixed name stays the same",
          "[JuceName]") {
    // If the caller already passed the (2) form, keep it — don't
    // accidentally rewrite to (1).
    REQUIRE(resolveName("USB Audio (2)",
                        {"USB Audio (1)", "USB Audio (2)"})
            == "USB Audio (2)");
}

TEST_CASE("JuceName: no match returns rawName unchanged",
          "[JuceName]") {
    // Caller asked for a device that doesn't exist — let JUCE error
    // normally rather than silently rewriting to something else.
    REQUIRE(resolveName("Unknown Device",
                        {"USB Audio (1)"})
            == "Unknown Device");
}

TEST_CASE("JuceName: doesn't match 'USB Audio Pro' against 'USB Audio'",
          "[JuceName]") {
    // A naive prefix-plus-space check would match "USB Audio Pro"
    // against "USB Audio" — that's a different device. The stricter
    // "(<digits>)" check avoids the false positive.
    REQUIRE(resolveName("USB Audio", {"USB Audio Pro"}) == "USB Audio");
}

TEST_CASE("JuceName: non-digit parenthetical isn't treated as JUCE suffix",
          "[JuceName]") {
    // "Speakers (Main)" is a real device name, not JUCE disambiguation.
    // Wrong to match it against "Speakers".
    REQUIRE(resolveName("Speakers", {"Speakers (Main)"}) == "Speakers");
}

TEST_CASE("JuceName: multi-digit suffix recognised",
          "[JuceName]") {
    // Unlikely in practice but JUCE's pattern is any digits in parens.
    REQUIRE(resolveName("USB Audio",
                        {"USB Audio (99)"})
            == "USB Audio (99)");
}

// =============================================================================
// selectBootOutputDevice — wireless-default fallback
//
// At boot, if macOS' default output is wireless (AirPlay/Bluetooth),
// JUCE's initialiseWithDefaultDevices + subsequent aggregate creation
// triggers a ~15 s IOProc halt that times out Sonic Pi's boot
// handshake. Pick a non-wireless device up front instead.
// =============================================================================

static std::string selectBoot(const std::string& defName, bool defWireless,
                              std::vector<std::string> visible,
                              std::vector<bool> wireless) {
    return sonicpi::device::selectBootOutputDevice(defName, defWireless,
                                                    visible, wireless);
}

TEST_CASE("BootFallback: non-wireless default = no fallback",
          "[BootFallback]") {
    // If default is already non-wireless (e.g. MBP Speakers), there's
    // nothing to do — use the default as-is.
    REQUIRE(selectBoot("MacBook Pro Speakers", false,
                       {"MacBook Pro Speakers", "MOTU UltraLite"},
                       {false, false})
            .empty());
}

TEST_CASE("BootFallback: wireless default + non-wireless visible = pick it",
          "[BootFallback]") {
    // AirPlay is default but MBP Speakers is available. Pick MBP.
    auto picked = selectBoot(
        "Living Room Speakers", true,
        {"Living Room Speakers", "MacBook Pro Speakers"},
        {true, false});
    REQUIRE(picked == "MacBook Pro Speakers");
}

TEST_CASE("BootFallback: first non-wireless wins",
          "[BootFallback]") {
    // Multiple non-wireless candidates — pick the first (JUCE order).
    auto picked = selectBoot(
        "AirPlay Speaker", true,
        {"AirPlay Speaker", "MacBook Pro Speakers", "MOTU UltraLite"},
        {true, false, false});
    REQUIRE(picked == "MacBook Pro Speakers");
}

TEST_CASE("BootFallback: only wireless visible = empty (accept silence)",
          "[BootFallback]") {
    // If every visible output is wireless, the fallback is impossible.
    // Return empty and let the default path open the wireless device —
    // the alternative is silent boot.
    REQUIRE(selectBoot("AirPlay A", true,
                       {"AirPlay A", "AirPlay B"},
                       {true, true})
            .empty());
}

TEST_CASE("BootFallback: mismatched array sizes = empty (defensive)",
          "[BootFallback]") {
    // Caller bug: visible names and wireless flags don't zip. Return
    // empty rather than reading past the end.
    REQUIRE(selectBoot("X", true, {"A", "B", "C"}, {true, false})
            .empty());
}

TEST_CASE("BootFallback: skip the default itself when it's in the list",
          "[BootFallback]") {
    // Default name appears in visible list with its own wireless flag.
    // We should still pick a DIFFERENT non-wireless device, not the
    // wireless default itself.
    auto picked = selectBoot(
        "AirPlay", true,
        {"AirPlay", "MacBook Pro Speakers"},
        {true, false});
    REQUIRE(picked == "MacBook Pro Speakers");
}

// =============================================================================
// chooseBlockSize — auto-match scsynth block size to HW callback
//
// Matching means 1:1 per-callback processing (no prefetch buffer).
// Diverging means the decoupling code in JuceAudioCallback handles it.
// =============================================================================

static int chooseBlock(int hw, int def = 128, int lo = 32, int hi = 1024) {
    return sonicpi::device::chooseBlockSize(hw, def, lo, hi);
}

TEST_CASE("ChooseBlockSize: in-range HW matches 1:1", "[ChooseBlockSize]") {
    REQUIRE(chooseBlock(32)   == 32);
    REQUIRE(chooseBlock(64)   == 64);
    REQUIRE(chooseBlock(128)  == 128);
    REQUIRE(chooseBlock(256)  == 256);
    REQUIRE(chooseBlock(512)  == 512);
    REQUIRE(chooseBlock(1024) == 1024);
}

TEST_CASE("ChooseBlockSize: below floor falls back to default",
          "[ChooseBlockSize]") {
    // HW buffer smaller than scsynth's minimum block size (32) — fall
    // back rather than corrupt the graph.
    REQUIRE(chooseBlock(0)  == 128);
    REQUIRE(chooseBlock(16) == 128);
    REQUIRE(chooseBlock(31) == 128);
}

TEST_CASE("ChooseBlockSize: above ceiling falls back to default",
          "[ChooseBlockSize]") {
    // Exotic HW buffer bigger than scsynth's max — fall back.
    // static_audio_bus is sized to the max at compile time; using a
    // larger block would walk off the end.
    REQUIRE(chooseBlock(1025) == 128);
    REQUIRE(chooseBlock(2048) == 128);
    REQUIRE(chooseBlock(4096) == 128);
}

TEST_CASE("ChooseBlockSize: negative HW treated as fallback",
          "[ChooseBlockSize]") {
    // JUCE returns -1 from getCurrentBufferSizeSamples when no device
    // is open. Must not pass -1 through.
    REQUIRE(chooseBlock(-1) == 128);
}

TEST_CASE("ChooseBlockSize: respects custom default", "[ChooseBlockSize]") {
    // Defaults are injected so callers can override on niche platforms.
    REQUIRE(chooseBlock(0, 64) == 64);
    REQUIRE(chooseBlock(2048, 256) == 256);
}

// =============================================================================
// validateSwapDeviceNames — pre-flight check before destructive swap
// =============================================================================

static std::string validate(const std::string& out, const std::string& in,
                            std::vector<std::string> visible) {
    return sonicpi::device::validateSwapDeviceNames(out, in, visible);
}

TEST_CASE("ValidateSwap: empty names accepted (means leave unchanged)",
          "[ValidateSwap]") {
    REQUIRE(validate("", "", {"MacBook Pro Speakers"}).empty());
}

TEST_CASE("ValidateSwap: known sentinels accepted", "[ValidateSwap]") {
    REQUIRE(validate("__system__", "", {}).empty());
    REQUIRE(validate("", "__none__", {}).empty());
    REQUIRE(validate("__system__", "__none__", {}).empty());
}

TEST_CASE("ValidateSwap: matching device name accepted", "[ValidateSwap]") {
    REQUIRE(validate("MacBook Pro Speakers", "",
                     {"MacBook Pro Speakers", "MOTU UltraLite"}).empty());
}

TEST_CASE("ValidateSwap: matching JUCE-suffixed form accepted",
          "[ValidateSwap]") {
    REQUIRE(validate("USB Audio", "",
                     {"USB Audio (1)", "USB Audio (2)"}).empty());
}

TEST_CASE("ValidateSwap: '-- None --' display string refused",
          "[ValidateSwap]") {
    // The exact bug from the field: GUI's display string leaked through
    // to the wire. validateSwapDeviceNames must reject it so switchDevice
    // refuses up-front instead of mutating state and then failing
    // half-way through setAudioDeviceSetup.
    auto err = validate("", "-- None --",
                        {"MacBook Pro Speakers", "MacBook Pro Microphone"});
    REQUIRE_FALSE(err.empty());
    REQUIRE(err.find("-- None --") != std::string::npos);
}

TEST_CASE("ValidateSwap: unknown output device refused",
          "[ValidateSwap]") {
    auto err = validate("Phantom Device", "",
                        {"MacBook Pro Speakers"});
    REQUIRE_FALSE(err.empty());
    REQUIRE(err.find("Phantom Device") != std::string::npos);
    REQUIRE(err.find("output") != std::string::npos);
}

TEST_CASE("ValidateSwap: unknown input device refused",
          "[ValidateSwap]") {
    auto err = validate("", "Phantom Mic",
                        {"MacBook Pro Speakers", "MacBook Pro Microphone"});
    REQUIRE_FALSE(err.empty());
    REQUIRE(err.find("Phantom Mic") != std::string::npos);
    REQUIRE(err.find("input") != std::string::npos);
}

TEST_CASE("ValidateSwap: 'USB Audio' doesn't false-positive 'USB Audio Pro'",
          "[ValidateSwap]") {
    // Same prefix-matching trap as resolveJuceDeviceName — the
    // "(<digits>)" requirement is what excludes "USB Audio Pro" from
    // matching against base "USB Audio".
    auto err = validate("USB Audio", "", {"USB Audio Pro"});
    REQUIRE_FALSE(err.empty());
}

// =============================================================================
// locateDevice / planDeviceSwitch
// =============================================================================
//
// locateDevice answers "which AudioIODeviceType owns this device name?"
// from a flat (driver, device) table. planDeviceSwitch sits above it and
// returns the resolved (driver, device) pair plus whether the engine must
// call setCurrentAudioDeviceType before opening — the input to
// switchDevice's cross-driver branch.

using sonicpi::device::locateDevice;
using sonicpi::device::planDeviceSwitch;
using DevTable = std::vector<std::pair<std::string, std::string>>;

TEST_CASE("LocateDevice: empty name returns not-found",
          "[LocateDevice]") {
    DevTable table = {{"Windows Audio", "Speakers"}, {"ASIO", "MOTU Pro Audio"}};
    auto loc = locateDevice("", table);
    REQUIRE_FALSE(loc.found);
    REQUIRE(loc.driverName.empty());
}

TEST_CASE("LocateDevice: empty table returns not-found",
          "[LocateDevice]") {
    auto loc = locateDevice("MOTU Pro Audio", {});
    REQUIRE_FALSE(loc.found);
}

TEST_CASE("LocateDevice: exact name match returns owning driver",
          "[LocateDevice]") {
    DevTable table = {
        {"Windows Audio", "Speakers (MOTU Pro Audio)"},
        {"DirectSound",   "Primary Sound Driver"},
        {"ASIO",          "MOTU Pro Audio"},
        {"ASIO",          "Ableton Move"},
    };
    auto loc = locateDevice("MOTU Pro Audio", table);
    REQUIRE(loc.found);
    REQUIRE(loc.driverName == "ASIO");
    REQUIRE(loc.deviceName == "MOTU Pro Audio");
}

TEST_CASE("LocateDevice: same base name in two drivers — first wins",
          "[LocateDevice]") {
    // First-match-wins is the contract. A device name shared across
    // drivers resolves to whichever entry appears first in the table.
    // Callers that need a driver-specific answer scope the table.
    DevTable table = {
        {"Windows Audio", "MOTU Pro Audio"},
        {"ASIO",          "MOTU Pro Audio"},
    };
    auto loc = locateDevice("MOTU Pro Audio", table);
    REQUIRE(loc.found);
    REQUIRE(loc.driverName == "Windows Audio");
}

TEST_CASE("LocateDevice: tolerates JUCE '<base> (N)' disambiguation suffix",
          "[LocateDevice]") {
    // Two identical USB interfaces — JUCE appends " (2)" to the second.
    // A caller passing the unsuffixed base name should resolve.
    DevTable table = {
        {"Windows Audio", "Speakers (USB Audio)"},
        {"Windows Audio", "Speakers (USB Audio) (2)"},
    };
    auto loc = locateDevice("Speakers (USB Audio) (2)", table);
    REQUIRE(loc.found);
    REQUIRE(loc.deviceName == "Speakers (USB Audio) (2)");
}

TEST_CASE("PlanDeviceSwitch: device on current driver — no type switch",
          "[PlanDeviceSwitch]") {
    DevTable table = {
        {"Windows Audio", "Speakers"},
        {"ASIO",          "MOTU Pro Audio"},
    };
    auto plan = planDeviceSwitch("Windows Audio", "Speakers", table);
    REQUIRE(plan.deviceFound);
    REQUIRE_FALSE(plan.needsTypeSwitch);
    REQUIRE(plan.targetDriver == "Windows Audio");
    REQUIRE(plan.targetDevice == "Speakers");
}

TEST_CASE("PlanDeviceSwitch: device only on different driver — refused",
          "[PlanDeviceSwitch]") {
    // Runtime invariant: a device name that exists only on a driver
    // other than currentDriver resolves to deviceFound=false.
    // Cross-driver transitions are reserved for the explicit driver
    // selector; planDeviceSwitch does not perform them.
    DevTable table = {
        {"Windows Audio", "Speakers"},
        {"ASIO",          "MOTU Pro Audio"},
    };
    auto plan = planDeviceSwitch("Windows Audio", "MOTU Pro Audio", table);
    REQUIRE_FALSE(plan.deviceFound);
    REQUIRE_FALSE(plan.needsTypeSwitch);
    REQUIRE(plan.targetDriver.empty());
    REQUIRE(plan.targetDevice.empty());
}

TEST_CASE("PlanDeviceSwitch: unknown device — deviceFound=false",
          "[PlanDeviceSwitch]") {
    DevTable table = {{"ASIO", "MOTU Pro Audio"}};
    auto plan = planDeviceSwitch("ASIO", "Phantom Device", table);
    REQUIRE_FALSE(plan.deviceFound);
    REQUIRE_FALSE(plan.needsTypeSwitch);
    REQUIRE(plan.targetDriver.empty());
    REQUIRE(plan.targetDevice.empty());
}

TEST_CASE("PlanDeviceSwitch: empty current driver (cold init) — type switch needed",
          "[PlanDeviceSwitch]") {
    // At cold boot the engine has no current type yet; any named device
    // requires a type switch.
    DevTable table = {{"ASIO", "MOTU Pro Audio"}};
    auto plan = planDeviceSwitch("", "MOTU Pro Audio", table);
    REQUIRE(plan.deviceFound);
    REQUIRE(plan.needsTypeSwitch);
    REQUIRE(plan.targetDriver == "ASIO");
}

TEST_CASE("PlanDeviceSwitch: device exists under current AND another driver — stays on current",
          "[PlanDeviceSwitch]") {
    // When the same device name appears under multiple drivers
    // including currentDriver, the resolved driver is currentDriver.
    // The unscoped locateDevice would return the first matching entry
    // (always a WASAPI variant on Windows by JUCE's enumeration
    // order); planDeviceSwitch must scope the lookup to currentDriver
    // first so a DirectSound session stays on DirectSound.
    DevTable table = {
        {"Windows Audio",                    "Microphone (Realtek Audio)"},
        {"Windows Audio (Exclusive Mode)",   "Microphone (Realtek Audio)"},
        {"Windows Audio (Low Latency Mode)", "Microphone (Realtek Audio)"},
        {"DirectSound",                      "Microphone (Realtek Audio)"},
    };
    auto plan = planDeviceSwitch("DirectSound", "Microphone (Realtek Audio)", table);
    REQUIRE(plan.deviceFound);
    REQUIRE_FALSE(plan.needsTypeSwitch);
    REQUIRE(plan.targetDriver == "DirectSound");
    REQUIRE(plan.targetDevice == "Microphone (Realtek Audio)");
}

TEST_CASE("PlanDeviceSwitch: shared name — current=Windows Audio also stays",
          "[PlanDeviceSwitch]") {
    // Same invariant from the other direction. With currentDriver=
    // "Windows Audio", the resolved driver is "Windows Audio" — the
    // scope rule applies regardless of which driver the user is on.
    DevTable table = {
        {"Windows Audio", "Microphone (Realtek Audio)"},
        {"DirectSound",   "Microphone (Realtek Audio)"},
    };
    auto plan = planDeviceSwitch("Windows Audio", "Microphone (Realtek Audio)", table);
    REQUIRE(plan.deviceFound);
    REQUIRE_FALSE(plan.needsTypeSwitch);
    REQUIRE(plan.targetDriver == "Windows Audio");
}

TEST_CASE("PlanDeviceSwitch: ASIO pick from DirectSound — refused, not auto-flipped",
          "[PlanDeviceSwitch]") {
    // ASIO is full-duplex single-device but the scope rule still
    // applies: an ASIO device name resolved from a non-ASIO
    // currentDriver returns deviceFound=false. No special-case
    // bypass for ASIO.
    DevTable table = {
        {"Windows Audio", "Speakers (Realtek)"},
        {"DirectSound",   "Primary Sound Driver"},
        {"ASIO",          "Focusrite USB ASIO"},
    };
    auto plan = planDeviceSwitch("DirectSound", "Focusrite USB ASIO", table);
    REQUIRE_FALSE(plan.deviceFound);
    REQUIRE_FALSE(plan.needsTypeSwitch);
    REQUIRE(plan.targetDriver.empty());
}

TEST_CASE("PlanDeviceSwitch: cold-init (currentDriver empty) — accepts any driver",
          "[PlanDeviceSwitch]") {
    // currentDriver=="" disables the scope rule (cold-init / boot,
    // -H, saved-config restoration). The lookup falls back to the
    // full table and needsTypeSwitch is set.
    DevTable table = {
        {"Windows Audio", "Speakers"},
        {"ASIO",          "Focusrite USB ASIO"},
    };
    auto plan = planDeviceSwitch("", "Focusrite USB ASIO", table);
    REQUIRE(plan.deviceFound);
    REQUIRE(plan.needsTypeSwitch);
    REQUIRE(plan.targetDriver == "ASIO");
    REQUIRE(plan.targetDevice == "Focusrite USB ASIO");
}

TEST_CASE("PlanDeviceSwitch: tolerates JUCE '<base> (N)' suffix under current driver",
          "[PlanDeviceSwitch]") {
    // The scoped (currentDriver-filtered) lookup applies the same
    // "<base> (N)" disambiguation rule as the global lookup.
    DevTable table = {
        {"DirectSound",   "USB Audio"},
        {"DirectSound",   "USB Audio (2)"},
        {"Windows Audio", "USB Audio"},
    };
    auto plan = planDeviceSwitch("DirectSound", "USB Audio (2)", table);
    REQUIRE(plan.deviceFound);
    REQUIRE_FALSE(plan.needsTypeSwitch);
    REQUIRE(plan.targetDriver == "DirectSound");
    REQUIRE(plan.targetDevice == "USB Audio (2)");
}

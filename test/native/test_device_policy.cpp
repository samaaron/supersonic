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

// =============================================================================
// resolveAggregateRate
//
// After the engine TRIES to set both aggregate sub-devices to the desired
// (remembered) rate, this decides the rate to actually run at. The output
// is the clock master / audible path, so we run at the rate it settled on;
// forcing a rate the output doesn't share causes aggregate-level SRC
// distortion (and changes the system rate for nothing).
// =============================================================================

static double aggRate(double desired, double in, double out) {
    return sonicpi::device::resolveAggregateRate(desired, in, out);
}

TEST_CASE("AggregateRate: output accepted the desired rate → use it",
          "[AggregateRate]") {
    // Remembered 44.1k, both sub-devices took it. Honour it.
    REQUIRE(aggRate(44100, 44100, 44100) == 44100);
}

TEST_CASE("AggregateRate: both refused but agree → adopt their shared rate",
          "[AggregateRate]") {
    // Built-in Speakers+Mic pinned at 48k, remembered pref 44.1k. Don't
    // force 44.1k over 48k (SRC); run 48k.
    REQUIRE(aggRate(44100, 48000, 48000) == 48000);
}

TEST_CASE("AggregateRate: sub-devices disagree → output (clock master) wins",
          "[AggregateRate]") {
    // Bluetooth HFP mic forced to 16k while the output is 48k. Run at the
    // output's 48k (no output-side SRC); the 16k input is resampled to match
    // — unavoidable for such a device, and only affects the input path.
    REQUIRE(aggRate(48000, 16000, 48000) == 48000);
    // Even if the input happened to take the desired rate, the output is
    // the master: a 44.1k input against a 48k output still runs at 48k.
    REQUIRE(aggRate(44100, 44100, 48000) == 48000);
}

TEST_CASE("AggregateRate: output rate unreadable → fall back to input",
          "[AggregateRate]") {
    REQUIRE(aggRate(44100, 44100, 0) == 44100);
}

TEST_CASE("AggregateRate: nothing readable → fall back to desired",
          "[AggregateRate]") {
    REQUIRE(aggRate(44100, 0, 0) == 44100);
}

// =============================================================================
// shouldFollowDefaultOutputChange
//
// The CoreAudio default-output listener fires whenever the system default
// changes — including changes our own aggregate create/destroy provokes.
// Following the wrong target cold-swaps + rebuilds the aggregate, which
// perturbs the device list and re-fires the listener: the storm/freeze.
// These cases lock in what we will and won't chase.
// =============================================================================

static bool follow(const std::string& nd, const std::string& cur, bool virt,
                   const std::string& selfPrefix = "SuperSonic") {
    return sonicpi::device::shouldFollowDefaultOutputChange(nd, cur, virt,
                                                            selfPrefix);
}

TEST_CASE("FollowDefault: real hardware device, not current → follow",
          "[FollowDefault]") {
    REQUIRE(follow("External Headphones", "MacBook Pro Speakers", false));
}

TEST_CASE("FollowDefault: virtual device (NDI/Loopback) → do NOT follow",
          "[FollowDefault]") {
    // NDI Audio (virtual) became the system default — must not be chased.
    REQUIRE_FALSE(follow("NDI Audio", "MacBook Pro Speakers", true));
    REQUIRE_FALSE(follow("Loopback Audio", "iRig USB", true));
}

TEST_CASE("FollowDefault: new default is already the current output → no-op",
          "[FollowDefault]") {
    REQUIRE_FALSE(follow("MacBook Pro Speakers", "MacBook Pro Speakers", false));
}

TEST_CASE("FollowDefault: our own SuperSonic aggregate elevated to default → ignore",
          "[FollowDefault]") {
    // Creating an aggregate can briefly make it the system default.
    REQUIRE_FALSE(follow("SuperSonic#7", "MacBook Pro Speakers", false));
}

TEST_CASE("FollowDefault: empty new-default name → ignore",
          "[FollowDefault]") {
    REQUIRE_FALSE(follow("", "MacBook Pro Speakers", false));
}

TEST_CASE("FollowDefault: self-aggregate detection follows the app name",
          "[FollowDefault]") {
    // The aggregate is named "<appName>#N" (AggregateDeviceHelper). An
    // embedder that renames the app must still have its own aggregate
    // ignored — the literal "SuperSonic" must not be baked in.
    REQUIRE_FALSE(follow("MyLoopApp#3", "MacBook Pro Speakers", false,
                         "MyLoopApp"));
    REQUIRE_FALSE(follow("SuperSonic#7", "MacBook Pro Speakers", false,
                         "SuperSonic"));
}

TEST_CASE("FollowDefault: real device sharing the app-name prefix is followed",
          "[FollowDefault]") {
    // Only "<prefix>#N" is ours. A hardware device that merely starts with
    // the app name (or the app name itself, however unlikely) is a real
    // target, not our aggregate.
    REQUIRE(follow("SuperSonic Audio Interface", "MacBook Pro Speakers", false,
                   "SuperSonic"));
}

TEST_CASE("FollowDefault: empty self-prefix never matches",
          "[FollowDefault]") {
    REQUIRE(follow("#1 DAC", "MacBook Pro Speakers", false, ""));
}

// =============================================================================
// resolveInputWidth
//
// One answer to "how many input channels do we actually request". Both
// switchDevice's auto-enable block and enableInputChannels resolve the -1
// sentinel against the boot -i flag, and the result must be clamped to the
// device's probed capacity: WASAPI rejects setAudioDeviceSetup outright
// when asked for more inputs than exist (CoreAudio silently clamps, which
// is how the unclamped kRequestMaxChannels path shipped). Previously
// enableInputChannels resolved WITHOUT clamping — the Windows boot path
// (enableInputChannels(-1) from Main) sent 64 input bits into WASAPI.
// =============================================================================

static int width(int requested, int boot, int probed) {
    return sonicpi::device::resolveInputWidth(requested, boot, probed);
}

TEST_CASE("InputWidth: auto sentinel + boot auto-max clamps to probed count",
          "[InputWidth]") {
    // THE Windows boot bug: -i -1 → kRequestMaxChannels must not survive
    // a successful probe.
    REQUIRE(width(-1, -1, 2) == 2);
    REQUIRE(width(-1, -1, 8) == 8);
}

TEST_CASE("InputWidth: auto sentinel + boot auto-max + probe failed → request max",
          "[InputWidth]") {
    // Unknown capacity: keep the over-request (CoreAudio clamps; nothing
    // better is knowable).
    REQUIRE(width(-1, -1, -1) == sonicpi::device::kRequestMaxChannels);
    REQUIRE(width(-1, -1, 0) == sonicpi::device::kRequestMaxChannels);
}

TEST_CASE("InputWidth: auto sentinel honours an explicit boot count",
          "[InputWidth]") {
    REQUIRE(width(-1, 4, -1) == 4);
    // ...still clamped when the device has fewer.
    REQUIRE(width(-1, 4, 2) == 2);
}

TEST_CASE("InputWidth: auto sentinel + boot disabled inputs → stereo default",
          "[InputWidth]") {
    REQUIRE(width(-1, 0, -1) == 2);
    REQUIRE(width(-1, 0, 1) == 1);
}

TEST_CASE("InputWidth: explicit request clamps to probed capacity",
          "[InputWidth]") {
    REQUIRE(width(4, -1, 2) == 2);
    REQUIRE(width(4, -1, 8) == 4);
    REQUIRE(width(4, -1, -1) == 4);
}

TEST_CASE("InputWidth: explicit zero (disable) is never touched",
          "[InputWidth]") {
    REQUIRE(width(0, -1, 2) == 0);
    REQUIRE(width(0, 0, -1) == 0);
}

// =============================================================================
// deviceNameVisible
//
// A just-created CoreAudio aggregate isn't in JUCE's device list until it
// rescans; opening it before then errors "No such device". The engine polls
// scanForDevices() and uses this to gate the open. These lock in the
// match/suffix/absent rules the poll depends on.
// =============================================================================

static bool visible(const std::string& n, std::vector<std::string> names) {
    return sonicpi::device::deviceNameVisible(n, names);
}

TEST_CASE("DeviceVisible: exact match present → visible (safe to open)",
          "[DeviceVisible]") {
    REQUIRE(visible("SuperSonic#7", {"MacBook Pro Speakers", "SuperSonic#7"}));
}

TEST_CASE("DeviceVisible: absent → not visible (keep polling, don't open)",
          "[DeviceVisible]") {
    REQUIRE_FALSE(visible("SuperSonic#7", {"MacBook Pro Speakers"}));
}

TEST_CASE("DeviceVisible: tolerates JUCE '<name> (N)' disambiguation suffix",
          "[DeviceVisible]") {
    REQUIRE(visible("USB Audio", {"USB Audio (2)"}));
}

TEST_CASE("DeviceVisible: empty list or empty name → not visible",
          "[DeviceVisible]") {
    REQUIRE_FALSE(visible("SuperSonic#1", {}));
    REQUIRE_FALSE(visible("", {"SuperSonic#1"}));
}

TEST_CASE("DeviceVisible: does not false-match a longer base name",
          "[DeviceVisible]") {
    // "USB Audio" must NOT be considered visible just because "USB Audio Pro"
    // is present (same stricter rule resolveJuceDeviceName enforces).
    REQUIRE_FALSE(visible("USB Audio", {"USB Audio Pro"}));
}

// =============================================================================
// usableAggregateRates
//
// The macOS rate dropdown collapses to the current rate when on an aggregate.
// This decides what it *should* offer: the rates both sub-devices support,
// so we never offer a rate that forces aggregate-internal SRC. These lock in
// the intersection + fallbacks.
// =============================================================================

static std::vector<int> aggRates(std::vector<int> out, std::vector<int> in) {
    return sonicpi::device::usableAggregateRates(out, in);
}

TEST_CASE("UsableAggRates: both support the same set → that set",
          "[UsableAggRates]") {
    REQUIRE(aggRates({44100, 48000, 88200, 96000},
                     {44100, 48000, 88200, 96000})
            == std::vector<int>{44100, 48000, 88200, 96000});
}

TEST_CASE("UsableAggRates: intersection when input supports fewer",
          "[UsableAggRates]") {
    // Built-in speakers do 44.1–96k; a mic that only does 44.1/48 →
    // offer just the shared 44.1/48.
    REQUIRE(aggRates({44100, 48000, 88200, 96000}, {44100, 48000})
            == std::vector<int>{44100, 48000});
}

TEST_CASE("UsableAggRates: disjoint (BT 16k mic vs 48k out) → output rates",
          "[UsableAggRates]") {
    // The output is the clock master / audible path; offer its rates and let
    // the odd-rate input be resampled (unavoidable for a 16k HFP mic).
    REQUIRE(aggRates({44100, 48000}, {16000})
            == std::vector<int>{44100, 48000});
}

TEST_CASE("UsableAggRates: no input sub-device → output rates",
          "[UsableAggRates]") {
    REQUIRE(aggRates({44100, 48000}, {}) == std::vector<int>{44100, 48000});
}

TEST_CASE("UsableAggRates: output list empty → fall back to input",
          "[UsableAggRates]") {
    REQUIRE(aggRates({}, {44100, 48000}) == std::vector<int>{44100, 48000});
}

// =============================================================================
// resolveSwapScope
//
// Which driver a switchDevice call resolves its device names under, and
// whether it abandons a pending switchDriver intent. The intent is a USER
// concept: the user picked Driver=ASIO but no ASIO device is open yet, so
// the next user device pick scopes under ASIO. Engine-internal traffic
// (recovery reopen after a failed swap, hotplug re-attach) must neither
// consume that intent nor scope under it — otherwise a failed swap whose
// recovery lands on the system default silently eats the pending pick,
// and the user's next device pick is refused against the wrong driver.
// =============================================================================

using SS = sonicpi::device::SwapScopeDecision;

namespace {
// Each pick resolves under exactly one driver: the MOTU only under ASIO,
// the remote-desktop redirect device only under Windows Audio.
const std::vector<std::pair<std::string, std::string>> kScopeTable = {
    {"Windows Audio", "Remote Audio"},
    {"DirectSound",   "Primary Sound Driver"},
    {"ASIO",          "MOTU Pro Audio"},
};
} // namespace

static SS scope(bool user, const std::string& intended,
                const std::string& current,
                const std::string& out, const std::string& in) {
    return sonicpi::device::resolveSwapScope(
        user, intended, current, out, in, kScopeTable);
}

TEST_CASE("SwapScope: internal recovery reopen keeps the pending intent",
          "[SwapScope]") {
    // Recovery reopens the system default on the actual driver. Scope
    // under what's actually open; the user's ASIO pick stays pending.
    auto d = scope(false, "ASIO", "Windows Audio", "Remote Audio", "");
    REQUIRE(d.scopedDriver == "Windows Audio");
    REQUIRE_FALSE(d.abandonIntent);
}

TEST_CASE("SwapScope: internal call with no intent scopes to the current driver",
          "[SwapScope]") {
    auto d = scope(false, "", "Windows Audio", "Remote Audio", "");
    REQUIRE(d.scopedDriver == "Windows Audio");
    REQUIRE_FALSE(d.abandonIntent);
}

TEST_CASE("SwapScope: user pick under the intended driver scopes to the intent",
          "[SwapScope]") {
    // Driver=ASIO pending, user picks the ASIO device: the two-step
    // driver→device flow commits.
    auto d = scope(true, "ASIO", "Windows Audio", "MOTU Pro Audio", "");
    REQUIRE(d.scopedDriver == "ASIO");
    REQUIRE_FALSE(d.abandonIntent);
}

TEST_CASE("SwapScope: user pick on the current driver abandons the intent",
          "[SwapScope]") {
    // Driver=ASIO pending but the user picks a current-driver device —
    // they've implicitly walked away from the driver swap.
    auto d = scope(true, "ASIO", "Windows Audio", "Remote Audio", "");
    REQUIRE(d.scopedDriver == "Windows Audio");
    REQUIRE(d.abandonIntent);
}

TEST_CASE("SwapScope: user rate/buffer-only change keeps the intent",
          "[SwapScope]") {
    // Empty and sentinel names aren't device picks; they resolve anywhere.
    auto d = scope(true, "ASIO", "Windows Audio", "", "");
    REQUIRE(d.scopedDriver == "ASIO");
    REQUIRE_FALSE(d.abandonIntent);

    d = scope(true, "ASIO", "Windows Audio", "__system__", "__none__");
    REQUIRE(d.scopedDriver == "ASIO");
    REQUIRE_FALSE(d.abandonIntent);
}

TEST_CASE("SwapScope: user pick resolving under neither driver keeps the "
          "intent scope", "[SwapScope]") {
    // Unknown name: scope stays on the intent so the refusal names the
    // driver the user actually chose.
    auto d = scope(true, "ASIO", "Windows Audio", "Ghost Device", "");
    REQUIRE(d.scopedDriver == "ASIO");
    REQUIRE_FALSE(d.abandonIntent);
}

TEST_CASE("SwapScope: no pending intent scopes to the current driver",
          "[SwapScope]") {
    auto d = scope(true, "", "Windows Audio", "Remote Audio", "");
    REQUIRE(d.scopedDriver == "Windows Audio");
    REQUIRE_FALSE(d.abandonIntent);
}

TEST_CASE("SwapScope: input name participates in the resolution",
          "[SwapScope]") {
    // Input-only pick that lives under the intended driver: commit path.
    auto d = scope(true, "ASIO", "Windows Audio", "", "MOTU Pro Audio");
    REQUIRE(d.scopedDriver == "ASIO");
    REQUIRE_FALSE(d.abandonIntent);
}

// ── Exclusive duplex pair resolution ─────────────────────────────────────────
// The PipeWire patchbay's input and output sides live on one filter node,
// so it cannot be half-paired with a stream device. A request that would
// produce a mixed pair must resolve to an explicit, truthful pair — never
// silently override the side the user just changed (sonic-pi #3553
// follow-up: switching output to System Default while the patchbay held
// the input hijacked the output back to the patchbay, then a reconcile
// pass amputated the input).

namespace {
sonicpi::device::ExclusivePair xpair(const std::string& reqOut, const std::string& reqIn,
                                     const std::string& curOut, const std::string& curIn) {
    return sonicpi::device::resolveExclusiveDuplexPair(
        reqOut, reqIn, curOut, curIn, "Patchbay (16 ch)", "System Default");
}
} // namespace

TEST_CASE("ExclusivePair: picking the exclusive device claims both sides",
          "[ExclusivePair]") {
    // Output dropdown pick...
    auto p = xpair("Patchbay (16 ch)", "", "System Default", "System Default");
    REQUIRE(p.output == "Patchbay (16 ch)");
    REQUIRE(p.input == "Patchbay (16 ch)");
    // ...and input dropdown pick (GUI re-sends the unchanged output).
    p = xpair("System Default", "Patchbay (16 ch)", "System Default", "System Default");
    REQUIRE(p.output == "Patchbay (16 ch)");
    REQUIRE(p.input == "Patchbay (16 ch)");
}

TEST_CASE("ExclusivePair: changing output away drops the carried input, "
          "never the output choice", "[ExclusivePair]") {
    // The #3553 follow-up scenario: on patchbay both sides, user picks
    // System Default output; input request is empty (= keep current).
    auto p = xpair("System Default", "", "Patchbay (16 ch)", "Patchbay (16 ch)");
    REQUIRE(p.output == "System Default");
    REQUIRE(p.input == "__none__");
}

TEST_CASE("ExclusivePair: changing input away frees the carried output to "
          "the fallback", "[ExclusivePair]") {
    // On patchbay both sides, user picks a stream input; the GUI re-sends
    // the (unchanged) patchbay output alongside it. The changed side wins.
    auto p = xpair("Patchbay (16 ch)", "Built-in Audio Analog Stereo",
                   "Patchbay (16 ch)", "Patchbay (16 ch)");
    REQUIRE(p.output == "System Default");
    REQUIRE(p.input == "Built-in Audio Analog Stereo");
}

TEST_CASE("ExclusivePair: pairs not involving the exclusive device pass "
          "through untouched", "[ExclusivePair]") {
    auto p = xpair("System Default", "", "Built-in Audio Analog Stereo", "");
    REQUIRE(p.output == "System Default");
    REQUIRE(p.input == "");
    p = xpair("", "Built-in Audio Analog Stereo", "System Default", "System Default");
    REQUIRE(p.output == "");
    REQUIRE(p.input == "Built-in Audio Analog Stereo");
}

TEST_CASE("ExclusivePair: staying on the exclusive device passes through",
          "[ExclusivePair]") {
    // Re-selecting or rate/buffer-only changes while on the patchbay.
    auto p = xpair("Patchbay (16 ch)", "Patchbay (16 ch)",
                   "Patchbay (16 ch)", "Patchbay (16 ch)");
    REQUIRE(p.output == "Patchbay (16 ch)");
    REQUIRE(p.input == "Patchbay (16 ch)");
    p = xpair("Patchbay (16 ch)", "", "Patchbay (16 ch)", "Patchbay (16 ch)");
    REQUIRE(p.output == "Patchbay (16 ch)");
    REQUIRE(p.input == "Patchbay (16 ch)");
}

TEST_CASE("ExclusivePair: empty exclusive name disables the policy",
          "[ExclusivePair]") {
    auto p = sonicpi::device::resolveExclusiveDuplexPair(
        "System Default", "", "Patchbay (16 ch)", "Patchbay (16 ch)", "", "System Default");
    REQUIRE(p.output == "System Default");
    REQUIRE(p.input == "");
}

TEST_CASE("ExclusivePair: exclusive output with inputs disabled is legal, "
          "not a conflict", "[ExclusivePair]") {
    // Re-picking the patchbay output while inputs are off must not force
    // the output anywhere; inputs stay as requested.
    auto p = xpair("Patchbay (16 ch)", "", "Patchbay (16 ch)", "");
    REQUIRE(p.output == "Patchbay (16 ch)");
    REQUIRE(p.input == "");
    p = xpair("Patchbay (16 ch)", "__none__", "System Default", "System Default");
    REQUIRE(p.output == "Patchbay (16 ch)");
    REQUIRE(p.input == "__none__");
}

// ── Device-table capability annotation ───────────────────────────────────────
// The device table is the single source of truth for the GUI's dropdowns:
// every row the user can pick exists in the table, and semantics ride on
// per-device capability flags instead of client-side synthesis or name
// sentinels. Drivers with a native default-follow device (PipeWire's
// "System Default") get it flagged; drivers without one get a synthetic
// flagged row contributed by the engine, so the GUI never invents rows.

namespace {
sonicpi::device::DriverTableAnnotation annotate(const std::string& driver,
                                                const std::vector<std::string>& outputs) {
    return sonicpi::device::annotateDriverOutputs(
        driver, outputs, "PipeWire", "System Default", "Patchbay (16 ch)");
}
} // namespace

TEST_CASE("TableAnnotation: native driver flags its own default and patchbay",
          "[DeviceTable]") {
    auto a = annotate("PipeWire", { "System Default", "Built-in Audio", "Patchbay (16 ch)" });
    REQUIRE_FALSE(a.insertSyntheticDefault);
    REQUIRE(a.flags.size() == 3);
    REQUIRE(a.flags[0] == "follows-default");
    REQUIRE(a.flags[1] == "");
    REQUIRE(a.flags[2] == "exclusive-duplex");
}

TEST_CASE("TableAnnotation: driver without native default gets a synthetic row",
          "[DeviceTable]") {
    auto a = annotate("ALSA", { "PipeWire Sound Server", "HDA Intel" });
    REQUIRE(a.insertSyntheticDefault);
    REQUIRE(a.syntheticName == "System Default");
    REQUIRE(a.syntheticFlags == "follows-default,synthetic");
    REQUIRE(a.flags.size() == 2);
    REQUIRE(a.flags[0] == "");
}

TEST_CASE("TableAnnotation: ASIO never gets a synthetic default",
          "[DeviceTable]") {
    // ASIO has no OS-default concept — each driver IS its device.
    auto a = annotate("ASIO", { "MOTU Pro Audio" });
    REQUIRE_FALSE(a.insertSyntheticDefault);
}

TEST_CASE("TableAnnotation: patchbay name in a foreign driver is not special",
          "[DeviceTable]") {
    // Only the native driver's device carries the capability; an ALSA
    // device that happens to share the name must not inherit it.
    auto a = annotate("ALSA", { "Patchbay (16 ch)" });
    REQUIRE(a.flags[0] == "");
}

// =============================================================================
// chooseBootInputDevice
// =============================================================================
// The daemon hands the user's saved input via -H (see parseHardwareFlag);
// boot's aggregate promotion pairs the opened output with this choice.
// Before it existed, boot always paired the system default input and the
// user's saved input arrived one cold swap later (a whole second studio
// boot).

static std::string chooseInput(const std::string& requested,
                               const std::string& fallback,
                               const std::vector<std::string>& visible) {
    return sonicpi::device::chooseBootInputDevice(requested, fallback, visible);
}

TEST_CASE("BootInput: no request = system default", "[BootInput]") {
    REQUIRE(chooseInput("", "MacBook Pro Microphone",
                        { "MacBook Pro Microphone", "Loopback Audio" })
            == "MacBook Pro Microphone");
}

TEST_CASE("BootInput: __none__ sentinel = system default", "[BootInput]") {
    // Input disablement travels as -i 0; the pairing choice just falls back.
    REQUIRE(chooseInput("__none__", "MacBook Pro Microphone",
                        { "MacBook Pro Microphone", "Loopback Audio" })
            == "MacBook Pro Microphone");
}

TEST_CASE("BootInput: requested input visible = requested wins", "[BootInput]") {
    REQUIRE(chooseInput("Loopback Audio", "MacBook Pro Microphone",
                        { "MacBook Pro Microphone", "Loopback Audio" })
            == "Loopback Audio");
}

TEST_CASE("BootInput: JUCE-suffixed form of the requested name matches",
          "[BootInput]") {
    REQUIRE(chooseInput("USB Audio", "MacBook Pro Microphone",
                        { "MacBook Pro Microphone", "USB Audio (2)" })
            == "USB Audio (2)");
}

TEST_CASE("BootInput: requested input unplugged = system default", "[BootInput]") {
    // The stale pref is the GUI's to notice and clear; boot must still
    // come up with a working input rather than none.
    REQUIRE(chooseInput("MOTU M4", "MacBook Pro Microphone",
                        { "MacBook Pro Microphone", "Loopback Audio" })
            == "MacBook Pro Microphone");
}

TEST_CASE("BootInput: suffix match must not fuzzy-match a longer name",
          "[BootInput]") {
    // "USB Audio Pro" is not the "<USB Audio> (N)" form — same rule as
    // resolveJuceDeviceName.
    REQUIRE(chooseInput("USB Audio", "MacBook Pro Microphone",
                        { "MacBook Pro Microphone", "USB Audio Pro" })
            == "MacBook Pro Microphone");
}

// Suitability mask (parallel to visibleInputs, selectBootOutputDevice-style):
// switchDevice never aggregates a wireless input (HFP 16 kHz mono; CoreAudio
// IOProc freeze), so boot pairing must apply the same vetting — a saved
// Bluetooth input pref falls back to the system default instead of
// deterministically rebuilding the bad aggregate on every boot.

static std::string chooseInput(const std::string& requested,
                               const std::string& fallback,
                               const std::vector<std::string>& visible,
                               const std::vector<bool>& suitable) {
    return sonicpi::device::chooseBootInputDevice(requested, fallback,
                                                  visible, suitable);
}

TEST_CASE("BootInput: unsuitable (wireless) requested input is not paired",
          "[BootInput]") {
    REQUIRE(chooseInput("AirPods Pro", "MacBook Pro Microphone",
                        { "MacBook Pro Microphone", "AirPods Pro" },
                        { true, false })
            == "MacBook Pro Microphone");
}

TEST_CASE("BootInput: suitable requested input still wins under a mask",
          "[BootInput]") {
    REQUIRE(chooseInput("MOTU M4", "MacBook Pro Microphone",
                        { "MacBook Pro Microphone", "MOTU M4" },
                        { true, true })
            == "MOTU M4");
}

TEST_CASE("BootInput: suffixed resolved form is judged by its own mask slot",
          "[BootInput]") {
    // "USB Audio" resolves to "USB Audio (2)"; that entry is the unsuitable
    // one, so the pref is dropped.
    REQUIRE(chooseInput("USB Audio", "MacBook Pro Microphone",
                        { "MacBook Pro Microphone", "USB Audio (2)" },
                        { true, false })
            == "MacBook Pro Microphone");
}

TEST_CASE("BootInput: mismatched mask length = treated as all-suitable",
          "[BootInput]") {
    // Defensive: a caller bug in building the mask must not veto a good
    // pairing.
    REQUIRE(chooseInput("MOTU M4", "MacBook Pro Microphone",
                        { "MacBook Pro Microphone", "MOTU M4" },
                        { true })
            == "MOTU M4");
}

// =============================================================================
// parseHardwareFlag
// =============================================================================
// scsynth's -H takes one or two device names; upstream (scsynth_main.cpp)
// reads two-name as "<input> <output>" and mirrors a single name into BOTH
// directions. Sonic Pi's daemon relies on all three shapes: -H <in> <out>,
// -H <in> (input-only pref), -H <out> (output-only pref). The second argv
// token only counts as a name when it's non-empty and not flag-shaped.

using sonicpi::device::parseHardwareFlag;

TEST_CASE("HFlag: single name serves both directions", "[HFlag]") {
    // daemon.rb sends `-H <input>` when only an input pref is saved —
    // upstream semantics apply it to input AND output.
    auto r = parseHardwareFlag("MOTU M4", nullptr);
    REQUIRE(r.outputDevice == "MOTU M4");
    REQUIRE(r.inputDevice  == "MOTU M4");
    REQUIRE(!r.secondTokenUsed);
}

TEST_CASE("HFlag: single name followed by a flag token", "[HFlag]") {
    auto r = parseHardwareFlag("MOTU M4", "-u");
    REQUIRE(r.outputDevice == "MOTU M4");
    REQUIRE(r.inputDevice  == "MOTU M4");
    REQUIRE(!r.secondTokenUsed);
}

TEST_CASE("HFlag: two names are input then output", "[HFlag]") {
    auto r = parseHardwareFlag("MOTU M4", "MacBook Pro Speakers");
    REQUIRE(r.inputDevice  == "MOTU M4");
    REQUIRE(r.outputDevice == "MacBook Pro Speakers");
    REQUIRE(r.secondTokenUsed);
}

TEST_CASE("HFlag: empty second token is not a device name", "[HFlag]") {
    // A quoted empty shell var (`-H "Speakers" ""`) must not become the
    // output device — ""[0] is '\0', not '-'.
    auto r = parseHardwareFlag("MacBook Pro Speakers", "");
    REQUIRE(r.outputDevice == "MacBook Pro Speakers");
    REQUIRE(r.inputDevice  == "MacBook Pro Speakers");
    REQUIRE(!r.secondTokenUsed);
}

TEST_CASE("HFlag: __system__ output sentinel is not mirrored to input",
          "[HFlag]") {
    // "Follow the system default output" says nothing about input.
    auto r = parseHardwareFlag("__system__", nullptr);
    REQUIRE(r.outputDevice == "__system__");
    REQUIRE(r.inputDevice.empty());
}

TEST_CASE("HFlag: __none__ disables input without hijacking output", "[HFlag]") {
    auto r = parseHardwareFlag("__none__", nullptr);
    REQUIRE(r.inputDevice == "__none__");
    REQUIRE(r.outputDevice.empty());
}

TEST_CASE("HFlag: two-name form passes sentinels through", "[HFlag]") {
    auto r = parseHardwareFlag("__none__", "MacBook Pro Speakers");
    REQUIRE(r.inputDevice  == "__none__");
    REQUIRE(r.outputDevice == "MacBook Pro Speakers");
    REQUIRE(r.secondTokenUsed);
}

// =============================================================================
// resolveSwapTarget / SwapScope
//
// Promoted from switchDevice's `considerName` lambda — the cross-driver
// resolution algorithm that was function-local state until it caused a
// cross-platform build break (init needed the same answer and couldn't
// reach it). Resolves one requested name against the scoped driver and
// latches cross-driver state first-wins into a SwapScope.
// =============================================================================

using sonicpi::device::SwapScope;
using sonicpi::device::resolveSwapTarget;

static const std::vector<std::pair<std::string, std::string>> kTwoDriverTable = {
    {"CoreAudio", "MacBook Pro Speakers"},
    {"CoreAudio", "External Headphones"},
    {"ASIO",      "MOTU Pro Audio"},
};

TEST_CASE("SwapTarget: sentinels and empty resolve to no-op", "[SwapTarget]") {
    SwapScope scope;
    REQUIRE(resolveSwapTarget("", "CoreAudio", "CoreAudio",
                              kTwoDriverTable, scope).empty());
    REQUIRE(resolveSwapTarget("__system__", "CoreAudio", "CoreAudio",
                              kTwoDriverTable, scope).empty());
    REQUIRE(resolveSwapTarget("__none__", "CoreAudio", "CoreAudio",
                              kTwoDriverTable, scope).empty());
    REQUIRE(!scope.crossDriver);
}

TEST_CASE("SwapTarget: in-driver device resolves without cross-driver",
          "[SwapTarget]") {
    SwapScope scope;
    auto err = resolveSwapTarget("External Headphones", "CoreAudio",
                                 "CoreAudio", kTwoDriverTable, scope);
    REQUIRE(err.empty());
    REQUIRE(!scope.crossDriver);
}

TEST_CASE("SwapTarget: unknown name reports the scoped driver in the error",
          "[SwapTarget]") {
    // Exact wording is a contract — the GUI surfaces it verbatim.
    SwapScope scope;
    auto err = resolveSwapTarget("Ghost Device", "CoreAudio", "CoreAudio",
                                 kTwoDriverTable, scope);
    REQUIRE(err == "device 'Ghost Device' not available on driver 'CoreAudio'");
    SwapScope scope2;
    auto err2 = resolveSwapTarget("Ghost Device", "", "", kTwoDriverTable,
                                  scope2);
    REQUIRE(err2 == "device 'Ghost Device' not available on driver '(none)'");
}

TEST_CASE("SwapTarget: pending-intent scope resolves under the intended driver "
          "and latches cross-driver against JUCE's actual type",
          "[SwapTarget]") {
    // Two-step driver→device flow: switchDriver recorded intent 'ASIO',
    // JUCE still has CoreAudio open. The device lookup scopes to the
    // intent; the type-switch decision compares against JUCE's reality.
    SwapScope scope;
    auto err = resolveSwapTarget("MOTU Pro Audio", "ASIO", "CoreAudio",
                                 kTwoDriverTable, scope);
    REQUIRE(err.empty());
    REQUIRE(scope.crossDriver);
    REQUIRE(scope.targetDriver == "ASIO");
    REQUIRE(scope.targetDevice == "MOTU Pro Audio");
}

TEST_CASE("SwapTarget: cross-driver latch is first-wins", "[SwapTarget]") {
    // Output already latched the cross; a second name (the input) must not
    // overwrite the target driver/device.
    SwapScope scope;
    REQUIRE(resolveSwapTarget("MOTU Pro Audio", "ASIO", "CoreAudio",
                              kTwoDriverTable, scope).empty());
    const std::string firstDriver = scope.targetDriver;
    const std::string firstDevice = scope.targetDevice;
    // Cold-init global scope ("") lets a CoreAudio name resolve too.
    REQUIRE(resolveSwapTarget("External Headphones", "", "CoreAudio",
                              kTwoDriverTable, scope).empty());
    REQUIRE(scope.crossDriver);
    REQUIRE(scope.targetDriver == firstDriver);
    REQUIRE(scope.targetDevice == firstDevice);
}

// =============================================================================
// sameDeviceName
//
// THE device-name identity predicate. JUCE appends " (N)" when CoreAudio
// reports duplicate device names; "same device" must mean equal modulo
// that suffix — in either direction, digits only. This replaces the loose
// prefix-space rule that lived (twice) beside the strict one: the loose
// rule matched "USB Audio Pro" against "USB Audio", so the hotplug
// auto-switch could treat a DIFFERENT physical device as the preferred
// one. One rule, one implementation, everywhere.
// =============================================================================

using sonicpi::device::sameDeviceName;

TEST_CASE("SameDevice: exact names match", "[SameDevice]") {
    REQUIRE(sameDeviceName("MacBook Pro Speakers", "MacBook Pro Speakers"));
    REQUIRE_FALSE(sameDeviceName("MacBook Pro Speakers", "MacBook Air Speakers"));
}

TEST_CASE("SameDevice: JUCE duplicate suffix matches in either direction",
          "[SameDevice]") {
    REQUIRE(sameDeviceName("USB Audio (2)", "USB Audio"));
    REQUIRE(sameDeviceName("USB Audio", "USB Audio (2)"));
    REQUIRE(sameDeviceName("USB Audio (17)", "USB Audio"));
}

TEST_CASE("SameDevice: a longer real name is NOT the same device",
          "[SameDevice]") {
    // The loose rule's false positive: a different product sharing a
    // prefix. Must not match.
    REQUIRE_FALSE(sameDeviceName("USB Audio Pro", "USB Audio"));
    REQUIRE_FALSE(sameDeviceName("USB Audio", "USB Audio Pro"));
}

TEST_CASE("SameDevice: suffix must be digits in parens", "[SameDevice]") {
    REQUIRE_FALSE(sameDeviceName("USB Audio (two)", "USB Audio"));
    REQUIRE_FALSE(sameDeviceName("USB Audio ()", "USB Audio"));
    REQUIRE_FALSE(sameDeviceName("USB Audio (2", "USB Audio"));
}

TEST_CASE("SameDevice: empty never matches non-empty", "[SameDevice]") {
    REQUIRE_FALSE(sameDeviceName("", "USB Audio"));
    REQUIRE_FALSE(sameDeviceName("USB Audio", ""));
    REQUIRE(sameDeviceName("", ""));
}

// =============================================================================
// DeviceInfo aggregate suitability
//
// One predicate answers "can this device be half of an aggregate":
// wireless (Bluetooth/AirPlay) is out — HAL can't open it and HFP mode
// wrecks rates. Virtual (Loopback/BlackHole/NDI) is deliberately IN:
// aggregates work when the hardware sub-device is the clock master
// (AggregateDeviceHelper's master selection; the virtual-output + mic
// recipe is field-verified). A stale comment in switchDevice used to
// claim the opposite — these pin the truth.
// =============================================================================

#include "DeviceInfo.h"

static DeviceInfo withTransport(uint32_t fourCC) {
    DeviceInfo d;
    d.name = "X";
    d.transportType = fourCC;
    return d;
}

TEST_CASE("Aggregate suitability: wireless transports are unsuitable",
          "[DeviceInfo]") {
    REQUIRE_FALSE(withTransport(CoreAudioTransport::kBluetooth)
                      .isSuitableForAggregate());
    REQUIRE_FALSE(withTransport(CoreAudioTransport::kAirPlay)
                      .isSuitableForAggregate());
}

TEST_CASE("Aggregate suitability: virtual transports ARE suitable",
          "[DeviceInfo]") {
    REQUIRE(withTransport(CoreAudioTransport::kVirtual)
                .isSuitableForAggregate());
}

TEST_CASE("Aggregate suitability: plain hardware is suitable",
          "[DeviceInfo]") {
    REQUIRE(withTransport(0x626C746E /* bltn built-in */)
                .isSuitableForAggregate());
    REQUIRE(withTransport(0x75736220 /* usb  */)
                .isSuitableForAggregate());
}

// =============================================================================
// selectReportedDevices
//
// The list-shaping half of sendDeviceReport, extracted pure: which devices
// the GUI is offered. Filter order is contractual and pinned here:
// clutter/wireless split → known-bad-input removal → unpairable-current-
// output clears inputs → (grouped lists snapshot) → dedupe by name with
// active-driver preference → transient-enumeration suppression.
// =============================================================================

using sonicpi::device::selectReportedDevices;

namespace {
DeviceInfo dev(const std::string& name, const std::string& driver,
               int outs, int ins, uint32_t transport = 0) {
    DeviceInfo d;
    d.name = name;
    d.typeName = driver;
    d.maxOutputChannels = outs;
    d.maxInputChannels = ins;
    d.transportType = transport;
    return d;
}
} // namespace

TEST_CASE("ReportSelect: wireless devices are hidden from both lists",
          "[ReportSelect]") {
    auto sel = selectReportedDevices(
        { dev("Speakers", "CoreAudio", 2, 0),
          dev("AirPods", "CoreAudio", 2, 1, CoreAudioTransport::kBluetooth) },
        "Speakers", "", "CoreAudio", {});
    REQUIRE(sel.outputs.size() == 1);
    REQUIRE(sel.outputs[0].name == "Speakers");
    REQUIRE(sel.inputs.empty());
}

TEST_CASE("ReportSelect: known-bad inputs for the current output are hidden",
          "[ReportSelect]") {
    auto sel = selectReportedDevices(
        { dev("Speakers", "CoreAudio", 2, 0),
          dev("Bad Mic", "CoreAudio", 0, 1),
          dev("Good Mic", "CoreAudio", 0, 1) },
        "Speakers", "Good Mic", "CoreAudio", { "Bad Mic" });
    REQUIRE(sel.inputs.size() == 1);
    REQUIRE(sel.inputs[0].name == "Good Mic");
}

TEST_CASE("ReportSelect: unpairable current output clears the input list",
          "[ReportSelect]") {
    // Wireless current output: no separate mic can join it. The GUI's
    // "-- None --" row is client-side; an empty input list here is the
    // deliberate signal.
    auto sel = selectReportedDevices(
        { dev("AirPods", "CoreAudio", 2, 0, CoreAudioTransport::kAirPlay),
          dev("Mic", "CoreAudio", 0, 1) },
        "AirPods", "", "CoreAudio", {});
    REQUIRE(sel.inputs.empty());
    // Virtual current output pairs fine — inputs stay.
    auto sel2 = selectReportedDevices(
        { dev("BlackHole", "CoreAudio", 2, 0, CoreAudioTransport::kVirtual),
          dev("Mic", "CoreAudio", 0, 1) },
        "BlackHole", "", "CoreAudio", {});
    REQUIRE(sel2.inputs.size() == 1);
}

TEST_CASE("ReportSelect: dedupe keeps the active driver's entry first",
          "[ReportSelect]") {
    auto sel = selectReportedDevices(
        { dev("Speakers", "Windows Audio", 2, 0),
          dev("Speakers", "DirectSound", 2, 0),
          dev("Only WASAPI", "Windows Audio", 2, 0) },
        "Speakers", "", "DirectSound", {});
    REQUIRE(sel.outputs.size() == 2);
    REQUIRE(sel.outputs[0].typeName == "DirectSound");  // active driver wins
    REQUIRE(sel.outputs[1].name == "Only WASAPI");      // fallback still listed
    // The grouped (per-driver) lists are NOT deduped.
    REQUIRE(sel.outputsByDriver.size() == 3);
}

TEST_CASE("ReportSelect: transient empty input list suppresses the report",
          "[ReportSelect]") {
    // A current input with no visible inputs = JUCE mid-churn snapshot;
    // reporting it would make the GUI silently deselect the user's mic.
    auto sel = selectReportedDevices(
        { dev("Speakers", "CoreAudio", 2, 0) },
        "Speakers", "Real Mic", "CoreAudio", {});
    REQUIRE(sel.suppressReport);
    auto sel2 = selectReportedDevices(
        { dev("Speakers", "CoreAudio", 2, 0) },
        "Speakers", "", "CoreAudio", {});
    REQUIRE_FALSE(sel2.suppressReport);
}

TEST_CASE("ReportSelect: PipeWire detected via native driver or ALSA compat",
          "[ReportSelect]") {
    auto viaNative = selectReportedDevices(
        { dev("Card", "PipeWire", 2, 0) }, "", "", "PipeWire", {});
    REQUIRE(viaNative.pipewireActive);
    auto viaCompat = selectReportedDevices(
        { dev("PipeWire Sound Server", "ALSA", 2, 0) }, "", "", "ALSA", {});
    REQUIRE(viaCompat.pipewireActive);
    auto without = selectReportedDevices(
        { dev("hw:0", "ALSA", 2, 0) }, "", "", "ALSA", {});
    REQUIRE_FALSE(without.pipewireActive);
}

// =============================================================================
// resolveTargetRate
//
// The pure core of "which rate does a swap land on": keep the current
// session rate when the target device supports it (no cold swap for
// nothing), else the device's nearest supported rate. 0 = keep — the
// caller only cold-swaps on a returned change. One of four historical
// rate policies; the P2 planner routes the others through here or names
// their divergence explicitly.
// =============================================================================

using sonicpi::device::resolveTargetRate;

TEST_CASE("TargetRate: supported current rate is kept (returns 0)",
          "[TargetRate]") {
    REQUIRE(resolveTargetRate({44100, 48000}, 48000) == 0);
}

TEST_CASE("TargetRate: unsupported current rate resolves to nearest",
          "[TargetRate]") {
    REQUIRE(resolveTargetRate({44100, 96000}, 48000) == 44100);
    REQUIRE(resolveTargetRate({44100, 48000}, 96000) == 48000);
}

TEST_CASE("TargetRate: no rates known = keep (returns 0)", "[TargetRate]") {
    REQUIRE(resolveTargetRate({}, 48000) == 0);
}

TEST_CASE("TargetRate: integer comparison tolerates 44100.0001-style reports",
          "[TargetRate]") {
    REQUIRE(resolveTargetRate({44100.0001}, 44100) == 0);
}

// =============================================================================
// planSwap
//
// The whole decision half of switchDevice, pure: scope resolution, ASIO
// mirroring, the rate precedence ladder (explicit > wireless-exit >
// probe-nearest > per-device memory), input auto-enable with WASAPI
// clamp, channel-count cold forcing, and the final hot/cold verdict.
// The executor (applySwapPlan side of switchDevice) mutates nothing the
// planner didn't decide.
// =============================================================================

using sonicpi::device::planSwap;
using sonicpi::device::SwapPlanRequest;
using sonicpi::device::SwapSnapshot;

namespace {
SwapSnapshot snapTwoDevices() {
    SwapSnapshot s;
    s.hasDeviceManager = true;
    s.juceCurrentType = "CoreAudio";
    s.deviceTable = { {"CoreAudio", "Speakers"},
                      {"CoreAudio", "Interface"},
                      {"ASIO", "MOTU"} };
    s.currentOutputName = "Speakers";
    s.currentRate = 48000;
    s.currentOutputChannels = 2;
    s.bootInputChannels = -1;   // boot asked for auto-max
    return s;
}
} // namespace

TEST_CASE("PlanSwap: unknown device errors without any plan", "[PlanSwap]") {
    SwapPlanRequest req;
    req.outputName = "Ghost";
    auto plan = planSwap(req, snapTwoDevices());
    REQUIRE(!plan.error.empty());
}

TEST_CASE("PlanSwap: same-driver, same-rate named swap is hot", "[PlanSwap]") {
    SwapPlanRequest req;
    req.outputName = "Interface";
    auto plan = planSwap(req, snapTwoDevices());
    REQUIRE(plan.error.empty());
    REQUIRE(!plan.isCold);
    REQUIRE(!plan.scope.crossDriver);
}

TEST_CASE("PlanSwap: explicit rate change is cold at that rate", "[PlanSwap]") {
    SwapPlanRequest req;
    req.sampleRate = 44100;
    auto plan = planSwap(req, snapTwoDevices());
    REQUIRE(plan.isCold);
    REQUIRE(plan.sampleRate == 44100);
}

TEST_CASE("PlanSwap: cross-driver pick forces cold and mirrors ASIO input",
          "[PlanSwap]") {
    SwapPlanRequest req;
    req.outputName = "MOTU";
    auto snap = snapTwoDevices();
    snap.intendedDriver = "ASIO";   // two-step driver→device flow pending
    auto plan = planSwap(req, snap);
    REQUIRE(plan.error.empty());
    REQUIRE(plan.scope.crossDriver);
    REQUIRE(plan.scope.targetDriver == "ASIO");
    REQUIRE(plan.isCold);
    REQUIRE(plan.inputName == "MOTU");   // full-duplex mirror
}

TEST_CASE("PlanSwap: wireless exit restores the pre-wireless rate",
          "[PlanSwap]") {
    auto snap = snapTwoDevices();
    snap.currentOutputName = "AirPods";
    snap.wirelessDeviceNames = { "AirPods" };
    snap.currentRate = 44100;       // what AirPlay negotiated
    snap.preWirelessRate = 48000;
    SwapPlanRequest req;
    req.outputName = "Speakers";
    auto plan = planSwap(req, snap);
    REQUIRE(plan.sampleRate == 48000);
    REQUIRE(plan.restoredPreWirelessRate);
    REQUIRE(plan.isCold);
}

TEST_CASE("PlanSwap: unsupported current rate resolves to target's nearest",
          "[PlanSwap]") {
    auto snap = snapTwoDevices();
    snap.outputDeviceRates = { 44100, 96000 };  // target doesn't do 48k
    SwapPlanRequest req;
    req.outputName = "Interface";
    auto plan = planSwap(req, snap);
    REQUIRE(plan.sampleRate == 44100);
    REQUIRE(plan.isCold);
}

TEST_CASE("PlanSwap: remembered per-device rate is restored when nothing "
          "else decided a rate", "[PlanSwap]") {
    auto snap = snapTwoDevices();
    snap.rememberedRate = 96000;
    SwapPlanRequest req;
    req.outputName = "Interface";
    auto plan = planSwap(req, snap);
    REQUIRE(plan.sampleRate == 96000);
    REQUIRE(plan.rateFromMemory);
}

TEST_CASE("PlanSwap: naming an input with zero live inputs auto-enables, "
          "clamped to the probed width", "[PlanSwap]") {
    auto snap = snapTwoDevices();
    snap.currentInputChannels = 0;
    snap.probedInputChannels = 2;   // device has 2; boot asked auto-max (64)
    SwapPlanRequest req;
    req.outputName = "Interface";
    req.inputName  = "Interface";
    auto plan = planSwap(req, snap);
    REQUIRE(plan.enableInputWidth == 2);
    REQUIRE(plan.isCold);           // world must rebuild with input buses
}

TEST_CASE("PlanSwap: __none__ input never auto-enables", "[PlanSwap]") {
    auto snap = snapTwoDevices();
    snap.currentInputChannels = 0;
    SwapPlanRequest req;
    req.outputName = "Interface";
    req.inputName  = "__none__";
    auto plan = planSwap(req, snap);
    REQUIRE(plan.enableInputWidth == -1);
}

TEST_CASE("PlanSwap: channel-count change at the target forces cold",
          "[PlanSwap]") {
    auto snap = snapTwoDevices();
    snap.probedTargetOut = 8;       // current world is 2-out
    SwapPlanRequest req;
    req.outputName = "Interface";
    auto plan = planSwap(req, snap);
    REQUIRE(plan.isCold);
    REQUIRE(plan.coldForChannels);
}

TEST_CASE("PlanSwap: user pick under the still-active driver abandons the "
          "pending driver intent", "[PlanSwap]") {
    auto snap = snapTwoDevices();
    snap.intendedDriver = "ASIO";
    SwapPlanRequest req;
    req.outputName = "Interface";   // resolves under CoreAudio
    req.userInitiated = true;
    auto plan = planSwap(req, snap);
    REQUIRE(plan.abandonDriverIntent);
    REQUIRE(!plan.scope.crossDriver);
}

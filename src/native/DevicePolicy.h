/*
 * DevicePolicy.h — Pure policy functions for device management
 *
 * These are decisions that SupersonicEngine's swap and hot-plug paths
 * need to make — extracted as pure functions so the rules can be
 * unit-tested without spinning up a real audio device. Each scenario
 * here corresponds to a bug that's been fixed and should stay fixed.
 *
 * This is an internal header: tests include it directly, and
 * SupersonicEngine.cpp uses it — but it's NOT part of the public
 * engine API. Callers embedding SupersonicEngine only see
 * SupersonicEngine.h.
 */
#pragma once

#include <string>
#include <vector>

namespace sonicpi::device {

// Minimum buffer size on a drift-compensated aggregate (see the longer
// comment on SupersonicEngine::kMinAggregateBufferSize).
inline constexpr int kMinAggregateBufferSize = 256;

// Clamp bufferSize up to kMinAggregateBufferSize if and only if a
// drift-compensated aggregate is active. Same-clock aggregates and
// single devices pass through unchanged (they can run at 16/32/64).
// Zero or negative bufferSize is a sentinel meaning "pick
// automatically" and must NOT be reinterpreted as "too small".
int clampBufferForDriftComp(int bufferSize,
                            bool aggregateWithDriftCompActive);

// Wireless-exit rate resolution. When leaving an AirPlay / Bluetooth
// device, currentRate is whatever the wireless receiver negotiated
// (often 44.1 kHz on AirPlay 1). That rate shouldn't carry onto
// hardware the user was previously running at a different rate.
// Returns the rate the swap should use: either the caller's
// requestedRate (always wins), or preWirelessRate when we're genuinely
// exiting wireless, otherwise requestedRate (unchanged).
double resolveWirelessExitRate(double requestedRate,
                               int preWirelessRate,
                               double currentRate,
                               bool currentIsWireless,
                               bool targetIsWireless);

// Hot-plug decision. Given the user's preferred output/input device
// names, the currently-active output, the currently-active input
// channel count, and the list of devices now visible to CoreAudio,
// returns what (if anything) the engine should do in response to a
// device-list change.
struct HotplugDecision {
    bool        switchOutput = false;  // full swap to preferred output
    bool        switchInput  = false;  // input-only re-aggregate
    std::string outputName;            // target output device
    std::string inputName;             // target input device
};

HotplugDecision decideHotplugAction(
    const std::string& preferredOutput,
    const std::string& preferredInput,
    const std::string& currentOutput,
    int  currentActiveInputChannels,
    const std::vector<std::string>& visibleDevices);

// Translate a CoreAudio-raw device name into JUCE's disambiguated form.
//
// JUCE appends " (N)" to device names when CoreAudio has duplicate base
// names (e.g. two identical USB interfaces, two AirPlay endpoints with
// the same base name). CoreAudio APIs return the raw base name; JUCE
// APIs (setAudioDeviceSetup) require the suffixed form or error with
// "No such device". Call this whenever a name sourced from CoreAudio
// needs to be handed to JUCE.
//
// Rules:
//   - empty rawName          → returned unchanged
//   - exact match in visible → returned as-is
//   - "<raw> (<digits>)" present in visible → that match wins
//   - nothing matches        → rawName returned unchanged (lets JUCE
//                               error normally, doesn't silently rewrite)
// The stricter "<raw> (digits)" check avoids matching "USB Audio Pro"
// against "USB Audio" (which a naive prefix+space check would).
std::string resolveJuceDeviceName(const std::string& rawName,
                                  const std::vector<std::string>& visibleDevices);

// Decide which output device to open at boot. If the macOS system
// default is wireless (AirPlay / Bluetooth), opening it via
// initialiseWithDefaultDevices and then transitioning to a real device
// triggers a CoreAudio IOProc halt (~15 s dead period) that times out
// Sonic Pi's boot handshake. Policy: if the default is wireless, find
// a non-wireless candidate from the visible list and boot with that
// directly. If no non-wireless candidate exists, return empty — caller
// falls back to default-device boot and accepts the silence window.
//
// Returns the device name to use, or empty if the default should be
// used unchanged (non-wireless, or no non-wireless fallback available).
std::string selectBootOutputDevice(const std::string& defaultName,
                                   bool defaultIsWireless,
                                   const std::vector<std::string>& visibleDevices,
                                   const std::vector<bool>& visibleIsWireless);

// Validate device names against a visible-device list before any
// destructive swap work happens. Returns empty string on success or
// an error string naming the bad argument. Names are accepted if:
//   - empty (means "leave unchanged")
//   - "__system__" sentinel (output only — system default)
//   - "__none__" sentinel (input only — disable inputs)
//   - exact match in visibleDevices
//   - matches "<name> (<digits>)" form in visibleDevices
//
// switchDevice does many destructive operations (destroy_world,
// removeAudioCallback, opts[] mutation) before reaching JUCE's
// setAudioDeviceSetup. If the doomed name only fails *there*, we're
// already in a half-built state with no easy rollback. Refusing
// up-front keeps the engine on the previous device.
std::string validateSwapDeviceNames(
    const std::string& deviceName,
    const std::string& inputDeviceName,
    const std::vector<std::string>& visibleDevices);

// Decide scsynth's block size (mBufLength) at boot given the hardware
// callback buffer size. Matching them means the audio-thread loop
// processes exactly one scsynth block per HW callback — no prefetch
// buffer, no input accumulator. Diverging means the decoupling
// machinery in JuceAudioCallback handles the mismatch (correct but
// more memcpy).
//
// Rules:
//   - hwBufSize in [minBlockSize, maxBlockSize] → return hwBufSize
//   - hwBufSize outside that range (or 0 / negative) → return
//     defaultBlockSize
// The clamp matches JuceAudioCallback::initialiseWorld's own clamp
// so the two agree about what's valid.
int chooseBlockSize(int hwBufSize, int defaultBlockSize,
                    int minBlockSize, int maxBlockSize);

} // namespace sonicpi::device

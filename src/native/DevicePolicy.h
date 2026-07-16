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

// Decide the rate to run a CoreAudio aggregate at, after the caller has
// TRIED to set both sub-devices to `desired` and read back what they
// actually settled at (`actualIn` / `actualOut`). The output sub-device
// is the aggregate's clock master and carries playback, so the engine
// must run at the OUTPUT's actual rate — running it at a rate the output
// doesn't share makes CoreAudio resample inside the IOProc (audible
// distortion) and changes the system device rate for nothing.
//
// This implements "try the remembered rate, else use the device's rate":
// actualOut == desired when the output accepted it, or the output's own
// rate when it refused. A disagreeing input (e.g. a Bluetooth HFP mic
// pinned to 16 kHz against a 48 kHz output) is resampled to match — that
// only affects the input path, which is unavoidable for such a device.
//   - actualOut readable (>0) → actualOut
//   - else actualIn readable  → actualIn
//   - else                    → desired
double resolveAggregateRate(double desired, double actualIn, double actualOut);

// Decide whether to follow a macOS system-default-output change (the
// CoreAudio default-device listener fired). We auto-follow the default so
// playback tracks where the user sends sound — but only onto a *real*
// device. Following the wrong thing here is what storms the device list:
// each follow cold-swaps + rebuilds the aggregate, which itself perturbs
// the device list, re-firing the listener.
//
// Returns false (don't follow) when:
//   - newDefault is empty (couldn't read it)
//   - newDefault is one of our own "SuperSonic" aggregates — creating an
//     aggregate briefly elevates it to system default; following that
//     nests aggregation and spirals
//   - newDefault == currentOutput (already there)
//   - newDefault is a virtual device (NDI Audio, Loopback, BlackHole, …):
//     apps spawn these and macOS may make one the default, but chasing it
//     cold-swaps onto a device the user never chose and storms the device
//     list. Explicit selection of a virtual output goes through
//     setDeviceMode(name), not this auto-follow, so it's unaffected.
bool shouldFollowDefaultOutputChange(const std::string& newDefault,
                                     const std::string& currentOutput,
                                     bool newDefaultIsVirtual);

// True if `name` (or its JUCE "<name> (N)" disambiguated form) currently
// appears in `visibleNames`. After creating a CoreAudio aggregate, JUCE's
// device list only shows it once it rescans — which can take longer than a
// fixed sleep. The engine polls scanForDevices() and uses this to know when
// the aggregate is safe to open: opening it before JUCE can see it errors
// "No such device" and forces a fallback that drops the aggregate (losing
// the mic). Same "<base> (N)" tolerance as resolveJuceDeviceName.
bool deviceNameVisible(const std::string& name,
                       const std::vector<std::string>& visibleNames);

// Sample rates an aggregate can run *cleanly*, given its two sub-devices'
// available-rate lists: the rates BOTH support. A rate only one side
// supports forces CoreAudio to resample inside the aggregate (distortion),
// so it isn't offered. This is what the macOS rate dropdown should show
// when on an aggregate — not just the current rate. Fallbacks keep the
// list usable:
//   - outputRates empty                  → inputRates
//   - inputRates empty (output-only)     → outputRates
//   - both present but disjoint (e.g. a  → outputRates (output is the
//     16 kHz Bluetooth HFP mic vs a         aggregate's clock master and
//     48 kHz output)                        the audible path; the input is
//                                            resampled to match)
// Order follows outputRates (already device-sorted).
std::vector<int> usableAggregateRates(const std::vector<int>& outputRates,
                                      const std::vector<int>& inputRates);

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

// A device's location: which AudioIODeviceType owns it, and the
// canonical (driver-disambiguated) device name. Pure data — no JUCE
// dependency so it can be returned from a unit-testable helper.
struct DeviceLocation {
    std::string driverName;
    std::string deviceName;
    bool found = false;
};

// Find which driver type owns a given device name. The deviceTable is a
// flat list of (driverName, deviceName) pairs across every available
// type (callers build this by iterating getAvailableDeviceTypes() +
// scanForDevices() + getDeviceNames(false)). Match is case-sensitive
// exact, plus the JUCE "<base> (N)" disambiguation suffix tolerated
// (matches resolveJuceDeviceName's rules).
//
// Returns {found=false} if the name resolves to no known device. The
// caller should treat that as "validation failure" — a name that
// doesn't appear under any driver isn't openable.
DeviceLocation locateDevice(
    const std::string& deviceName,
    const std::vector<std::pair<std::string, std::string>>& deviceTable);

// Resolved plan for a switchDevice call. `targetDriver` /
// `targetDevice` carry the resolved (driver, device) pair; both are
// empty when deviceFound=false. `needsTypeSwitch` is true only when
// the engine must call setCurrentAudioDeviceType before opening —
// currently that's the cold-init path (no driver active yet).
// Runtime device picks resolve strictly within the active driver,
// so needsTypeSwitch is always false there; an unresolvable name
// returns deviceFound=false and the caller rejects the swap.
struct DeviceSwitchPlan {
    bool        needsTypeSwitch = false;
    std::string targetDriver;
    std::string targetDevice;
    bool        deviceFound = false;
};

DeviceSwitchPlan planDeviceSwitch(
    const std::string& currentDriver,
    const std::string& targetDeviceName,
    const std::vector<std::pair<std::string, std::string>>& deviceTable);

// Which driver a switchDevice call resolves its device names under, and
// whether it abandons a pending switchDriver intent.
//
// The pending intent is a USER concept: the user picked a driver whose
// device isn't open yet (ASIO with no remembered device), so their next
// device pick scopes under that driver — that's the two-step driver→device
// flow. A user pick that instead resolves only under the driver actually
// open means they've walked away from the swap: abandon the intent and
// scope to the current driver.
//
// Engine-internal traffic (userInitiated=false: recovery reopen after a
// failed swap, hotplug re-attach, system-default follows) is not a
// statement of user intent. It always scopes under the driver actually
// open and never consumes the pending intent — a recovery that lands on
// the system default must not eat the user's driver pick, or their next
// device pick gets refused against the wrong driver.
//
// Empty names and the "__system__" / "__none__" sentinels aren't device
// picks; they resolve under any driver. A name resolving under neither
// driver keeps the intended scope so the refusal names the driver the
// user chose.
struct SwapScopeDecision {
    std::string scopedDriver;    // driver to resolve device names under
    bool        abandonIntent = false;  // clear the pending driver intent
};

SwapScopeDecision resolveSwapScope(
    bool userInitiated,
    const std::string& intendedDriver,
    const std::string& currentDriver,
    const std::string& outputName,
    const std::string& inputName,
    const std::vector<std::pair<std::string, std::string>>& deviceTable);

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

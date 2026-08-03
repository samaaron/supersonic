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

#include "DeviceInfo.h"

#include <string>
#include <vector>

namespace sonicpi::device {

// Minimum buffer size on a drift-compensated aggregate (see the longer
// comment on SupersonicEngine::kMinAggregateBufferSize).
inline constexpr int kMinAggregateBufferSize = 256;

// Upper bound on channels we'll ever request when the true capacity is
// unknown. 64 covers commodity audio interfaces (MOTU, RME, etc.). NOTE:
// CoreAudio silently clamps an over-request to the device's real count,
// but WASAPI rejects setAudioDeviceSetup outright — an unclamped
// over-request must never reach a device whose capacity was probeable.
inline constexpr int kRequestMaxChannels = 64;

// One answer to "how many input channels do we actually request".
// `requested` < 0 is the auto sentinel ("re-enable inputs"), resolved
// against the boot -i flag: an explicit boot count wins, boot auto-max
// (-1) means kRequestMaxChannels, boot-disabled (0) defaults to stereo.
// The result — auto or explicit — is then clamped to `probedMax`, the
// device's probed input capacity (<= 0 = probe failed / unknown, no
// clamp). Shared by switchDevice's auto-enable block and
// enableInputChannels; they previously resolved independently and only
// the former clamped, which shipped 64 input bits into WASAPI on the
// enableInputChannels(-1) boot path.
int resolveInputWidth(int requested, int bootInputChannels, int probedMax);

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
//   - newDefault is one of our own aggregates, named
//     "<selfAggregatePrefix>#N" where the prefix is the published app name
//     (AggregateDeviceHelper names them from ss_app_name(), not a fixed
//     "SuperSonic" literal) — creating an aggregate briefly elevates it to
//     system default; following that nests aggregation and spirals
//   - newDefault == currentOutput (already there)
//   - newDefault is a virtual device (NDI Audio, Loopback, BlackHole, …):
//     apps spawn these and macOS may make one the default, but chasing it
//     cold-swaps onto a device the user never chose and storms the device
//     list. Explicit selection of a virtual output goes through
//     setDeviceMode(name), not this auto-follow, so it's unaffected.
bool shouldFollowDefaultOutputChange(const std::string& newDefault,
                                     const std::string& currentOutput,
                                     bool newDefaultIsVirtual,
                                     const std::string& selfAggregatePrefix);

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

// Decide which input device the boot open pairs with the opened output.
// The daemon passes the user's saved input via -H (see parseHardwareFlag);
// before that existed boot always paired the system default input, and the
// saved input arrived one cold swap (a full second studio boot) later.
//   - requestedInput empty / "__none__"         → systemDefaultInput
//   - requestedInput visible (exact or JUCE
//     "<name> (N)" form, resolveJuceDeviceName)  → the resolved name
//   - requestedInput not visible (unplugged)     → systemDefaultInput; the
//     GUI's restore reconciler surfaces/clears the stale pref
//   - resolved entry marked unsuitable in visibleIsSuitable (parallel to
//     visibleInputs; empty or mismatched length = all suitable) →
//     systemDefaultInput. Callers pass the same vetting switchDevice
//     applies (no wireless aggregate sub-devices).
std::string chooseBootInputDevice(const std::string& requestedInput,
                                  const std::string& systemDefaultInput,
                                  const std::vector<std::string>& visibleInputs,
                                  const std::vector<bool>& visibleIsSuitable = {});

// -H flag semantics, scsynth-compatible (upstream scsynth_main.cpp): one
// name serves BOTH directions; two names are "<input> <output>". Sonic Pi's
// daemon sends all three shapes (-H <in> <out>, -H <in>, -H <out>).
// `secondToken` is the next argv token or nullptr; it counts as a device
// name only when non-empty and not flag-shaped (leading '-') — a real
// device name starting with '-' therefore can't be passed second, the same
// ambiguity upstream has. Sentinels stay direction-scoped: "__system__"
// (follow default output) is never mirrored to input, "__none__" (disable
// input) never hijacks output.
struct HardwareFlagRequest {
    std::string outputDevice;      // empty = default output
    std::string inputDevice;       // empty = default input
    bool secondTokenUsed = false;  // caller advances argv one extra slot
};
HardwareFlagRequest parseHardwareFlag(const std::string& first,
                                      const char* secondToken);

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

// The list-shaping half of sendDeviceReport: which devices the GUI is
// offered, given everything JUCE enumerated. Filter order is contractual:
//   1. platform clutter hidden (isPlatformClutter, with PipeWire-active
//      detection folded in — direct-hw ALSA PCMs are unopenable while
//      PipeWire owns the card);
//   2. wireless outputs hidden (HAL can't open them), unsuitable inputs
//      hidden (wireless mics force HFP 16 kHz mono);
//   3. inputs remembered as known-bad against the CURRENT output hidden
//      (per-output scoping — the same input may pair fine elsewhere);
//   4. an unpairable (wireless) current output clears the whole input
//      list — don't offer mics that a swap would drop;
//   5. grouped per-driver lists snapshot here (NOT deduped — one row per
//      (driver, device) so clients can render any driver's list);
//   6. flat lists dedupe by name, active driver's entry winning
//      (Windows enumerates one endpoint under four driver types);
//   7. an empty input list against a non-empty current input marks the
//      snapshot transient (JUCE mid-churn) — suppress the whole report,
//      or the GUI silently deselects the user's mic.
struct DeviceListSelection {
    bool pipewireActive = false;
    bool suppressReport = false;
    std::vector<DeviceInfo> outputsByDriver, inputsByDriver;  // step 5
    std::vector<DeviceInfo> outputs, inputs;                  // step 6
};

DeviceListSelection selectReportedDevices(
    const std::vector<DeviceInfo>& all,
    const std::string& currentOutputName,
    const std::string& currentInputName,
    const std::string& activeDriver,
    const std::vector<std::string>& knownBadInputsForCurrent);

// Where a swap will land, driver-wise. Accumulated across the output and
// input names of one swap request by resolveSwapTarget; crossDriver
// latches first-wins (the first name that resolves under a driver other
// than JUCE's actual type decides the transition; the second name never
// overwrites it). Formerly function-local state inside switchDevice's
// `considerName` lambda — promoted because init needed the same answer
// from inside an #else branch and couldn't reach it (cross-platform
// build break, 2026-08-02).
struct SwapScope {
    bool        crossDriver = false;
    std::string targetDriver;   // driver the swap transitions to
    std::string targetDevice;   // the device that forced the transition
};

// Resolve one requested device name against the scoped driver (see
// resolveSwapScope for how the scope is chosen) and fold the result into
// `scope`. Returns an error string when the name doesn't resolve under
// the scoped driver — exact wording is a GUI-facing contract — or empty
// on success / sentinel / empty name. The cross-driver comparison runs
// against `juceCurrentType` (JUCE's actual open type), NOT the scope:
// that's what setCurrentAudioDeviceType has to be called for, regardless
// of the pending-intent scope used for the lookup.
std::string resolveSwapTarget(
    const std::string& name,
    const std::string& scopedDriver,
    const std::string& juceCurrentType,
    const std::vector<std::pair<std::string, std::string>>& deviceTable,
    SwapScope& scope);

// ── planSwap ────────────────────────────────────────────────────────────
// The entire decision half of switchDevice, pure. The engine builds a
// SwapSnapshot (every probe/lookup the decisions need), planSwap decides,
// and the executor applies the plan without deciding anything further.
// Decision order is contractual and mirrors years of field fixes:
//   1. scope resolution (resolveSwapScope) + pending-intent abandonment;
//   2. name resolution per side (resolveSwapTarget), cross-driver latch;
//   3. ASIO full-duplex input mirroring;
//   4. rate precedence: explicit request > wireless-exit restore
//      (resolveWirelessExitRate) > target-probe nearest (resolveTargetRate,
//      output side first) > per-device rate memory;
//   5. input auto-enable when a device was named against a zero-input
//      world (resolveInputWidth, probed clamp) — forces cold;
//   6. channel-count change at the target forces cold (a hot swap keeps
//      the World, whose bus counts are only re-read on rebuild);
//   7. cold = forced || channels || cross-driver || rate change.
// Provenance flags exist so the executor can log exactly what the old
// inline code logged.
struct SwapPlanRequest {
    std::string outputName, inputName;   // raw; sentinels included
    double sampleRate = 0;               // 0 = unspecified
    int    bufferSize = 0;
    bool   forceCold = false;
    bool   userInitiated = true;
};

struct SwapSnapshot {
    bool hasDeviceManager = false;       // headless: only rate/cold basics apply
    std::string juceCurrentType;         // JUCE's actually-open type
    std::string intendedDriver;          // pending switchDriver intent
    std::string deviceMode;              // pinned output ("" = system mode)
    std::vector<std::pair<std::string, std::string>> deviceTable;
    std::string currentOutputName;
    std::vector<std::string> wirelessDeviceNames;
    double currentRate = 0;
    int currentOutputChannels = 0;
    int currentInputChannels = 0;
    int bootInputChannels = 0;           // the boot -i flag
    int preWirelessRate = 0;
    std::vector<double> outputDeviceRates, inputDeviceRates;  // target probes
    int probedInputChannels = -1;        // target input capacity (auto-enable clamp)
    int probedTargetOut = -1, probedTargetIn = -1;  // channel-cold probes
    int rememberedRate = 0;              // per-device rate memory for outputName
};

struct SwapPlan {
    std::string error;                   // non-empty = refuse, nothing decided
    SwapScope scope;
    bool abandonDriverIntent = false;
    std::string inputName;               // post-ASIO-mirror input
    double sampleRate = 0;               // 0 = keep current
    bool restoredPreWirelessRate = false;
    bool rateAdjustedToNearest  = false;
    bool rateFromMemory         = false;
    int  enableInputWidth = -1;          // >= 0: set world input width, cold
    int  enableInputRequested = 0;       // pre-clamp width (logging)
    int  enableInputProbed = -1;         // probe answer (logging)
    bool coldForChannels = false;
    bool isCold = false;
};

SwapPlan planSwap(const SwapPlanRequest& req, const SwapSnapshot& snap);

// The pure core of "which rate does a swap land on" when no rate was
// requested: keep the current session rate when the target device
// supports it (integer-compared — drivers report 44100.0001-style
// values), else the device's nearest supported rate. Returns 0 for
// "keep" — the caller only forces a cold swap on a returned change.
double resolveTargetRate(const std::vector<double>& deviceRates,
                         double currentRate);

// THE device-name identity predicate: equal, or equal modulo JUCE's
// " (N)" duplicate-disambiguation suffix (digits only), in either
// direction. Replaces the loose "base + space" prefix rule that lived in
// two copies beside this strict one — the loose rule matched
// "USB Audio Pro" against "USB Audio", so the hotplug auto-switch could
// treat a different physical device as the preferred one.
bool sameDeviceName(const std::string& a, const std::string& b);

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

// An "exclusive duplex" device carries both its input and output sides on
// a single node (the PipeWire patchbay), so it can never be half of a
// mixed pair with another device. Resolve a switch request into the pair
// that will actually open, honouring the side the user changed:
//   - a request that *introduces* the exclusive device on either side
//     claims both sides (selecting it anywhere selects it everywhere);
//   - a request that moves one side away from it is never hijacked — the
//     carried-over exclusive side yields instead: an input falls to
//     "__none__" (inputs disabled), an output falls to fallbackOutput;
//   - pairs not involving the exclusive device pass through, empty
//     request fields (= "keep current") included.
// "Introduces" is judged against the current pair because the GUI re-sends
// the unchanged side's name alongside the one the user picked. Empty
// exclusiveName disables the policy.
struct ExclusivePair {
    std::string output;
    std::string input;
};
ExclusivePair resolveExclusiveDuplexPair(const std::string& requestedOutput,
                                         const std::string& requestedInput,
                                         const std::string& currentOutput,
                                         const std::string& currentInput,
                                         const std::string& exclusiveName,
                                         const std::string& fallbackOutput);

// Display/table name for the default-follow entry — both the PipeWire
// driver's native device and every synthetic row use it, so a pick of
// this name is a real device under drivers that have one and a system-
// mode request everywhere else (see isSyntheticDefaultPick).
inline constexpr const char* kSystemDefaultTableName = "System Default";

// Capability annotation for one driver's device-table group. The table is
// the single source of truth for client dropdowns: semantics ride on
// per-device flags ("follows-default", "exclusive-duplex", "synthetic"),
// never on client-side row synthesis or name sentinels. A driver whose
// own device list carries a native default-follow device (nativeDriver's
// defaultFollowName) gets it flagged; any other driver gets a synthetic
// flagged default row (ASIO excepted — it has no OS-default concept, each
// ASIO driver is its single device). Flags are per-output, parallel to
// the outputs vector; comma-separated tokens.
struct DriverTableAnnotation {
    std::vector<std::string> flags;      // one entry per output, "" = none
    bool insertSyntheticDefault = false; // prepend syntheticName to the group
    std::string syntheticName;
    std::string syntheticFlags;
};
DriverTableAnnotation annotateDriverOutputs(const std::string& driver,
                                            const std::vector<std::string>& outputs,
                                            const std::string& nativeDriver,
                                            const std::string& defaultFollowName,
                                            const std::string& exclusiveName);

} // namespace sonicpi::device

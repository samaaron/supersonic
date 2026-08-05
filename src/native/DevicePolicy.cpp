/*
 * DevicePolicy.cpp — see DevicePolicy.h
 */
#include "DevicePolicy.h"
#include "FuzzyMatch.h"

#include <algorithm>
#include <set>

namespace sonicpi::device {

namespace {
// full == base + " (digits)" — JUCE's duplicate-name suffix, exactly.
bool hasJuceDupSuffix(const std::string& full, const std::string& base) {
    if (full.size() < base.size() + 4) return false;   // need " (1)"
    if (full.compare(0, base.size(), base) != 0) return false;
    size_t i = base.size();
    if (full[i] != ' ' || full[i + 1] != '(') return false;
    if (full.back() != ')') return false;
    bool digits = false;
    for (size_t k = i + 2; k + 1 < full.size(); ++k) {
        if (full[k] < '0' || full[k] > '9') return false;
        digits = true;
    }
    return digits;
}
} // anonymous namespace

bool sameDeviceName(const std::string& a, const std::string& b) {
    return a == b || hasJuceDupSuffix(a, b) || hasJuceDupSuffix(b, a);
}


SwapPlan planSwap(const SwapPlanRequest& req, const SwapSnapshot& snap) {
    SwapPlan plan;
    plan.inputName  = req.inputName;
    plan.sampleRate = req.sampleRate;
    bool forceCold  = req.forceCold;

    if (snap.hasDeviceManager) {
        auto scopeDecision = resolveSwapScope(
            req.userInitiated, snap.intendedDriver, snap.juceCurrentType,
            req.outputName, req.inputName, snap.deviceTable);
        plan.abandonDriverIntent = scopeDecision.abandonIntent;

        if (auto err = resolveSwapTarget(req.outputName,
                                         scopeDecision.scopedDriver,
                                         snap.juceCurrentType,
                                         snap.deviceTable, plan.scope);
            !err.empty()) {
            plan.error = err;
            return plan;
        }
        if (auto err = resolveSwapTarget(req.inputName,
                                         scopeDecision.scopedDriver,
                                         snap.juceCurrentType,
                                         snap.deviceTable, plan.scope);
            !err.empty()) {
            plan.error = err;
            return plan;
        }

        // ASIO is full-duplex single-device by spec — one open call
        // delivers both directions. On a cross-driver switch to ASIO with
        // an explicit output but no input, mirror the output into the
        // input.
        if (plan.scope.crossDriver && plan.scope.targetDriver == "ASIO"
            && !req.outputName.empty()
            && (plan.inputName.empty() || plan.inputName == "__none__"))
            plan.inputName = plan.scope.targetDevice;
    }

    auto isWireless = [&](const std::string& n) {
        if (n.empty()) return false;
        for (auto& w : snap.wirelessDeviceNames)
            if (sameDeviceName(w, n)) return true;
        return false;
    };

    // Rate precedence rung 2: leaving wireless restores the remembered
    // pre-wireless rate (the wireless receiver's negotiated rate must not
    // carry onto hardware).
    if (plan.sampleRate <= 0 && snap.preWirelessRate > 0
        && snap.hasDeviceManager) {
        const std::string targetName =
            req.outputName.empty() ? snap.deviceMode : req.outputName;
        const double resolved = resolveWirelessExitRate(
            plan.sampleRate, snap.preWirelessRate, snap.currentRate,
            isWireless(snap.currentOutputName), isWireless(targetName));
        if (resolved != plan.sampleRate) {
            plan.sampleRate = resolved;
            plan.restoredPreWirelessRate = true;
        }
    }

    // Rung 3: target device doesn't support the current rate → nearest.
    // Output side answers first; the input side is only consulted when
    // the output side kept the rate (or knew nothing).
    if (plan.sampleRate <= 0) {
        double r = resolveTargetRate(snap.outputDeviceRates, snap.currentRate);
        if (r == 0)
            r = resolveTargetRate(snap.inputDeviceRates, snap.currentRate);
        if (r != 0) {
            plan.sampleRate = r;
            plan.rateAdjustedToNearest = true;
        }
    }

    // Input auto-enable: a device was named against a zero-input world.
    // Must precede the cold verdict so the forced cold takes effect.
    if (!plan.inputName.empty() && plan.inputName != "__none__"
        && snap.currentInputChannels == 0) {
        plan.enableInputRequested =
            resolveInputWidth(-1, snap.bootInputChannels, -1);
        plan.enableInputProbed = snap.probedInputChannels;
        plan.enableInputWidth =
            resolveInputWidth(-1, snap.bootInputChannels,
                              snap.probedInputChannels);
        forceCold = true;
    }

    // Rung 4: per-device rate memory, only when nothing above decided.
    if (plan.sampleRate <= 0 && !req.outputName.empty()
        && snap.rememberedRate > 0) {
        plan.sampleRate = static_cast<double>(snap.rememberedRate);
        plan.rateFromMemory = true;
    }

    // A hot swap keeps the existing World; its bus counts are only
    // re-read on rebuild, so a channel-count change at the target forces
    // cold (writes to higher buses would land on private buses instead of
    // hardware).
    if (snap.hasDeviceManager && !forceCold) {
        if (snap.probedTargetOut > 0
            && snap.probedTargetOut != snap.currentOutputChannels)
            plan.coldForChannels = true;
        if (snap.probedTargetIn > 0
            && snap.probedTargetIn != snap.currentInputChannels)
            plan.coldForChannels = true;
    }

    plan.isCold = forceCold || plan.coldForChannels || plan.scope.crossDriver
               || (plan.sampleRate > 0 && plan.sampleRate != snap.currentRate);
    return plan;
}

double resolveTargetRate(const std::vector<double>& deviceRates,
                         double currentRate) {
    if (deviceRates.empty() || currentRate <= 0) return 0;
    for (double r : deviceRates)
        if (static_cast<int>(r) == static_cast<int>(currentRate))
            return 0;
    double nearest = deviceRates[0];
    for (double r : deviceRates)
        if (std::abs(r - currentRate) < std::abs(nearest - currentRate))
            nearest = r;
    return nearest;
}

DeviceListSelection selectReportedDevices(
        const std::vector<DeviceInfo>& all,
        const std::string& currentOutputName,
        const std::string& currentInputName,
        const std::string& activeDriver,
        const std::vector<std::string>& knownBadInputsForCurrent) {
    DeviceListSelection sel;

    for (auto& d : all) {
        if (d.typeName == "PipeWire"
            || (d.typeName == "ALSA" && d.name == "PipeWire Sound Server")) {
            sel.pipewireActive = true;
            break;
        }
    }

    std::vector<DeviceInfo> outputs, inputs;
    for (auto& d : all) {
        if (d.isPlatformClutter(sel.pipewireActive)) continue;
        if (d.maxOutputChannels > 0 && !d.isWirelessTransport())
            outputs.push_back(d);
        if (d.maxInputChannels > 0 && d.isSuitableForInput())
            inputs.push_back(d);
    }

    // Inputs that failed against this output are deliberately NOT removed.
    // Quarantining them meant the list quietly shrank as the user worked
    // through it, with nothing said and no way back — the app appeared to be
    // losing hardware. A device that cannot open says so when it is picked,
    // every time, which is duller and much easier to understand.
    (void)knownBadInputsForCurrent;

    if (!currentOutputName.empty()) {
        for (auto& d : all) {
            if (sameDeviceName(d.name, currentOutputName)
                && !d.isSuitableForAggregate()) {
                inputs.clear();
                break;
            }
        }
    }

    sel.outputsByDriver = outputs;
    sel.inputsByDriver  = inputs;

    auto dedupeByName = [&](std::vector<DeviceInfo>& devs) {
        std::vector<DeviceInfo> out;
        out.reserve(devs.size());
        std::set<std::string> seen;
        for (auto& d : devs)
            if (d.typeName == activeDriver && seen.insert(d.name).second)
                out.push_back(d);
        for (auto& d : devs)
            if (seen.insert(d.name).second) out.push_back(d);
        devs.swap(out);
    };
    dedupeByName(outputs);
    dedupeByName(inputs);
    sel.outputs = std::move(outputs);
    sel.inputs  = std::move(inputs);

    sel.suppressReport = sel.inputs.empty() && !currentInputName.empty();
    return sel;
}


int resolveInputWidth(int requested, int bootInputChannels, int probedMax) {
    int width = requested;
    if (requested < 0) {
        if (bootInputChannels > 0)      width = bootInputChannels;
        else if (bootInputChannels < 0) width = kRequestMaxChannels;
        else                            width = 2;
    }
    if (probedMax > 0 && width > probedMax) width = probedMax;
    return width;
}

int clampBufferForDriftComp(int bufferSize,
                            bool aggregateWithDriftCompActive) {
    if (aggregateWithDriftCompActive
        && bufferSize > 0
        && bufferSize < kMinAggregateBufferSize) {
        return kMinAggregateBufferSize;
    }
    return bufferSize;
}

double resolveWirelessExitRate(double requestedRate,
                               int preWirelessRate,
                               double currentRate,
                               bool currentIsWireless,
                               bool targetIsWireless) {
    if (requestedRate > 0)                                return requestedRate;
    if (preWirelessRate <= 0)                             return requestedRate;
    if (!currentIsWireless)                               return requestedRate;
    if (targetIsWireless)                                 return requestedRate;
    if (static_cast<int>(currentRate) == preWirelessRate) return requestedRate;
    return static_cast<double>(preWirelessRate);
}

double resolveAggregateRate(double desired, double actualIn, double actualOut) {
    // Output is the aggregate's clock master and the audible path — run at
    // whatever rate it actually settled on (== desired if it accepted that,
    // its own rate if it refused). Fall back to the input rate, then the
    // desired rate, only when the output rate is unreadable.
    if (static_cast<int>(actualOut) > 0) return actualOut;
    if (static_cast<int>(actualIn)  > 0) return actualIn;
    return desired;
}

bool shouldFollowDefaultOutputChange(const std::string& newDefault,
                                     const std::string& currentOutput,
                                     bool newDefaultIsVirtual,
                                     const std::string& selfAggregatePrefix,
                                     const std::string& pinnedOutputDevice) {
    // A pinned output (-H / GUI choice) always wins: following would
    // convert a hardware event into a device switch the user never asked
    // for, and the setDeviceMode("") routing would erase the pin. The
    // hot-plug reconciler owns returning to the pin.
    if (!pinnedOutputDevice.empty()) return false;
    if (newDefault.empty())          return false;
    if (!selfAggregatePrefix.empty()
        && newDefault.compare(0, selfAggregatePrefix.size(),
                              selfAggregatePrefix) == 0
        && newDefault.size() > selfAggregatePrefix.size()
        && newDefault[selfAggregatePrefix.size()] == '#')
        return false;
    if (newDefault == currentOutput) return false;
    if (newDefaultIsVirtual)         return false;
    return true;
}

namespace {
// Index of the entry `name` genuinely resolves to, or -1. resolveJuceDeviceName
// returns the exact name, the "<name> (N)" form if that's what's present, or
// `name` unchanged when nothing matches — so a genuine match is exactly "the
// resolved name is actually in the list". Shared core of deviceNameVisible
// and chooseBootInputDevice.
int resolvedVisibleIndex(const std::string& name,
                         const std::vector<std::string>& visibleNames) {
    if (name.empty()) return -1;
    const std::string resolved = resolveJuceDeviceName(name, visibleNames);
    for (size_t i = 0; i < visibleNames.size(); ++i)
        if (visibleNames[i] == resolved) return static_cast<int>(i);
    return -1;
}
} // namespace

bool deviceNameVisible(const std::string& name,
                       const std::vector<std::string>& visibleNames) {
    return resolvedVisibleIndex(name, visibleNames) >= 0;
}

std::string selectRecoveryTarget(const std::string& pinnedOutput,
                                 const std::vector<std::string>& visibleOutputs) {
    if (pinnedOutput.empty()) return {};
    const int idx = resolvedVisibleIndex(pinnedOutput, visibleOutputs);
    if (idx >= 0) return visibleOutputs[idx];
    return {};
}

std::vector<std::string> scopeInputsToDriver(
        const std::vector<std::pair<std::string, std::string>>& inputTable,
        const std::string& driver) {
    std::vector<std::string> names;
    for (auto& [drv, dev] : inputTable)
        if (driver.empty() || drv == driver)
            names.push_back(dev);
    return names;
}

std::string chooseBootInputDevice(const std::string& requestedInput,
                                  const std::string& systemDefaultInput,
                                  const std::vector<std::string>& visibleInputs,
                                  const std::vector<bool>& visibleIsSuitable) {
    // A populated, length-matched mask turns vetting on. An empty or
    // mismatched mask (caller bug) keeps the legacy no-vetting behaviour —
    // a bug in building the mask must not veto a good pairing.
    const bool vetting = !visibleIsSuitable.empty()
                      && visibleIsSuitable.size() == visibleInputs.size();
    const bool noRequest =
        requestedInput.empty() || requestedInput == "__none__";

    if (!vetting) {
        if (noRequest) return systemDefaultInput;
        const int idx = resolvedVisibleIndex(requestedInput, visibleInputs);
        if (idx >= 0) return visibleInputs[idx];
        // Requested input isn't attached: boot with a working input anyway
        // and let the GUI's restore reconciler notice and clear the stale
        // pref.
        return systemDefaultInput;
    }

    if (!noRequest) {
        const int idx = resolvedVisibleIndex(requestedInput, visibleInputs);
        if (idx >= 0 && visibleIsSuitable[idx]) return visibleInputs[idx];
    }

    // The system default is an input like any other and gets the same
    // vetting (#3555: an unvetted Bluetooth HFP default became an aggregate
    // sub-device — 16 kHz engine + heap corruption from the rate/buffer
    // churn). Resolve it against the visible list so raw CoreAudio names
    // are judged by their JUCE-form mask slot; a default that isn't in the
    // list can't be judged, and unjudgeable = unpaired. Empty means "pair
    // nothing": boot output-only rather than poison the aggregate.
    const int idx = resolvedVisibleIndex(systemDefaultInput, visibleInputs);
    if (idx >= 0 && visibleIsSuitable[idx]) return visibleInputs[idx];
    return {};
}

BootInputPairing planBootInputPairing(const std::string& openedOutputName,
                                      bool openedByHardwareFlag,
                                      const std::string& preferredInput,
                                      const std::string& systemDefaultInput,
                                      const std::vector<DeviceInfo>& devices) {
    BootInputPairing plan;
    if (openedOutputName.empty()) return plan;

    std::vector<std::string> inputNames;
    std::vector<bool> inputSuitable;
    for (auto& d : devices) {
        if (d.maxInputChannels <= 0) continue;
        inputNames.push_back(d.name);
        inputSuitable.push_back(d.isSuitableForAggregate()
                                || d.name == openedOutputName);
    }
    // No enumerable inputs at all: nothing to pair. (An empty mask would
    // otherwise flip chooseBootInputDevice into its legacy no-vetting
    // mode and wave the raw default through.)
    if (inputNames.empty()) {
        plan.reason = "no input devices enumerated — booting output-only";
        return plan;
    }

    const std::string chosen = chooseBootInputDevice(
        preferredInput, systemDefaultInput, inputNames, inputSuitable);
    if (chosen.empty()) {
        plan.reason = "no suitable input to pair (wireless/aggregate-class "
                      "or unenumerated) — booting output-only";
        return plan;
    }

    if (chosen == openedOutputName) {
        // -H already settled its own input at open (or deliberately
        // dropped it); reopening would discard the user's chosen output.
        if (openedByHardwareFlag) return plan;
        plan.action = BootInputPairing::Action::FullDuplexReopen;
        plan.inputName = chosen;
        return plan;
    }

    for (auto& d : devices) {
        if (d.name != openedOutputName) continue;
        if (!d.isSuitableForAggregate()) {
            plan.reason = "'" + openedOutputName + "' is not aggregable "
                          "(wireless or aggregate-class) — input disabled";
            return plan;
        }
        plan.action = BootInputPairing::Action::Aggregate;
        plan.inputName = chosen;
        return plan;
    }
    plan.reason = "'" + openedOutputName + "' not found in the device "
                  "enumeration — input disabled";
    return plan;
}

BootDriverChoice resolveBootDriver(const std::string& requested,
                                   const std::vector<std::string>& availableTypes,
                                   bool hasDeviceRequest) {
    BootDriverChoice choice;
    if (requested.empty()) return choice;

    auto lower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), ::tolower);
        return s;
    };

    std::string matched;
    for (auto& t : availableTypes)
        if (t == requested) { matched = t; break; }
    if (matched.empty()) {
        const std::string want = lower(requested);
        int hits = 0;
        for (auto& t : availableTypes)
            if (lower(t) == want) { matched = t; ++hits; }
        if (hits > 1) matched.clear();  // ambiguous — treat as no match
    }

    if (matched.empty()) {
        choice.warning = "requested audio driver '" + requested
                       + "' is not available — keeping the platform default."
                         " Available drivers:";
        for (auto& t : availableTypes) choice.warning += " [" + t + "]";
        return choice;
    }

    if (matched == "ASIO" && !hasDeviceRequest) {
        choice.warning = "requested audio driver 'ASIO' has no default device"
                         " and probing one can hang — keeping the platform"
                         " default until a device is specified";
        return choice;
    }

    choice.driver = matched;
    return choice;
}

std::string resolveBootHardwareMatch(
        const std::string& requested,
        const std::string& preferredDriver,
        const std::vector<std::pair<std::string, std::string>>& deviceTable) {
    if (requested.empty()) return {};

    auto combine = [&](bool scoped) {
        std::vector<std::string> names;
        for (auto& [driver, device] : deviceTable) {
            if (scoped && driver != preferredDriver) continue;
            names.push_back(driver + " : " + device);
        }
        return names;
    };

    if (!preferredDriver.empty()) {
        std::string m = fuzzyMatch(requested, combine(true));
        if (!m.empty()) return m;
    }
    return fuzzyMatch(requested, combine(false));
}

HardwareFlagRequest parseHardwareFlag(const std::string& first,
                                      const char* secondToken) {
    HardwareFlagRequest r;
    const bool secondIsName =
        secondToken && secondToken[0] != '\0' && secondToken[0] != '-';
    if (secondIsName) {
        r.inputDevice     = first;
        r.outputDevice    = secondToken;
        r.secondTokenUsed = true;
        return r;
    }
    // Direction-scoped sentinels don't cross over.
    if (first == "__none__")   { r.inputDevice  = first; return r; }
    if (first == "__system__") { r.outputDevice = first; return r; }
    // Upstream parity: a single real name serves both directions.
    r.outputDevice = first;
    r.inputDevice  = first;
    return r;
}

std::vector<int> usableAggregateRates(const std::vector<int>& outputRates,
                                      const std::vector<int>& inputRates) {
    if (outputRates.empty()) return inputRates;
    if (inputRates.empty())  return outputRates;
    std::vector<int> isect;
    for (int o : outputRates)
        for (int i : inputRates)
            if (o == i) { isect.push_back(o); break; }
    return isect.empty() ? outputRates : isect;
}

std::string resolveJuceDeviceName(const std::string& rawName,
                                  const std::vector<std::string>& visibleDevices) {
    if (rawName.empty()) return rawName;

    for (auto& v : visibleDevices)
        if (v == rawName) return v;

    // Looking for "<rawName> (<digits>)" — JUCE's disambiguation pattern.
    for (auto& v : visibleDevices)
        if (hasJuceDupSuffix(v, rawName)) return v;

    return rawName;
}

std::string selectBootOutputDevice(const std::string& defaultName,
                                   bool defaultIsWireless,
                                   const std::vector<std::string>& visibleDevices,
                                   const std::vector<bool>& visibleIsWireless) {
    if (!defaultIsWireless) return {};
    if (visibleDevices.size() != visibleIsWireless.size()) return {};

    // Prefer a built-in or similarly obvious non-wireless candidate.
    // If several exist, the first non-wireless device in enumeration
    // order wins — JUCE's order matches System Settings top-to-bottom
    // so this is predictable to the user.
    for (size_t i = 0; i < visibleDevices.size(); ++i) {
        if (!visibleIsWireless[i] && !visibleDevices[i].empty()
            && visibleDevices[i] != defaultName) {
            return visibleDevices[i];
        }
    }
    return {};
}

std::string resolveSwapTarget(
        const std::string& name,
        const std::string& scopedDriver,
        const std::string& juceCurrentType,
        const std::vector<std::pair<std::string, std::string>>& deviceTable,
        SwapScope& scope) {
    if (name.empty() || name == "__system__" || name == "__none__")
        return {};
    auto plan = planDeviceSwitch(scopedDriver, name, deviceTable);
    if (!plan.deviceFound) {
        return "device '" + name + "' not available on driver '"
             + (scopedDriver.empty() ? "(none)" : scopedDriver) + "'";
    }
    if (plan.targetDriver != juceCurrentType && !scope.crossDriver) {
        scope.crossDriver  = true;
        scope.targetDriver = plan.targetDriver;
        scope.targetDevice = plan.targetDevice;
    }
    return {};
}

int chooseBlockSize(int hwBufSize, int defaultBlockSize,
                    int minBlockSize, int maxBlockSize) {
    if (hwBufSize >= minBlockSize && hwBufSize <= maxBlockSize)
        return hwBufSize;
    return defaultBlockSize;
}

HotplugDecision decideHotplugAction(
        const std::string& preferredOutput,
        const std::string& preferredInput,
        const std::string& currentOutput,
        int currentActiveInputChannels,
        const std::vector<std::string>& visibleDevices) {
    HotplugDecision d;

    auto visible = [&](const std::string& name) {
        if (name.empty()) return false;
        for (auto& v : visibleDevices)
            if (sameDeviceName(v, name)) return true;
        return false;
    };

    // Preferred output just appeared (or returned) and we're not on it.
    if (!preferredOutput.empty()
        && !sameDeviceName(currentOutput, preferredOutput)
        && visible(preferredOutput)) {
        d.switchOutput = true;
        d.outputName   = preferredOutput;
        d.inputName    = visible(preferredInput) ? preferredInput : std::string();
        return d;
    }

    // Preferred input returned while already on the correct output and
    // currently running with no inputs — re-aggregate without touching
    // the output.
    if (!preferredInput.empty()
        && visible(preferredInput)
        && currentActiveInputChannels == 0) {
        d.switchInput = true;
        d.inputName   = preferredInput;
    }
    return d;
}

namespace {
// Matches resolveJuceDeviceName's stricter "<base> (<digits>)" form.
bool deviceNameAcceptable(const std::string& name,
                          const std::vector<std::string>& visible) {
    for (auto& v : visible)
        if (v == name || hasJuceDupSuffix(v, name)) return true;
    return false;
}
} // anonymous namespace

DeviceLocation locateDevice(
        const std::string& deviceName,
        const std::vector<std::pair<std::string, std::string>>& deviceTable) {
    DeviceLocation result;
    if (deviceName.empty()) return result;

    // Exact match wins outright.
    for (auto& [drv, dev] : deviceTable) {
        if (dev == deviceName) {
            result.driverName = drv;
            result.deviceName = dev;
            result.found = true;
            return result;
        }
    }
    // Tolerate JUCE's "<base> (N)" disambiguation suffix on either side.
    for (auto& [drv, dev] : deviceTable) {
        if (dev.size() < deviceName.size() + 4) continue;
        if (dev.compare(0, deviceName.size(), deviceName) != 0) continue;
        size_t i = deviceName.size();
        if (dev[i] != ' ' || dev[i + 1] != '(') continue;
        if (dev.back() != ')') continue;
        bool digits = false;
        for (size_t k = i + 2; k + 1 < dev.size(); ++k) {
            if (dev[k] < '0' || dev[k] > '9') { digits = false; break; }
            digits = true;
        }
        if (digits) {
            result.driverName = drv;
            result.deviceName = dev;
            result.found = true;
            return result;
        }
    }
    return result;
}

DeviceSwitchPlan planDeviceSwitch(
        const std::string& currentDriver,
        const std::string& targetDeviceName,
        const std::vector<std::pair<std::string, std::string>>& deviceTable) {
    DeviceSwitchPlan plan;

    // Empty currentDriver: cold-init / boot. Global lookup;
    // needsTypeSwitch=true so the caller does
    // setCurrentAudioDeviceType before opening.
    if (currentDriver.empty()) {
        auto loc = locateDevice(targetDeviceName, deviceTable);
        if (!loc.found) return plan;
        plan.deviceFound     = true;
        plan.targetDriver    = loc.driverName;
        plan.targetDevice    = loc.deviceName;
        plan.needsTypeSwitch = true;
        return plan;
    }

    // Runtime: scope strictly to the active driver. A name that
    // resolves only under a different driver returns
    // deviceFound=false and the caller refuses the swap. Cross-
    // driver transitions are explicit user actions via
    // /supersonic/devices/mode, never an implicit side effect of
    // a device pick.
    std::vector<std::pair<std::string, std::string>> scoped;
    for (auto& [drv, dev] : deviceTable)
        if (drv == currentDriver) scoped.emplace_back(drv, dev);
    auto loc = locateDevice(targetDeviceName, scoped);
    if (!loc.found) return plan;

    plan.deviceFound     = true;
    plan.targetDriver    = loc.driverName;
    plan.targetDevice    = loc.deviceName;
    plan.needsTypeSwitch = false;
    return plan;
}

SwapScopeDecision resolveSwapScope(
        bool userInitiated,
        const std::string& intendedDriver,
        const std::string& currentDriver,
        const std::string& outputName,
        const std::string& inputName,
        const std::vector<std::pair<std::string, std::string>>& deviceTable) {
    SwapScopeDecision decision;
    decision.scopedDriver = currentDriver;

    // Internal traffic never speaks for the user: current-driver scope,
    // pending intent untouched.
    if (!userInitiated) return decision;

    if (intendedDriver.empty() || intendedDriver == currentDriver)
        return decision;

    auto resolvesUnder = [&](const std::string& drv, const std::string& n) {
        if (n.empty() || n == "__system__" || n == "__none__") return true;
        return planDeviceSwitch(drv, n, deviceTable).deviceFound;
    };
    bool intendedOk = resolvesUnder(intendedDriver, outputName)
                   && resolvesUnder(intendedDriver, inputName);
    bool currentOk  = resolvesUnder(currentDriver, outputName)
                   && resolvesUnder(currentDriver, inputName);

    // Picks that resolve only under the actually-open driver mean the user
    // walked away from the pending driver swap. Anything else keeps the
    // intended scope (including unresolvable names, so the refusal names
    // the driver the user chose).
    if (!intendedOk && currentOk) {
        decision.abandonIntent = true;
        return decision;
    }
    decision.scopedDriver = intendedDriver;
    return decision;
}

std::string validateSwapDeviceNames(
        const std::string& deviceName,
        const std::string& inputDeviceName,
        const std::vector<std::string>& visibleDevices) {
    if (!deviceName.empty()
        && deviceName != "__system__"
        && !deviceNameAcceptable(deviceName, visibleDevices)) {
        return "unknown output device: '" + deviceName + "'";
    }
    if (!inputDeviceName.empty()
        && inputDeviceName != "__none__"
        && !deviceNameAcceptable(inputDeviceName, visibleDevices)) {
        return "unknown input device: '" + inputDeviceName + "'";
    }
    return {};
}

ExclusivePair resolveExclusiveDuplexPair(const std::string& requestedOutput,
                                         const std::string& requestedInput,
                                         const std::string& currentOutput,
                                         const std::string& currentInput,
                                         const std::string& exclusiveName,
                                         const std::string& fallbackOutput) {
    ExclusivePair p{ requestedOutput, requestedInput };
    if (exclusiveName.empty())
        return p;

    const std::string& x = exclusiveName;
    const std::string& effOut = requestedOutput.empty() ? currentOutput : requestedOutput;
    const std::string& effIn  = requestedInput.empty()  ? currentInput  : requestedInput;
    const bool outIsX = effOut == x;
    const bool inIsX  = effIn == x;
    if (!outIsX && !inIsX)
        return p;

    if (outIsX && inIsX) {
        // Fully on the exclusive device. Concrete on both sides so the
        // device layer never re-derives an empty "keep" field.
        p.output = x;
        p.input = x;
        return p;
    }

    // One side on the exclusive device with the other side absent or
    // disabled is a legal state (patchbay output with inputs off), not a
    // mixed pair — nothing to resolve.
    if (outIsX && (effIn.empty() || effIn == "__none__"))
        return p;
    if (inIsX && effOut.empty())
        return p;

    // Mixed pair. The user's intent is the side the request changes; a
    // side merely re-sent or inherited is ambient state.
    const bool introducedOut = requestedOutput == x && currentOutput != x;
    const bool introducedIn  = requestedInput == x && currentInput != x;
    if (introducedOut || introducedIn) {
        p.output = x;
        p.input = x;
        return p;
    }

    // The exclusive device is carried-over state only.
    //
    // Held as the INPUT, it yields: the output the user just named has to open,
    // and #3553 showed the pair being reconciled back onto the patchbay and the
    // input amputated anyway.
    //
    // Held as the OUTPUT, it stays. The session manager links each side of the
    // filter node independently, so the node keeps its output ports while the
    // input ports take their signal from another device — verified on PipeWire
    // with out_1..out_16 on the filter node and in_1/in_2 fed from the default
    // capture source. Yielding here would drop a 16-channel output the user
    // chose in order to honour an input, and force a channel-count cold swap
    // for a pair the device layer opens as asked.
    if (inIsX)
        p.input = "__none__";
    return p;
}

DriverTableAnnotation annotateDriverOutputs(const std::string& driver,
                                            const std::vector<std::string>& outputs,
                                            const std::string& nativeDriver,
                                            const std::string& defaultFollowName,
                                            const std::string& exclusiveName) {
    DriverTableAnnotation a;
    a.flags.assign(outputs.size(), "");

    bool hasNativeDefault = false;
    if (driver == nativeDriver) {
        for (size_t i = 0; i < outputs.size(); ++i) {
            if (!defaultFollowName.empty() && outputs[i] == defaultFollowName) {
                a.flags[i] = "follows-default";
                hasNativeDefault = true;
            } else if (!exclusiveName.empty() && outputs[i] == exclusiveName) {
                a.flags[i] = "exclusive-duplex";
            }
        }
    }

    if (!hasNativeDefault && driver != "ASIO") {
        a.insertSyntheticDefault = true;
        a.syntheticName = defaultFollowName;
        a.syntheticFlags = "follows-default,synthetic";
    }
    return a;
}

} // namespace sonicpi::device

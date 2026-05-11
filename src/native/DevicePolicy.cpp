/*
 * DevicePolicy.cpp — see DevicePolicy.h
 */
#include "DevicePolicy.h"

namespace sonicpi::device {

namespace {
// JUCE appends " (N)" suffixes when CoreAudio reports duplicate device
// names. Returns true if full == base, or full is base followed by a
// space. Duplicated here (identical copy lives in SupersonicEngine.cpp)
// to keep this TU standalone — the function is 6 lines and hasn't
// changed in years.
bool deviceNameMatches(const std::string& full, const std::string& base) {
    if (full == base) return true;
    return full.size() > base.size()
        && full.compare(0, base.size(), base) == 0
        && full[base.size()] == ' ';
}
} // anonymous namespace


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

std::string resolveJuceDeviceName(const std::string& rawName,
                                  const std::vector<std::string>& visibleDevices) {
    if (rawName.empty()) return rawName;

    for (auto& v : visibleDevices)
        if (v == rawName) return v;

    // Looking for "<rawName> (<digits>)" — JUCE's disambiguation pattern.
    for (auto& v : visibleDevices) {
        if (v.size() < rawName.size() + 4) continue;           // need " (1)"
        if (v.compare(0, rawName.size(), rawName) != 0) continue;
        size_t i = rawName.size();
        if (v[i] != ' ' || v[i + 1] != '(') continue;
        if (v.back() != ')') continue;
        bool hasDigits = false;
        for (size_t k = i + 2; k + 1 < v.size(); ++k) {
            if (v[k] < '0' || v[k] > '9') { hasDigits = false; break; }
            hasDigits = true;
        }
        if (hasDigits) return v;
    }

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
            if (deviceNameMatches(v, name)) return true;
        return false;
    };

    // Preferred output just appeared (or returned) and we're not on it.
    if (!preferredOutput.empty()
        && !deviceNameMatches(currentOutput, preferredOutput)
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
    for (auto& v : visible) {
        if (v == name) return true;
        if (v.size() < name.size() + 4) continue;          // need " (1)"
        if (v.compare(0, name.size(), name) != 0) continue;
        size_t i = name.size();
        if (v[i] != ' ' || v[i + 1] != '(') continue;
        if (v.back() != ')') continue;
        bool digits = false;
        for (size_t k = i + 2; k + 1 < v.size(); ++k) {
            if (v[k] < '0' || v[k] > '9') { digits = false; break; }
            digits = true;
        }
        if (digits) return true;
    }
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

} // namespace sonicpi::device

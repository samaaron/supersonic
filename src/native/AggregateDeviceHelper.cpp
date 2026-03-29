/*
 * AggregateDeviceHelper.cpp — macOS CoreAudio Aggregate Device management
 *
 * When input and output are different CoreAudio devices, JUCE creates an
 * AudioIODeviceCombiner which has NO drift correction. This causes glitches
 * over time (local devices) or immediate failure (network/wireless devices).
 *
 * Instead, we create a macOS Aggregate Device with kernel-level drift
 * correction, and tell JUCE to use that single device for both I/O.
 *
 * CoreAudio provides no completion callback for aggregate device setup,
 * so each configuration step (create, set sub-devices, set master,
 * set drift comp) is followed by a 100ms RunLoop wait to let the HAL
 * stabilise before the next step.
 */

#ifdef __APPLE__

#include "AggregateDeviceHelper.h"
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <cstdio>
#include <mutex>
#include <vector>

namespace AggregateDeviceHelper {

static const char* kAggregateUID  = "com.sonicpi.supersonic.aggregate";
static const char* kAggregateName = "SuperSonic";

static AudioObjectID sAggregateID = kAudioObjectUnknown;
static std::mutex sMutex;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void runLoopWait() {
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.1, false);
}

static AudioObjectID findDeviceByName(const std::string& name) {
    AudioObjectPropertyAddress pa = {
        kAudioHardwarePropertyDevices,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };

    UInt32 dataSize = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &pa, 0, nullptr, &dataSize) != noErr)
        return kAudioObjectUnknown;

    auto count = dataSize / sizeof(AudioObjectID);
    std::vector<AudioObjectID> ids(count);
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &pa, 0, nullptr, &dataSize, ids.data()) != noErr)
        return kAudioObjectUnknown;

    for (auto id : ids) {
        AudioObjectPropertyAddress nameAddr = {
            kAudioDevicePropertyDeviceNameCFString,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };

        CFStringRef cfName = nullptr;
        UInt32 nameSize = sizeof(cfName);
        if (AudioObjectGetPropertyData(id, &nameAddr, 0, nullptr, &nameSize, &cfName) != noErr)
            continue;

        char buf[256];
        CFStringGetCString(cfName, buf, sizeof(buf), kCFStringEncodingUTF8);
        CFRelease(cfName);

        if (name == buf)
            return id;
    }

    return kAudioObjectUnknown;
}

static std::string getDeviceUID(AudioObjectID deviceID) {
    AudioObjectPropertyAddress pa = {
        kAudioDevicePropertyDeviceUID,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };

    CFStringRef uid = nullptr;
    UInt32 size = sizeof(uid);
    if (AudioObjectGetPropertyData(deviceID, &pa, 0, nullptr, &size, &uid) != noErr)
        return "";

    char buf[256];
    CFStringGetCString(uid, buf, sizeof(buf), kCFStringEncodingUTF8);
    CFRelease(uid);
    return buf;
}

static UInt32 getTransportType(AudioObjectID deviceID) {
    AudioObjectPropertyAddress pa = {
        kAudioDevicePropertyTransportType,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };

    UInt32 transport = 0;
    UInt32 size = sizeof(transport);
    AudioObjectGetPropertyData(deviceID, &pa, 0, nullptr, &size, &transport);
    return transport;
}

static std::string transportTypeString(UInt32 t) {
    // FourCC to readable string
    if (t == 0) return "unknown";
    char cc[5] = {
        (char)((t >> 24) & 0xFF),
        (char)((t >> 16) & 0xFF),
        (char)((t >> 8) & 0xFF),
        (char)(t & 0xFF),
        0
    };
    return cc;
}

// ---------------------------------------------------------------------------
// Clean up any orphaned aggregate from a previous crash
// ---------------------------------------------------------------------------

static void cleanupOrphaned() {
    AudioObjectPropertyAddress pa = {
        kAudioHardwarePropertyDevices,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };

    UInt32 dataSize = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &pa, 0, nullptr, &dataSize) != noErr)
        return;

    auto count = dataSize / sizeof(AudioObjectID);
    std::vector<AudioObjectID> ids(count);
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &pa, 0, nullptr, &dataSize, ids.data()) != noErr)
        return;

    for (auto id : ids) {
        AudioObjectPropertyAddress uidAddr = {
            kAudioDevicePropertyDeviceUID,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };

        CFStringRef uid = nullptr;
        UInt32 uidSize = sizeof(uid);
        if (AudioObjectGetPropertyData(id, &uidAddr, 0, nullptr, &uidSize, &uid) != noErr)
            continue;

        char buf[256];
        CFStringGetCString(uid, buf, sizeof(buf), kCFStringEncodingUTF8);
        CFRelease(uid);

        if (std::string(buf) == kAggregateUID) {
            fprintf(stderr, "[audio-device] cleaning up orphaned SuperSonic aggregate device\n");
            AudioHardwareDestroyAggregateDevice(id);
        }
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

std::string createOrUpdate(const std::string& outputDeviceName,
                           const std::string& inputDeviceName) {
    // Destroy existing aggregate first (destroy-and-recreate pattern)
    {
        std::lock_guard<std::mutex> lock(sMutex);
        if (sAggregateID != kAudioObjectUnknown) {
            AudioHardwareDestroyAggregateDevice(sAggregateID);
            sAggregateID = kAudioObjectUnknown;
        }
    }
    runLoopWait();

    if (outputDeviceName.empty() || inputDeviceName.empty())
        return "";

    // Find device IDs and UIDs
    AudioObjectID outputID = findDeviceByName(outputDeviceName);
    AudioObjectID inputID  = findDeviceByName(inputDeviceName);

    if (outputID == kAudioObjectUnknown || inputID == kAudioObjectUnknown) {
        fprintf(stderr, "[audio-device] aggregate: couldn't find devices: out='%s' in='%s'\n",
                outputDeviceName.c_str(), inputDeviceName.c_str());
        return "";
    }

    // If same device, no aggregate needed
    if (outputID == inputID)
        return "";

    std::string outputUID = getDeviceUID(outputID);
    std::string inputUID  = getDeviceUID(inputID);

    if (outputUID.empty() || inputUID.empty()) {
        fprintf(stderr, "[audio-device] aggregate: couldn't get UIDs\n");
        return "";
    }

    // Log transport types for diagnostics
    UInt32 outTransport = getTransportType(outputID);
    UInt32 inTransport  = getTransportType(inputID);
    fprintf(stderr, "[audio-device] aggregate: out='%s' transport=%s, in='%s' transport=%s\n",
            outputDeviceName.c_str(), transportTypeString(outTransport).c_str(),
            inputDeviceName.c_str(), transportTypeString(inTransport).c_str());

    // Clean up any orphaned aggregate from a previous crash
    static bool cleaned = false;
    if (!cleaned) {
        cleanupOrphaned();
        cleaned = true;
    }

    // ── Step 1: Create empty aggregate device ────────────────────────────
    // Following Ardour's pattern: create first, then configure in steps
    // with RunLoop waits between each to let CoreAudio stabilise.

    CFStringRef uidRef  = CFStringCreateWithCString(nullptr, kAggregateUID, kCFStringEncodingUTF8);
    CFStringRef nameRef = CFStringCreateWithCString(nullptr, kAggregateName, kCFStringEncodingUTF8);

    int privateVal = 1;  // private — hidden from device lists (Ardour pattern)
    CFNumberRef privateRef = CFNumberCreate(nullptr, kCFNumberIntType, &privateVal);

    const void* descKeys[] = {
        CFSTR(kAudioAggregateDeviceUIDKey),
        CFSTR(kAudioAggregateDeviceNameKey),
        CFSTR(kAudioAggregateDeviceIsPrivateKey),
    };
    const void* descVals[] = {
        uidRef,
        nameRef,
        privateRef,
    };
    CFDictionaryRef desc = CFDictionaryCreate(nullptr,
        descKeys, descVals, 3,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

    AudioObjectID newID = kAudioObjectUnknown;
    OSStatus err = AudioHardwareCreateAggregateDevice(desc, &newID);

    CFRelease(desc);
    CFRelease(privateRef);
    CFRelease(nameRef);
    CFRelease(uidRef);

    if (err != noErr) {
        fprintf(stderr, "[audio-device] aggregate: creation failed (err %d)\n", (int)err);
        return "";
    }

    // Wait for CoreAudio to register the new device
    runLoopWait();

    // ── Step 2: Set sub-device list ──────────────────────────────────────

    CFStringRef outUIDRef = CFStringCreateWithCString(nullptr, outputUID.c_str(), kCFStringEncodingUTF8);
    CFStringRef inUIDRef  = CFStringCreateWithCString(nullptr, inputUID.c_str(), kCFStringEncodingUTF8);

    CFMutableArrayRef subDevicesArray = CFArrayCreateMutable(nullptr, 0, &kCFTypeArrayCallBacks);
    CFArrayAppendValue(subDevicesArray, outUIDRef);
    CFArrayAppendValue(subDevicesArray, inUIDRef);

    AudioObjectPropertyAddress subDevAddr = {
        kAudioAggregateDevicePropertyFullSubDeviceList,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    UInt32 subDevSize = sizeof(CFArrayRef);
    err = AudioObjectSetPropertyData(newID, &subDevAddr, 0, nullptr, subDevSize, &subDevicesArray);
    CFRelease(subDevicesArray);

    if (err != noErr) {
        fprintf(stderr, "[audio-device] aggregate: failed to set sub-devices (err %d)\n", (int)err);
        AudioHardwareDestroyAggregateDevice(newID);
        CFRelease(outUIDRef);
        CFRelease(inUIDRef);
        return "";
    }

    // Wait for sub-device list to take effect
    runLoopWait();

    // ── Step 3: Set master (clock source) device ─────────────────────────
    // Output device provides the clock — input device gets drift correction.

    AudioObjectPropertyAddress masterAddr = {
        kAudioAggregateDevicePropertyMasterSubDevice,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    UInt32 masterSize = sizeof(CFStringRef);
    err = AudioObjectSetPropertyData(newID, &masterAddr, 0, nullptr, masterSize, &outUIDRef);

    if (err != noErr) {
        fprintf(stderr, "[audio-device] aggregate: failed to set master device (err %d), "
                "trying input as master\n", (int)err);
        // Fall back to input as master (like Ardour)
        err = AudioObjectSetPropertyData(newID, &masterAddr, 0, nullptr, masterSize, &inUIDRef);
        if (err != noErr) {
            fprintf(stderr, "[audio-device] aggregate: failed to set any master (err %d)\n", (int)err);
            AudioHardwareDestroyAggregateDevice(newID);
            CFRelease(outUIDRef);
            CFRelease(inUIDRef);
            return "";
        }
    }

    // Wait for master assignment to take effect
    runLoopWait();

    // ── Step 4: Enable drift correction if sub-devices use different clocks ──
    // Check clock domains first (like Ardour) — devices on the same clock
    // don't need drift compensation and may not support the property.

    bool needsDriftComp = false;
    {
        AudioObjectPropertyAddress clockAddr = {
            kAudioDevicePropertyClockDomain,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };
        UInt32 outClock = 0, inClock = 0;
        UInt32 clockSize = sizeof(UInt32);
        OSStatus outErr = AudioObjectGetPropertyData(outputID, &clockAddr, 0, nullptr, &clockSize, &outClock);
        clockSize = sizeof(UInt32);
        OSStatus inErr  = AudioObjectGetPropertyData(inputID, &clockAddr, 0, nullptr, &clockSize, &inClock);

        if (outErr != noErr || inErr != noErr) {
            // Can't determine clock domains — assume drift comp needed
            needsDriftComp = true;
            fprintf(stderr, "[audio-device] aggregate: couldn't read clock domains, assuming drift comp needed\n");
        } else if (outClock != inClock) {
            needsDriftComp = true;
            fprintf(stderr, "[audio-device] aggregate: different clock domains (%u vs %u), enabling drift comp\n",
                    outClock, inClock);
        } else {
            fprintf(stderr, "[audio-device] aggregate: same clock domain (%u), skipping drift comp\n", outClock);
        }
    }

    if (needsDriftComp) {
        AudioObjectPropertyAddress ownedAddr = {
            kAudioObjectPropertyOwnedObjects,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };
        UInt32 ownedSize = 0;
        err = AudioObjectGetPropertyDataSize(newID, &ownedAddr, 0, nullptr, &ownedSize);
        if (err == noErr && ownedSize > 0) {
            auto nSubDevices = ownedSize / sizeof(AudioObjectID);
            std::vector<AudioObjectID> subDevices(nSubDevices);
            err = AudioObjectGetPropertyData(newID, &ownedAddr, 0, nullptr, &ownedSize, subDevices.data());
            if (err == noErr) {
                AudioObjectPropertyAddress driftAddr = {
                    kAudioSubDevicePropertyDriftCompensation,
                    kAudioObjectPropertyScopeGlobal,
                    kAudioObjectPropertyElementMain
                };
                // Skip first sub-device (master/clock source), enable drift on rest
                for (size_t i = 1; i < nSubDevices; ++i) {
                    UInt32 driftVal = 1;
                    OSStatus driftErr = AudioObjectSetPropertyData(
                        subDevices[i], &driftAddr, 0, nullptr, sizeof(UInt32), &driftVal);
                    if (driftErr != noErr) {
                        fprintf(stderr, "[audio-device] aggregate: drift comp failed on sub-device %zu (err %d)\n",
                                i, (int)driftErr);
                    } else {
                        fprintf(stderr, "[audio-device] aggregate: drift comp enabled on sub-device %zu\n", i);
                    }
                }
            }
        }
    }

    CFRelease(outUIDRef);
    CFRelease(inUIDRef);

    // Final wait for drift compensation to take effect
    runLoopWait();

    // Store the ID under the lock
    {
        std::lock_guard<std::mutex> lock(sMutex);
        sAggregateID = newID;
    }

    fprintf(stderr, "[audio-device] aggregate: created '%s' (out=%s, in=%s)\n",
            kAggregateName, outputDeviceName.c_str(), inputDeviceName.c_str());

    return kAggregateName;
}

void destroy() {
    std::lock_guard<std::mutex> lock(sMutex);

    if (sAggregateID != kAudioObjectUnknown) {
        fprintf(stderr, "[audio-device] aggregate: destroying '%s'\n", kAggregateName);
        AudioHardwareDestroyAggregateDevice(sAggregateID);
        sAggregateID = kAudioObjectUnknown;
    }
}

bool exists() {
    std::lock_guard<std::mutex> lock(sMutex);
    return sAggregateID != kAudioObjectUnknown;
}

std::string currentName() {
    std::lock_guard<std::mutex> lock(sMutex);
    return (sAggregateID != kAudioObjectUnknown) ? kAggregateName : "";
}

} // namespace AggregateDeviceHelper

#endif // __APPLE__

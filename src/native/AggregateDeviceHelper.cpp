/*
 * AggregateDeviceHelper.cpp — macOS CoreAudio Aggregate Device management
 *
 * When input and output are different CoreAudio devices, JUCE creates an
 * AudioIODeviceCombiner which has NO drift correction. This causes glitches
 * over time (local devices) or immediate failure (network/wireless devices).
 *
 * Instead, we create a macOS Aggregate Device with kernel-level drift
 * correction, and tell JUCE to use that single device for both I/O.
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
// Helpers to get device UID from device name
// ---------------------------------------------------------------------------

static AudioObjectID findDeviceByName(const std::string& name) {
    AudioObjectPropertyAddress pa;
    pa.mSelector = kAudioHardwarePropertyDevices;
    pa.mScope    = kAudioObjectPropertyScopeGlobal;
    pa.mElement  = kAudioObjectPropertyElementMain;

    UInt32 dataSize = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &pa, 0, nullptr, &dataSize) != noErr)
        return kAudioObjectUnknown;

    auto count = dataSize / sizeof(AudioObjectID);
    std::vector<AudioObjectID> ids(count);
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &pa, 0, nullptr, &dataSize, ids.data()) != noErr)
        return kAudioObjectUnknown;

    for (auto id : ids) {
        AudioObjectPropertyAddress nameAddr;
        nameAddr.mSelector = kAudioDevicePropertyDeviceNameCFString;
        nameAddr.mScope    = kAudioObjectPropertyScopeGlobal;
        nameAddr.mElement  = kAudioObjectPropertyElementMain;

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
    AudioObjectPropertyAddress pa;
    pa.mSelector = kAudioDevicePropertyDeviceUID;
    pa.mScope    = kAudioObjectPropertyScopeGlobal;
    pa.mElement  = kAudioObjectPropertyElementMain;

    CFStringRef uid = nullptr;
    UInt32 size = sizeof(uid);
    if (AudioObjectGetPropertyData(deviceID, &pa, 0, nullptr, &size, &uid) != noErr)
        return "";

    char buf[256];
    CFStringGetCString(uid, buf, sizeof(buf), kCFStringEncodingUTF8);
    CFRelease(uid);
    return buf;
}

// ---------------------------------------------------------------------------
// Clean up any orphaned aggregate from a previous crash
// ---------------------------------------------------------------------------

static void cleanupOrphaned() {
    AudioObjectPropertyAddress pa;
    pa.mSelector = kAudioHardwarePropertyDevices;
    pa.mScope    = kAudioObjectPropertyScopeGlobal;
    pa.mElement  = kAudioObjectPropertyElementMain;

    UInt32 dataSize = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &pa, 0, nullptr, &dataSize) != noErr)
        return;

    auto count = dataSize / sizeof(AudioObjectID);
    std::vector<AudioObjectID> ids(count);
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &pa, 0, nullptr, &dataSize, ids.data()) != noErr)
        return;

    for (auto id : ids) {
        AudioObjectPropertyAddress uidAddr;
        uidAddr.mSelector = kAudioDevicePropertyDeviceUID;
        uidAddr.mScope    = kAudioObjectPropertyScopeGlobal;
        uidAddr.mElement  = kAudioObjectPropertyElementMain;

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
    // Let CoreAudio process the destroy outside the lock
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.1, false);

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

    // Clean up any orphaned aggregate from a previous crash
    static bool cleaned = false;
    if (!cleaned) {
        cleanupOrphaned();
        cleaned = true;
    }

    // Build sub-device descriptions
    // Output = clock source (no drift correction)
    // Input  = drift correction enabled
    CFStringRef outUIDRef = CFStringCreateWithCString(nullptr, outputUID.c_str(), kCFStringEncodingUTF8);
    CFStringRef inUIDRef  = CFStringCreateWithCString(nullptr, inputUID.c_str(), kCFStringEncodingUTF8);

    const void* outSubKeys[] = {
        CFSTR(kAudioSubDeviceUIDKey),
        CFSTR(kAudioSubDeviceDriftCompensationKey)
    };
    const void* outSubVals[] = {
        outUIDRef,
        kCFBooleanFalse  // clock source — no drift correction
    };
    CFDictionaryRef outSubDict = CFDictionaryCreate(nullptr,
        outSubKeys, outSubVals, 2,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

    const void* inSubKeys[] = {
        CFSTR(kAudioSubDeviceUIDKey),
        CFSTR(kAudioSubDeviceDriftCompensationKey)
    };
    const void* inSubVals[] = {
        inUIDRef,
        kCFBooleanTrue   // non-clock device — enable drift correction
    };
    CFDictionaryRef inSubDict = CFDictionaryCreate(nullptr,
        inSubKeys, inSubVals, 2,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

    const void* subDevices[] = { outSubDict, inSubDict };
    CFArrayRef subDeviceList = CFArrayCreate(nullptr, subDevices, 2,
        &kCFTypeArrayCallBacks);

    // Build aggregate description
    CFStringRef uidRef  = CFStringCreateWithCString(nullptr, kAggregateUID, kCFStringEncodingUTF8);
    CFStringRef nameRef = CFStringCreateWithCString(nullptr, kAggregateName, kCFStringEncodingUTF8);

    const void* descKeys[] = {
        CFSTR(kAudioAggregateDeviceUIDKey),
        CFSTR(kAudioAggregateDeviceNameKey),
        CFSTR(kAudioAggregateDeviceSubDeviceListKey),
        CFSTR(kAudioAggregateDeviceMasterSubDeviceKey),
        CFSTR(kAudioAggregateDeviceIsPrivateKey),
    };
    int privateVal = 0;  // public so JUCE can see it in device enumeration
    CFNumberRef privateRef = CFNumberCreate(nullptr, kCFNumberIntType, &privateVal);
    const void* descVals[] = {
        uidRef,
        nameRef,
        subDeviceList,
        outUIDRef,       // output device is clock source
        privateRef,
    };
    CFDictionaryRef desc = CFDictionaryCreate(nullptr,
        descKeys, descVals, 5,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

    // Create the aggregate device
    AudioObjectID newID = kAudioObjectUnknown;
    OSStatus err = AudioHardwareCreateAggregateDevice(desc, &newID);

    // Release CF objects
    CFRelease(desc);
    CFRelease(privateRef);
    CFRelease(uidRef);
    CFRelease(nameRef);
    CFRelease(subDeviceList);
    CFRelease(outSubDict);
    CFRelease(inSubDict);
    CFRelease(outUIDRef);
    CFRelease(inUIDRef);

    if (err != noErr) {
        fprintf(stderr, "[audio-device] aggregate: creation failed (err %d)\n", (int)err);
        return "";
    }

    // Store the ID under the lock
    {
        std::lock_guard<std::mutex> lock(sMutex);
        sAggregateID = newID;
    }

    // Let CoreAudio stabilize outside the lock
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.1, false);

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

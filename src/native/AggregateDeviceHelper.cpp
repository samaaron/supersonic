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
#include "DeviceInfo.h"
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace AggregateDeviceHelper {

static const char* kAggregateUIDBase  = "com.sonicpi.supersonic.aggregate";
static const char* kAggregateNameBase = "SuperSonic";

static AudioObjectID sAggregateID = kAudioObjectUnknown;
static AudioObjectID sPrevAggregateID = kAudioObjectUnknown;
static std::string  sCurrentName;
static std::atomic<int> sAggregateCounter{0};
static std::mutex sMutex;
// Whether the current aggregate has CoreAudio drift compensation enabled
// on its input sub-device. True when the sub-devices reported different
// clock domains (separate USB / PCI clocks); false when they share a clock
// (e.g. MBP built-in speakers + built-in mic — same Apple Silicon audio
// block). Dropdown filter uses this to decide whether to enforce the 256-
// sample floor on buffer sizes.
static std::atomic<bool> sDriftCompEnabled{false};

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

void cleanupOrphaned() {
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

        // Match any of our aggregate UIDs (we now suffix them with a counter).
        std::string sUid(buf);
        bool isOurs = (sUid.rfind(kAggregateUIDBase, 0) == 0);
        if (isOurs) {
            fprintf(stderr, "[audio-device] cleaning up orphaned SuperSonic aggregate device\n");
            AudioHardwareDestroyAggregateDevice(id);
        }
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

std::string createOrUpdate(const std::string& outputDeviceName,
                           const std::string& inputDeviceName,
                           double desiredSampleRate) {
    // Don't destroy the existing aggregate here: JUCE's CoreAudioIODevice
    // still holds a reference to it, and its later AudioComponentInstance-
    // Dispose would hit a dangling CoreAudio id and crash. Stash the old
    // aggregate in sPrevAggregateID; caller invokes destroyPrevious()
    // AFTER setAudioDeviceSetup has moved JUCE off it.
    {
        std::lock_guard<std::mutex> lock(sMutex);
        // Back-to-back switch: JUCE is already on the most recent device,
        // so an earlier pending previous is now safe to destroy.
        if (sPrevAggregateID != kAudioObjectUnknown) {
            AudioHardwareDestroyAggregateDevice(sPrevAggregateID);
            sPrevAggregateID = kAudioObjectUnknown;
        }
        sPrevAggregateID = sAggregateID;
        sAggregateID = kAudioObjectUnknown;
    }

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

    // Pre-set sample rate on both sub-devices so the aggregate doesn't have
    // to negotiate rates at open time (Ardour's pattern). This is especially
    // important for virtual devices whose default rate may differ from the
    // master's — leaving them mismatched causes CoreAudio to stop the
    // aggregate within a callback.
    auto setDeviceRate = [](AudioObjectID devID, double rate, bool isInput) {
        AudioObjectPropertyAddress rateAddr = {
            kAudioDevicePropertyNominalSampleRate,
            isInput ? kAudioObjectPropertyScopeInput : kAudioObjectPropertyScopeOutput,
            kAudioObjectPropertyElementMain
        };
        Float64 curRate = 0;
        UInt32 sz = sizeof(curRate);
        AudioObjectGetPropertyData(devID, &rateAddr, 0, nullptr, &sz, &curRate);
        if ((int)curRate == (int)rate) return;
        OSStatus err = AudioObjectSetPropertyData(devID, &rateAddr, 0, nullptr,
                                                  sizeof(rate), &rate);
        if (err != noErr) {
            fprintf(stderr, "[aggregate] sample rate set err=%d on id=%u\n", (int)err, (unsigned)devID);
            fflush(stderr);
        }
        // Brief wait for CoreAudio to apply
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.1, false);
    };
    // Pick the rate we'll force on both sub-devices. Priority:
    //   1. caller's desiredSampleRate (the engine's current rate — most
    //      important, because scsynth processes audio at that rate and
    //      any mismatch with the aggregate forces aggregate-level SRC
    //      inside CoreAudio, producing distortion).
    //   2. input device's current rate (legacy fallback).
    //   3. 48000 (last-resort default).
    AudioObjectPropertyAddress inRateAddr = {
        kAudioDevicePropertyNominalSampleRate,
        kAudioObjectPropertyScopeInput,
        kAudioObjectPropertyElementMain
    };
    Float64 masterRate = desiredSampleRate > 0 ? desiredSampleRate : 0.0;
    if (masterRate <= 0) {
        UInt32 rateSz = sizeof(masterRate);
        AudioObjectGetPropertyData(inputID, &inRateAddr, 0, nullptr, &rateSz, &masterRate);
    }
    if (masterRate <= 0) masterRate = 48000.0;
    fprintf(stderr, "[aggregate] pre-aligning sub-devices to %.0f Hz "
            "(desired=%.0f)\n", masterRate, desiredSampleRate);
    fflush(stderr);
    setDeviceRate(inputID, masterRate, true);
    setDeviceRate(outputID, masterRate, false);

    // Read back what each sub-device actually settled at — if either
    // sub-device refused our rate (e.g. MOTU pinned to 96k by its
    // control panel), we'll get distortion from aggregate-level SRC.
    // Logging this makes the diagnosis obvious.
    {
        Float64 actualIn = 0, actualOut = 0;
        UInt32 sz = sizeof(Float64);
        AudioObjectGetPropertyData(inputID, &inRateAddr, 0, nullptr, &sz, &actualIn);
        sz = sizeof(Float64);
        AudioObjectPropertyAddress outRateAddr = {
            kAudioDevicePropertyNominalSampleRate,
            kAudioObjectPropertyScopeOutput,
            kAudioObjectPropertyElementMain
        };
        AudioObjectGetPropertyData(outputID, &outRateAddr, 0, nullptr, &sz, &actualOut);
        fprintf(stderr, "[aggregate] actual rates: in=%.0f out=%.0f (requested %.0f)%s\n",
                actualIn, actualOut, masterRate,
                (int)actualIn != (int)masterRate || (int)actualOut != (int)masterRate
                    ? " — MISMATCH, expect aggregate-level SRC distortion" : "");
        fflush(stderr);
    }

    // Orphan cleanup now runs at boot via SupersonicEngine::init()

    // ── Step 1: Create empty aggregate device ────────────────────────────
    // Each new aggregate gets a unique UID and name suffix. Reusing the
    // same UID causes JUCE's setAudioDeviceSetup to assume "same device"
    // and not reopen — the new aggregate then starts with stale state
    // and produces no callbacks. Incrementing a counter sidesteps this.
    int n = sAggregateCounter.fetch_add(1) + 1;
    char uidBuf[128], nameBuf[64];
    snprintf(uidBuf, sizeof(uidBuf), "%s.%d", kAggregateUIDBase, n);
    snprintf(nameBuf, sizeof(nameBuf), "%s#%d", kAggregateNameBase, n);

    CFStringRef uidRef  = CFStringCreateWithCString(nullptr, uidBuf, kCFStringEncodingUTF8);
    CFStringRef nameRef = CFStringCreateWithCString(nullptr, nameBuf, kCFStringEncodingUTF8);

    // NOTE: kAudioAggregateDeviceIsPrivateKey is known to be flaky on some
    // macOS versions — silently succeeds but leaves properties half-applied,
    // which breaks drift compensation. Create a public aggregate (visible in
    // Audio MIDI Setup as "SuperSonic") instead. We clean up orphaned ones
    // at boot, so there's no persistence issue.
    const void* descKeys[] = {
        CFSTR(kAudioAggregateDeviceUIDKey),
        CFSTR(kAudioAggregateDeviceNameKey),
    };
    const void* descVals[] = {
        uidRef,
        nameRef,
    };
    CFDictionaryRef desc = CFDictionaryCreate(nullptr,
        descKeys, descVals, 2,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

    AudioObjectID newID = kAudioObjectUnknown;
    OSStatus err = AudioHardwareCreateAggregateDevice(desc, &newID);

    CFRelease(desc);
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

    // Order matters: CoreAudio concatenates output channels from sub-
    // devices in list order to form the aggregate's output channel layout.
    // If the input sub-device (e.g. a MOTU with its own outputs) is first,
    // its outputs become aggregate channels 0..N and scsynth writing to
    // channels 0-1 would land on MOTU, not on the intended output device.
    // Put the OUTPUT sub-device first so its channels come first.
    //
    // Clock mastering is set explicitly via kAudioAggregateDevicePropertyMasterSubDevice
    // below — it's independent of sub-device list order.
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
    // The master MUST be a device with a real hardware clock.  Virtual
    // devices (Loopback, Blackhole) are driven by the OS scheduler and
    // have no stable hardware timestamps — using them as master causes
    // CoreAudio's drift-compensation SRC to crash inside AudioUnitRender.
    // Ardour's CoreAudio backend uses the same order: try INPUT (capture)
    // first, fall back to OUTPUT (playback) — because the capture device
    // is typically hardware while playback may be virtual.

    AudioObjectPropertyAddress masterAddr = {
        kAudioAggregateDevicePropertyMasterSubDevice,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    UInt32 masterSize = sizeof(CFStringRef);

    // Choose master: prefer whichever side is hardware. If both are hardware
    // (the common case), prefer input (mic) — matches Ardour. If one side is
    // virtual, that side cannot be master.
    // Default: hardware input as master. Only fall back to output-as-master
    // if the input is virtual but the output is hardware.
    CFStringRef* firstTry  = &inUIDRef;
    CFStringRef* secondTry = &outUIDRef;
    const char*  firstName  = "input";
    const char*  secondName = "output";
    if (CoreAudioTransport::isVirtual(inTransport)
        && !CoreAudioTransport::isVirtual(outTransport)) {
        firstTry = &outUIDRef; secondTry = &inUIDRef;
        firstName = "output"; secondName = "input";
    }

    err = AudioObjectSetPropertyData(newID, &masterAddr, 0, nullptr, masterSize, firstTry);
    CFStringRef masterUIDRef = *firstTry;

    if (err != noErr) {
        fprintf(stderr, "[audio-device] aggregate: %s-master failed (err %d), trying %s\n",
                firstName, (int)err, secondName);
        fflush(stderr);
        err = AudioObjectSetPropertyData(newID, &masterAddr, 0, nullptr, masterSize, secondTry);
        if (err != noErr) {
            fprintf(stderr, "[audio-device] aggregate: no master could be set (err %d)\n", (int)err);
            fflush(stderr);
            AudioHardwareDestroyAggregateDevice(newID);
            CFRelease(outUIDRef);
            CFRelease(inUIDRef);
            return "";
        }
        masterUIDRef = *secondTry;
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
    sDriftCompEnabled.store(needsDriftComp);

    if (needsDriftComp) {
        // Query OwnedObjects with a SubDevice class qualifier so we only
        // get actual sub-devices (not clocks/taps which don't support
        // DriftCompensation and return 'who?').
        AudioObjectPropertyAddress ownedAddr = {
            kAudioObjectPropertyOwnedObjects,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };
        UInt32 qualifierDataSize = sizeof(AudioObjectID);
        AudioClassID inClass = kAudioSubDeviceClassID;
        UInt32 ownedSize = 0;
        OSStatus szErr = noErr;
        // Aggregates containing virtual sub-devices (Loopback, Blackhole)
        // take time to expose their owned sub-device objects. Poll up to
        // ~1 second for them to appear.
        for (int i = 0; i < 10; ++i) {
            szErr = AudioObjectGetPropertyDataSize(newID, &ownedAddr,
                qualifierDataSize, &inClass, &ownedSize);
            if (szErr == noErr && ownedSize > 0) break;
            runLoopWait();
        }
        if (szErr == noErr && ownedSize > 0) {
            auto nSubDevices = ownedSize / sizeof(AudioObjectID);
            std::vector<AudioObjectID> subDevices(nSubDevices);
            OSStatus getErr = AudioObjectGetPropertyData(newID, &ownedAddr,
                qualifierDataSize, &inClass, &ownedSize, subDevices.data());
            if (getErr == noErr) {
                AudioObjectPropertyAddress driftAddr = {
                    kAudioSubDevicePropertyDriftCompensation,
                    kAudioObjectPropertyScopeGlobal,
                    kAudioObjectPropertyElementMain
                };
                AudioObjectPropertyAddress uidAddr = {
                    kAudioDevicePropertyDeviceUID,
                    kAudioObjectPropertyScopeGlobal,
                    kAudioObjectPropertyElementMain
                };
                // Enable drift on every sub-device EXCEPT the master.
                // Master is the clock source — drift comp doesn't apply
                // to it. We identify the master by comparing each sub-
                // device's UID against masterUIDRef (the UID we passed
                // to kAudioAggregateDevicePropertyMasterSubDevice).
                // Index-based skipping (old code) was wrong once the
                // sub-device list order stopped matching the master
                // choice.
                for (size_t i = 0; i < nSubDevices; ++i) {
                    CFStringRef subUID = nullptr;
                    UInt32 uidSize = sizeof(subUID);
                    if (AudioObjectGetPropertyData(subDevices[i], &uidAddr, 0, nullptr, &uidSize, &subUID) != noErr || !subUID) {
                        continue;
                    }
                    bool isMaster = (CFStringCompare(subUID, masterUIDRef, 0) == kCFCompareEqualTo);
                    CFRelease(subUID);
                    if (isMaster) continue;
                    UInt32 driftVal = 1;
                    AudioObjectSetPropertyData(
                        subDevices[i], &driftAddr, 0, nullptr, sizeof(UInt32), &driftVal);
                }
            }
        } else {
            fprintf(stderr, "[audio-device] aggregate: drift comp skipped — no sub-devices exposed after 1s poll\n");
            fflush(stderr);
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

    fprintf(stderr, "[audio-device] aggregate: created '%s' (out=%s, in=%s) id=%u\n",
            nameBuf, outputDeviceName.c_str(), inputDeviceName.c_str(),
            (unsigned)newID);
    fflush(stderr);


    // Store the current (unique) name so listDevices can filter it out
    {
        std::lock_guard<std::mutex> lock(sMutex);
        sCurrentName = nameBuf;
    }

    return nameBuf;
}

void destroy() {
    std::lock_guard<std::mutex> lock(sMutex);

    if (sAggregateID != kAudioObjectUnknown) {
        fprintf(stderr, "[audio-device] aggregate: destroying '%s'\n", sCurrentName.c_str());
        AudioHardwareDestroyAggregateDevice(sAggregateID);
        sAggregateID = kAudioObjectUnknown;
        sCurrentName.clear();
    }
    // Also clear any pending previous aggregate — shutdown path.
    if (sPrevAggregateID != kAudioObjectUnknown) {
        AudioHardwareDestroyAggregateDevice(sPrevAggregateID);
        sPrevAggregateID = kAudioObjectUnknown;
    }
    sDriftCompEnabled.store(false);
}

bool driftCompensationEnabled() {
    return sDriftCompEnabled.load();
}

void destroyPrevious() {
    AudioObjectID id = kAudioObjectUnknown;
    {
        std::lock_guard<std::mutex> lock(sMutex);
        id = sPrevAggregateID;
        sPrevAggregateID = kAudioObjectUnknown;
    }
    if (id != kAudioObjectUnknown) {
        AudioHardwareDestroyAggregateDevice(id);
    }
}

bool exists() {
    std::lock_guard<std::mutex> lock(sMutex);
    return sAggregateID != kAudioObjectUnknown;
}

std::string currentName() {
    std::lock_guard<std::mutex> lock(sMutex);
    return (sAggregateID != kAudioObjectUnknown) ? sCurrentName : "";
}

} // namespace AggregateDeviceHelper

#endif // __APPLE__

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
    fprintf(stderr, "[aggregate] createOrUpdate ENTER out='%s' in='%s' existing=%u\n",
            outputDeviceName.c_str(), inputDeviceName.c_str(),
            (unsigned)sAggregateID);
    fflush(stderr);

    // Destroy existing aggregate first (destroy-and-recreate pattern)
    {
        std::lock_guard<std::mutex> lock(sMutex);
        if (sAggregateID != kAudioObjectUnknown) {
            fprintf(stderr, "[aggregate] destroying existing id=%u before recreate\n",
                    (unsigned)sAggregateID);
            fflush(stderr);
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
        // Check current rate first
        Float64 curRate = 0;
        UInt32 sz = sizeof(curRate);
        AudioObjectGetPropertyData(devID, &rateAddr, 0, nullptr, &sz, &curRate);
        if ((int)curRate == (int)rate) return;
        fprintf(stderr, "[aggregate] pre-setting sample rate on id=%u from %.0f to %.0f (%s)\n",
                (unsigned)devID, curRate, rate, isInput ? "input" : "output");
        fflush(stderr);
        OSStatus err = AudioObjectSetPropertyData(devID, &rateAddr, 0, nullptr,
                                                  sizeof(rate), &rate);
        if (err != noErr) {
            fprintf(stderr, "[aggregate] sample rate set err=%d on id=%u\n", (int)err, (unsigned)devID);
            fflush(stderr);
        }
        // Brief wait for CoreAudio to apply
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.1, false);
    };
    // Get the master (input) device's current rate, then align output to it
    AudioObjectPropertyAddress inRateAddr = {
        kAudioDevicePropertyNominalSampleRate,
        kAudioObjectPropertyScopeInput,
        kAudioObjectPropertyElementMain
    };
    Float64 masterRate = 48000.0;
    UInt32 rateSz = sizeof(masterRate);
    AudioObjectGetPropertyData(inputID, &inRateAddr, 0, nullptr, &rateSz, &masterRate);
    if (masterRate <= 0) masterRate = 48000.0;
    setDeviceRate(inputID, masterRate, true);
    setDeviceRate(outputID, masterRate, false);
    fprintf(stderr, "[aggregate] sub-device rates aligned at %.0f Hz\n", masterRate);
    fflush(stderr);

    // Orphan cleanup now runs at boot via SupersonicEngine::initialise()

    // ── Step 1: Create empty aggregate device ────────────────────────────
    // Following Ardour's pattern: create first, then configure in steps
    // with RunLoop waits between each to let CoreAudio stabilise.

    CFStringRef uidRef  = CFStringCreateWithCString(nullptr, kAggregateUID, kCFStringEncodingUTF8);
    CFStringRef nameRef = CFStringCreateWithCString(nullptr, kAggregateName, kCFStringEncodingUTF8);

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

    // Order matters: the FIRST sub-device in this list becomes the default
    // clock master if the master-device property isn't set later, and
    // drift-compensation logic skips index 0 assuming it's the master.
    // Put INPUT first because it's the hardware clock source we prefer.
    CFMutableArrayRef subDevicesArray = CFArrayCreateMutable(nullptr, 0, &kCFTypeArrayCallBacks);
    CFArrayAppendValue(subDevicesArray, inUIDRef);
    CFArrayAppendValue(subDevicesArray, outUIDRef);

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
    bool outIsVirtual = (outTransport == 0x76697274); // 'virt'
    bool inIsVirtual  = (inTransport == 0x76697274);
    CFStringRef* firstTry  = &inUIDRef;   // default: input as master
    CFStringRef* secondTry = &outUIDRef;
    const char*  firstName  = "input";
    const char*  secondName = "output";
    if (inIsVirtual && !outIsVirtual) {
        firstTry = &outUIDRef; secondTry = &inUIDRef;
        firstName = "output"; secondName = "input";
    }

    fprintf(stderr, "[audio-device] aggregate: setting %s as master clock\n", firstName);
    fflush(stderr);
    err = AudioObjectSetPropertyData(newID, &masterAddr, 0, nullptr, masterSize, firstTry);

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

    // Previously disabled drift comp for virtual outputs, but that caused
    // the aggregate to expose ZERO input streams on the input scope —
    // CoreAudio needs drift comp enabled to sync a virtual output's
    // scheduler clock with the hardware input master. Re-enabling.
    (void)outIsVirtual;

    if (needsDriftComp) {
        // Use Ardour's exact pattern: query OwnedObjects with a class-ID
        // qualifier to get only the SubDevice children. Without the
        // qualifier the owned list includes clocks/taps that don't support
        // DriftCompensation (we saw 'who?' errors on them).
        AudioObjectPropertyAddress ownedAddr = {
            kAudioObjectPropertyOwnedObjects,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };
        UInt32 qualifierDataSize = sizeof(AudioObjectID);
        AudioClassID inClass = kAudioSubDeviceClassID;
        UInt32 ownedSize = 0;
        OSStatus szErr = AudioObjectGetPropertyDataSize(newID, &ownedAddr,
            qualifierDataSize, &inClass, &ownedSize);
        fprintf(stderr, "[aggregate] owned-objects query: err=%d size=%u (filter=%u)\n",
                (int)szErr, (unsigned)ownedSize, (unsigned)inClass);
        fflush(stderr);
        if (szErr == noErr && ownedSize > 0) {
            auto nSubDevices = ownedSize / sizeof(AudioObjectID);
            std::vector<AudioObjectID> subDevices(nSubDevices);
            OSStatus getErr = AudioObjectGetPropertyData(newID, &ownedAddr,
                qualifierDataSize, &inClass, &ownedSize, subDevices.data());
            fprintf(stderr, "[aggregate] got %zu sub-devices (getErr=%d)\n",
                    nSubDevices, (int)getErr);
            fflush(stderr);
            if (getErr == noErr) {
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
                    fprintf(stderr, "[audio-device] aggregate: drift comp sub-device %zu (id=%u) err=%d\n",
                            i, (unsigned)subDevices[i], (int)driftErr);
                    fflush(stderr);
                }
            }
        } else {
            fprintf(stderr, "[aggregate] drift comp skipped: no sub-devices found\n");
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
            kAggregateName, outputDeviceName.c_str(), inputDeviceName.c_str(),
            (unsigned)newID);
    fflush(stderr);

    // Diagnostic: check what streams the aggregate exposes on input scope.
    {
        AudioObjectPropertyAddress streamsAddr = {
            kAudioDevicePropertyStreams,
            kAudioObjectPropertyScopeInput,
            kAudioObjectPropertyElementMain
        };
        UInt32 streamsSize = 0;
        if (AudioObjectGetPropertyDataSize(newID, &streamsAddr, 0, nullptr, &streamsSize) == noErr) {
            auto nStreams = streamsSize / sizeof(AudioStreamID);
            fprintf(stderr, "[aggregate] input streams count: %zu (size=%u)\n",
                    nStreams, (unsigned)streamsSize);
            std::vector<AudioStreamID> streams(nStreams);
            if (AudioObjectGetPropertyData(newID, &streamsAddr, 0, nullptr, &streamsSize, streams.data()) == noErr) {
                for (auto s : streams) {
                    AudioObjectPropertyAddress fmtAddr = {
                        kAudioStreamPropertyPhysicalFormat,
                        kAudioObjectPropertyScopeGlobal,
                        kAudioObjectPropertyElementMain
                    };
                    AudioStreamBasicDescription fmt{};
                    UInt32 fmtSz = sizeof(fmt);
                    AudioObjectGetPropertyData(s, &fmtAddr, 0, nullptr, &fmtSz, &fmt);
                    fprintf(stderr, "[aggregate]   input stream id=%u sr=%.0f ch=%u bits=%u fmt=0x%x\n",
                            (unsigned)s, fmt.mSampleRate, (unsigned)fmt.mChannelsPerFrame,
                            (unsigned)fmt.mBitsPerChannel, (unsigned)fmt.mFormatFlags);
                }
            }
        } else {
            fprintf(stderr, "[aggregate] couldn't get input streams\n");
        }
        fflush(stderr);
    }

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

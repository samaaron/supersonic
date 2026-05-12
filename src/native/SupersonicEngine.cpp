/*
 * SupersonicEngine.cpp
 */
#include "SupersonicEngine.h"
#include "AggregateDeviceHelper.h"
#include "DevicePolicy.h"
#include "src/audio_processor.h"
#include "audio_config.h"
#include "src/shared_memory.h"
#include "osc/OscReceivedElements.h"
#include "RingBufferWriter.h"
#include "scsynth/server/SC_Prototypes.h"  // zfree
#include <juce_audio_formats/juce_audio_formats.h>
#include "FuzzyMatch.h"
#include <chrono>
#include <cstring>
#include <thread>
#ifdef __linux__
#include <dlfcn.h>
#endif
#ifdef __APPLE__
#include <CoreAudio/CoreAudio.h>
#include "MicPermission.h"
#endif

extern "C" {
    // Global used by init_memory() to pass external shared memory to World_New.
    // Declared extern "C" because init_memory() references it from an extern "C" block.
    void* g_external_shared_memory = nullptr;

    // Override for the metrics pointer. When non-null, init_memory() uses
    // this address for the global metrics pointer instead of the in-band slot
    // inside ring_buffer_storage. Set by the engine after creating its public
    // POSIX shm segment so Sonic Pi can observe metrics directly.
    extern PerformanceMetrics* g_external_metrics;

    // Same pattern for the audio-buffer slot array: points into the
    // POSIX shm segment so external readers (Sonic Pi session recorder)
    // see the master mix and any AudioOut2-driven stems.
    extern shm_audio_buffer* g_external_audio_buffers;

    void destroy_world();
    void rebuild_world(double sample_rate);
}

namespace {
// JUCE appends " (N)" suffixes to disambiguate duplicate CoreAudio device
// names (e.g. two "USB Audio Device" instances become "USB Audio Device"
// and "USB Audio Device (2)"). Returns true if `full` is exactly `base`,
// or begins with `base` followed by a space — i.e. base matches the
// real device name inside the JUCE-disambiguated form.
bool deviceNameMatches(const std::string& full, const std::string& base) {
    if (full == base) return true;
    return full.size() > base.size()
        && full.compare(0, base.size(), base) == 0
        && full[base.size()] == ' ';
}

#ifdef __linux__
// Silence libjack's stderr chatter on boxes where no jackd / pipewire-
// jack server is running. With JUCE_JACK=1 enabled, JUCE's
// JackAudioIODeviceType calls jack_client_open() during scanForDevices;
// libjack writes a two-line "connect(2) ... failed / attempt to connect
// to server failed" pair for every failed attempt. Routing that through
// our noop callback eliminates the spam without affecting the scan's
// outcome (JUCE still sees the open-failed return and correctly reports
// zero devices for the JACK type).
void silentJackLog(const char*) {}

void silenceJackLogsIfPossible() {
    // Only relevant after libjack has been loaded. JUCE dlopens it
    // lazily inside JackAudioIODeviceType's ctor; calling this once
    // after AudioDeviceManager construction catches that first scan.
    // If libjack isn't loadable at all (no package installed), dlopen
    // fails and we skip quietly.
    void* handle = dlopen("libjack.so.0", RTLD_LAZY | RTLD_NOLOAD);
    if (!handle) handle = dlopen("libjack.so.0", RTLD_LAZY);
    if (!handle) return;
    using set_fn = void (*)(void (*)(const char*));
    if (auto set_err  = (set_fn)dlsym(handle, "jack_set_error_function"))
        set_err(silentJackLog);
    if (auto set_info = (set_fn)dlsym(handle, "jack_set_info_function"))
        set_info(silentJackLog);
    // Don't dlclose — JUCE needs libjack resident for its own dlsym path.
}
#endif
}

SupersonicEngine::SupersonicEngine() = default;

void SupersonicEngine::recordSwapPreferences(const std::string& deviceName,
                                             const std::string& inputDeviceName,
                                             double sampleRate) {
    if (!deviceName.empty() && sampleRate > 0) {
        // Cap to avoid unbounded growth across long sessions of hot-plug
        // cycling (USB dock / AirPlay / Bluetooth churn can accumulate
        // device-name variants indefinitely). 32 is well above any realistic
        // device set a user juggles in one session — if we hit it, the
        // entries we forget get re-probed next time the device is chosen.
        static constexpr size_t kMaxDeviceRateMemoryEntries = 32;
        if (mDeviceRateMemory.size() >= kMaxDeviceRateMemoryEntries
            && mDeviceRateMemory.find(deviceName) == mDeviceRateMemory.end())
            mDeviceRateMemory.clear();
        mDeviceRateMemory[deviceName] = static_cast<int>(sampleRate);
    }

    // Track the user's long-lived preferred device for hot-plug re-attach.
    // An explicit deviceName means the caller picked it (GUI switch, OSC,
    // setDeviceMode); remember it even across cycles where the device
    // disappears. inputDeviceName == "__none__" means "disable inputs"
    // (user intent to not have any input), so clear the preferred input;
    // any other explicit input means that's the desired sub-device.
    if (!deviceName.empty())
        mPreferredOutputDevice = deviceName;
    if (inputDeviceName == "__none__")
        mPreferredInputDevice.clear();
    else if (!inputDeviceName.empty())
        mPreferredInputDevice = inputDeviceName;

    // Per-driver memory: record the just-opened device under the
    // driver JUCE actually opened on. switchDriver reads this to
    // delegate driver-only picks to an explicit-name switchDevice,
    // closing the alphabetical-first-auto-pick hazard. Use JUCE's
    // type directly — currentDriver() hides the intent fallback.
    if (!deviceName.empty() && mDeviceManager) {
        if (auto* dev = mDeviceManager->getCurrentAudioDevice()) {
            std::string drv = dev->getTypeName().toStdString();
            if (!drv.empty())
                mPreferredDeviceByDriver[drv] = deviceName;
            mIntendedDriver.clear();
        }
    }
}

std::string SupersonicEngine::refuseUnknownDeviceName(
        const std::string& deviceName,
        const std::string& inputDeviceName) {
    if (!mDeviceManager) return {};
    if (deviceName.empty() && inputDeviceName.empty()) return {};
    std::vector<std::string> visibleNames;
    for (auto& d : listDevices(false)) visibleNames.push_back(d.name);
    return sonicpi::device::validateSwapDeviceNames(
        deviceName, inputDeviceName, visibleNames);
}

std::string SupersonicEngine::refuseWirelessMicAddition(
        const std::string& deviceName,
        const std::string& inputDeviceName) {
#ifdef __APPLE__
    // Only applies to "add mic while keeping current output" swaps.
    if (!deviceName.empty()) return {};
    if (inputDeviceName.empty() || inputDeviceName == "__none__") return {};
    if (!mDeviceManager) return {};
    auto* cur = mDeviceManager->getCurrentAudioDevice();
    if (!cur) return {};
    std::string curOut = mRealOutputDeviceName.empty()
        ? cur->getName().toStdString()
        : mRealOutputDeviceName;
    for (auto& dev : listDevices(false)) {
        if (deviceNameMatches(dev.name, curOut) && dev.isWirelessTransport()) {
            std::string err = "can't add input '" + inputDeviceName
                            + "' — current output '" + curOut
                            + "' is wireless and can't be aggregated with a mic";
            fprintf(stderr, "[switchDevice] %s\n", err.c_str());
            fflush(stderr);
            return err;
        }
    }
#else
    (void)deviceName; (void)inputDeviceName;
#endif
    return {};
}

int SupersonicEngine::probeDeviceChannelCount(const std::string& name,
                                              bool isInput) {
    if (name.empty() || name == "__none__") return -1;
    if (!mDeviceManager) return -1;
    auto& types = mDeviceManager->getAvailableDeviceTypes();
    for (auto* type : types) {
        for (auto& n : type->getDeviceNames(isInput)) {
            if (!deviceNameMatches(n.toStdString(), name)) continue;
            auto outArg = isInput ? juce::String() : n;
            auto inArg  = isInput ? n : juce::String();
            std::unique_ptr<juce::AudioIODevice> probe(
                type->createDevice(outArg, inArg));
            if (probe)
                return isInput ? probe->getInputChannelNames().size()
                               : probe->getOutputChannelNames().size();
        }
    }
    return -1;
}

std::vector<double> SupersonicEngine::probeDeviceSampleRates(
        const std::string& name, bool isInput) {
    std::vector<double> result;
    if (name.empty() || !mDeviceManager) return result;
    auto& types = mDeviceManager->getAvailableDeviceTypes();
    for (auto* type : types) {
        type->scanForDevices();
        for (auto& n : type->getDeviceNames(isInput)) {
            if (!deviceNameMatches(n.toStdString(), name)) continue;
            auto outArg = isInput ? juce::String() : n;
            auto inArg  = isInput ? n : juce::String();
            std::unique_ptr<juce::AudioIODevice> probe(
                type->createDevice(outArg, inArg));
            if (!probe) return result;
            for (auto r : probe->getAvailableSampleRates())
                result.push_back(r);
            return result;
        }
    }
    return result;
}

void SupersonicEngine::probeAndAdjustForTargetRate(const std::string& name,
                                                   bool isInput,
                                                   double& sampleRate,
                                                   double currentRate) {
    if (sampleRate > 0 || currentRate <= 0) return;
    auto rates = probeDeviceSampleRates(name, isInput);
    if (rates.empty()) return;

    for (auto r : rates)
        if (static_cast<int>(r) == static_cast<int>(currentRate))
            return;  // supported — keep current rate

    double nearest = rates[0];
    for (auto r : rates)
        if (std::abs(r - currentRate) < std::abs(nearest - currentRate))
            nearest = r;
    sampleRate = nearest;
    fprintf(stderr,
        "[audio-device] current rate %.0f not supported "
        "by %s, will use %.0f (cold swap)\n",
        currentRate, name.c_str(), sampleRate);
}

void SupersonicEngine::clampAggregateBufferIfNeeded(int& bufferSize) {
#ifdef __APPLE__
    const bool active = AggregateDeviceHelper::exists()
                     && AggregateDeviceHelper::driftCompensationEnabled();
    const int clamped = sonicpi::device::clampBufferForDriftComp(bufferSize, active);
    if (clamped != bufferSize) {
        fprintf(stderr, "[audio-device] clamping aggregate buffer "
                "%d -> %d (drift-comp minimum)\n",
                bufferSize, clamped);
        fflush(stderr);
        bufferSize = clamped;
        mCurrentConfig.bufferSize = clamped;
    }
#endif
}

void SupersonicEngine::setEngineState(EngineState state, const std::string& reason) {
    EngineState prev = mEngineState.exchange(state);
    if (prev == state) return;  // no transition

    const char* stateStr = engineStateToString(state);
    fprintf(stderr, "[supersonic] state: %s -> %s (%s)\n",
            engineStateToString(prev), stateStr,
            reason.empty() ? "-" : reason.c_str());
    fflush(stderr);

    mUdpServer.sendStateChange(stateStr, reason.c_str());

    // /supersonic/setup fires ONLY when the World was actually rebuilt
    // (cold swap). Spider receives this and runs cold_swap_reinit! which
    // resets all node IDs. Sending it on swap-failed-rollback or hot swaps
    // (where old nodes still exist) would cause duplicate node ID errors.
    if (state == EngineState::Running && mWorldRebuilt) {
        mWorldRebuilt = false;
        // Bump before emit so the wire value is the post-rebuild
        // generation (mSetupGeneration starts at 1; first cold swap = 2).
        uint32_t gen = mSetupGeneration.fetch_add(1) + 1;
        auto* dev = mDeviceManager ? mDeviceManager->getCurrentAudioDevice() : nullptr;
        int sr  = dev ? static_cast<int>(dev->getCurrentSampleRate()) : mCurrentConfig.sampleRate;
        int buf = dev ? dev->getCurrentBufferSizeSamples() : mCurrentConfig.bufferSize;
        mUdpServer.sendSetup(sr, buf, gen);
    }
}

SupersonicEngine::~SupersonicEngine() {
    shutdown();
}


void SupersonicEngine::init(const Config& cfg) {
    if (mRunning.load()) return;
    setEngineState(EngineState::Booting, "init");

    mHeadless = cfg.headless;
    mCurrentConfig = cfg;
    // mBootInputChannels may be kAutoChannelCount (-1) here — resolved to a
    // concrete count when enableInputChannels() is eventually called.
    mBootInputChannels = cfg.numInputChannels;

    // Seed the pre-wireless rate from the boot config so that a user who
    // boots directly into a wireless default (e.g. AirPlay because no
    // other output is present) still has a rate to restore when they
    // later switch to a hardware device. Without this seed, the first
    // non-wireless switch after a boot-on-wireless has mPreWirelessRate=0
    // and the AirPlay-negotiated 44.1 kHz sticks onto the new device.
    mPreWirelessRate = cfg.sampleRate;

    // Seed the preferred-output-device name from -H / sound_card_name so
    // that a boot with the device absent (USB interface unplugged, etc.)
    // still remembers the user's intent — the device-list-change listener
    // will auto-switch to it when it reappears. "__system__" sentinel means
    // "explicitly follow macOS default"; leave the preferred empty.
    if (!cfg.hardwareDevice.empty() && cfg.hardwareDevice != "__system__")
        mPreferredOutputDevice = cfg.hardwareDevice;

    // Map -1 (auto/max) to a large request count. JUCE/CoreAudio will clamp
    // the bitmask to the actual device channel count, so asking for more
    // than exists is safe; the callback's active-channels query later reads
    // the real count back.
    auto resolveReq = [](int n) -> int {
        return n < 0 ? kRequestMaxChannels : n;
    };
    int reqIn  = resolveReq(cfg.numInputChannels);
    int reqOut = resolveReq(cfg.numOutputChannels);

    // -- Wire callbacks ---------------------------------------------------
    // onReply/onDebug should be set before init() — worker threads
    // read them via captured `this` pointer without synchronisation.
    // Setting them after init() is a data race.
    mReplyReader.onReply = [this](const uint8_t* d, uint32_t s) {
        // Intercept /supersonic/buffer/freed — free the buffer memory
        // and don't forward this internal message to external listeners.
        if (interceptBufferFreed(d, s)) return;

        // Send to all registered notify targets (registered via /supersonic/notify).
        // This replaces the single-target sendReply() so that all clients
        // (Spider, GUI, etc.) receive scsynth replies reliably.
        if (mDeviceManager) mUdpServer.broadcastToTargets(d, s);
        if (onReply) onReply(d, s);
    };
    mDebugReader.onDebug = [this](const std::string& s) {
        if (onDebug) onDebug(s);
    };

    if (!cfg.headless) {
#ifdef __APPLE__
        // Tell CoreAudio to deliver HAL property notifications on its own
        // internal thread instead of the main CFRunLoop.  Required because
        // we don't run a Cocoa event loop.
        {
            CFRunLoopRef nullRunLoop = NULL;
            AudioObjectPropertyAddress prop = {
                kAudioHardwarePropertyRunLoop,
                kAudioObjectPropertyScopeGlobal,
                kAudioObjectPropertyElementMain
            };
            AudioObjectSetPropertyData(kAudioObjectSystemObject, &prop,
                                       0, NULL, sizeof(CFRunLoopRef), &nullRunLoop);
        }

        // Clean up any orphaned aggregate device from a previous crash
        // before initialising the audio device manager.
        AggregateDeviceHelper::cleanupOrphaned();
#endif
        mDeviceManager = std::make_unique<juce::AudioDeviceManager>();

#ifdef __linux__
        silenceJackLogsIfPossible();
#endif

        // -- Select audio driver --------------------------------------------------
        {
            auto& types = mDeviceManager->getAvailableDeviceTypes();
            fprintf(stderr, "  Available drivers:");
            for (auto* t : types)
                fprintf(stderr, " [%s]", t->getTypeName().toRawUTF8());
            fprintf(stderr, "\n");
            fflush(stderr);
        }

#ifdef _WIN32
        // Default to DirectSound on Windows.  WASAPI shared mode batches
        // event callbacks in ~10ms bursts, causing audible crackles even
        // though our processing completes well within budget.  DirectSound
        // uses polling-based buffer management that avoids this entirely.
        {
            auto& types = mDeviceManager->getAvailableDeviceTypes();
            for (auto* t : types) {
                if (t->getTypeName() == "DirectSound") {
                    mDeviceManager->setCurrentAudioDeviceType("DirectSound", true);
                    break;
                }
            }
        }
#endif

        // -H "__system__" is the GUI sentinel for "follow macOS default".
        // Skip fuzzy-match and go straight to initialiseWithDefaultDevices.
        juce::String initError;
        bool openedByHardwareFlag = false;

        if (!cfg.hardwareDevice.empty() && cfg.hardwareDevice != "__system__") {
            struct DevEntry { std::string combined, typeName, devName; };
            std::vector<DevEntry> entries;
            std::vector<std::string> combinedNames;

            auto& types = mDeviceManager->getAvailableDeviceTypes();
            for (auto* type : types) {
                type->scanForDevices();
                for (auto& name : type->getDeviceNames(false)) {
                    DevEntry e;
                    e.typeName = type->getTypeName().toStdString();
                    e.devName  = name.toStdString();
                    e.combined = e.typeName + " : " + e.devName;
                    entries.push_back(e);
                    combinedNames.push_back(e.combined);
                }
            }

#ifdef __APPLE__
            // Filter wireless (AirPlay/Bluetooth) from fuzzy-match candidates.
            // These can't be opened via HAL — the route is only warmed up when
            // the device becomes the macOS system default via System Settings.
            {
                auto allDevs = listDevices();
                std::set<std::string> wirelessNames;
                for (auto& d : allDevs)
                    if (d.isWirelessTransport()) wirelessNames.insert(d.name);

                entries.erase(std::remove_if(entries.begin(), entries.end(),
                    [&wirelessNames](const DevEntry& e) {
                        for (auto& w : wirelessNames)
                            if (deviceNameMatches(e.devName, w)) return true;
                        return false;
                    }), entries.end());
                combinedNames.clear();
                for (auto& e : entries) combinedNames.push_back(e.combined);
            }
#endif

            std::string matched = fuzzyMatch(cfg.hardwareDevice, combinedNames);
            if (matched.empty()) {
                fprintf(stderr,
                        "[audio-device] WARNING: requested output device '%s' not found. "
                        "Falling back to system default. Available outputs:\n",
                        cfg.hardwareDevice.c_str());
                for (auto& e : entries)
                    fprintf(stderr, "    %s\n", e.combined.c_str());
            } else {
                for (auto& e : entries) {
                    if (e.combined != matched) continue;

                    mDeviceManager->setCurrentAudioDeviceType(
                        juce::String(e.typeName), true);

                    juce::AudioDeviceManager::AudioDeviceSetup setup;
                    setup.outputDeviceName = juce::String(e.devName);
                    setup.inputDeviceName  = juce::String();
                    setup.useDefaultOutputChannels = true;
                    setup.useDefaultInputChannels  = false;
                    if (cfg.sampleRate > 0) setup.sampleRate = cfg.sampleRate;
                    if (cfg.bufferSize > 0) setup.bufferSize = cfg.bufferSize;

                    initError = mDeviceManager->initialise(
                        reqIn, reqOut,
                        nullptr, false, juce::String(), &setup);

                    if (initError.isNotEmpty()) {
                        fprintf(stderr, "[audio-device] -H '%s' matched '%s' but failed: %s\n",
                                cfg.hardwareDevice.c_str(), e.combined.c_str(),
                                initError.toRawUTF8());
                    } else {
                        fprintf(stderr, "  -H '%s' -> %s\n",
                                cfg.hardwareDevice.c_str(), e.combined.c_str());
                        mDeviceMode = e.devName;
                        openedByHardwareFlag = true;
                    }
                    break;
                }
            }
        }

        if (!openedByHardwareFlag) {
#ifdef __APPLE__
            // On macOS, boot output-only then create an Aggregate Device.
            // JUCE's AudioIODeviceCombiner (used when input and output are
            // different hardware devices) is unreliable at small buffer sizes.
            //
            // Pre-check: if the macOS system default is wireless (AirPlay
            // / Bluetooth), do NOT open it at boot. Opening wireless then
            // transitioning to a non-wireless device for the aggregate
            // triggers a ~15 s CoreAudio IOProc halt — Sonic Pi's boot
            // handshake times out in that window and scopes never start.
            // Pick a non-wireless fallback up front.
            std::string bootFallback;
            {
                AudioDeviceID defaultID = kAudioObjectUnknown;
                AudioObjectPropertyAddress addr = {
                    kAudioHardwarePropertyDefaultOutputDevice,
                    kAudioObjectPropertyScopeGlobal,
                    kAudioObjectPropertyElementMain
                };
                UInt32 sz = sizeof(defaultID);
                if (AudioObjectGetPropertyData(kAudioObjectSystemObject,
                        &addr, 0, nullptr, &sz, &defaultID) == noErr
                    && defaultID != kAudioObjectUnknown) {
                    // Default name
                    CFStringRef cfName = nullptr;
                    UInt32 nsz = sizeof(cfName);
                    AudioObjectPropertyAddress nameAddr = {
                        kAudioDevicePropertyDeviceNameCFString,
                        kAudioObjectPropertyScopeGlobal,
                        kAudioObjectPropertyElementMain
                    };
                    std::string defaultName;
                    if (AudioObjectGetPropertyData(defaultID, &nameAddr,
                            0, nullptr, &nsz, &cfName) == noErr && cfName) {
                        char buf[256];
                        CFStringGetCString(cfName, buf, sizeof(buf),
                                           kCFStringEncodingUTF8);
                        CFRelease(cfName);
                        defaultName = buf;
                    }
                    // Transport type
                    AudioObjectPropertyAddress tAddr = {
                        kAudioDevicePropertyTransportType,
                        kAudioObjectPropertyScopeGlobal,
                        kAudioObjectPropertyElementMain
                    };
                    UInt32 tType = 0, tSize = sizeof(tType);
                    bool defaultIsWireless = false;
                    if (AudioObjectGetPropertyData(defaultID, &tAddr, 0,
                            nullptr, &tSize, &tType) == noErr) {
                        defaultIsWireless = CoreAudioTransport::isWireless(tType);
                    }
                    if (defaultIsWireless && !defaultName.empty()) {
                        std::vector<std::string> names;
                        std::vector<bool> wirelessFlags;
                        for (auto& d : listDevices()) {
                            names.push_back(d.name);
                            wirelessFlags.push_back(d.isWirelessTransport());
                        }
                        bootFallback = sonicpi::device::selectBootOutputDevice(
                            defaultName, defaultIsWireless, names, wirelessFlags);
                        if (!bootFallback.empty()) {
                            fprintf(stderr, "[audio-device] boot: default '%s' "
                                    "is wireless; using non-wireless fallback '%s'\n",
                                    defaultName.c_str(), bootFallback.c_str());
                            fflush(stderr);
                        } else {
                            fprintf(stderr, "[audio-device] boot: default '%s' "
                                    "is wireless and no non-wireless fallback "
                                    "available — opening wireless default may "
                                    "silence audio for ~15 s during boot handshake\n",
                                    defaultName.c_str());
                            fflush(stderr);
                        }
                    }
                }
            }

            mLastSelfTriggeredChange = std::chrono::steady_clock::now();
            if (!bootFallback.empty()) {
                juce::AudioDeviceManager::AudioDeviceSetup setup;
                setup.outputDeviceName = juce::String(bootFallback);
                setup.useDefaultOutputChannels = true;
                initError = mDeviceManager->initialise(
                    0, reqOut, nullptr, false, juce::String(), &setup);
                if (initError.isEmpty()) mDeviceMode = bootFallback;
            } else {
                initError = mDeviceManager->initialiseWithDefaultDevices(
                    0, reqOut);
            }
            if (initError.isNotEmpty()) {
                fprintf(stderr, "[audio-device] init with 0 in / %d out failed: %s\n",
                        reqOut, initError.toRawUTF8());
                mLastSelfTriggeredChange = std::chrono::steady_clock::now();
                initError = mDeviceManager->initialiseWithDefaultDevices(0, 0);
            }
            if (initError.isEmpty() && cfg.numInputChannels != 0) {
                auto* dev = mDeviceManager->getCurrentAudioDevice();
                if (dev) {
                    std::string outName = dev->getName().toStdString();
                    std::string inName;
                    AudioObjectPropertyAddress pa = {
                        kAudioHardwarePropertyDefaultInputDevice,
                        kAudioObjectPropertyScopeGlobal,
                        kAudioObjectPropertyElementMain
                    };
                    AudioDeviceID inputDevId = 0;
                    UInt32 sz = sizeof(inputDevId);
                    if (AudioObjectGetPropertyData(kAudioObjectSystemObject,
                            &pa, 0, nullptr, &sz, &inputDevId) == noErr
                            && inputDevId != 0) {
                        CFStringRef cfName = nullptr;
                        UInt32 nsz = sizeof(cfName);
                        AudioObjectPropertyAddress nameAddr = {
                            kAudioDevicePropertyDeviceNameCFString,
                            kAudioObjectPropertyScopeGlobal,
                            kAudioObjectPropertyElementMain
                        };
                        if (AudioObjectGetPropertyData(inputDevId, &nameAddr,
                                0, nullptr, &nsz, &cfName) == noErr && cfName) {
                            char buf[256];
                            CFStringGetCString(cfName, buf, sizeof(buf),
                                               kCFStringEncodingUTF8);
                            CFRelease(cfName);
                            inName = buf;
                        }
                    }
                    // Skip aggregate for wireless (Bluetooth/AirPlay) or virtual
                    // (Loopback/Blackhole) outputs — same rule as switchDevice.
                    // Boot with output-only instead so we don't crash JUCE's
                    // Combiner fallback when sample-rate negotiation fails.
                    bool outputSuitable = true;
                    if (!inName.empty() && inName != outName) {
                        for (auto& d : listDevices()) {
                            if (d.name == outName && !d.isSuitableForAggregate()) {
                                outputSuitable = false;
                                fprintf(stderr, "[audio-device] boot: skipping aggregate — "
                                        "'%s' is %s; input disabled\n",
                                        outName.c_str(),
                                        d.isVirtualTransport() ? "virtual" : "wireless");
                                fflush(stderr);
                                break;
                            }
                        }
                    }
                    if (!inName.empty() && inName != outName && outputSuitable) {
                        mLastSelfTriggeredChange = std::chrono::steady_clock::now();
                        auto aggName = AggregateDeviceHelper::createOrUpdate(
                            outName, inName,
                            static_cast<double>(mCurrentConfig.sampleRate));
                        if (!aggName.empty()) {
                            juce::Thread::sleep(300);
                            if (auto* dt = mDeviceManager->getCurrentDeviceTypeObject())
                                dt->scanForDevices();
                            mRealOutputDeviceName = outName;
                            mRealInputDeviceName  = inName;
                            mLastInputDeviceName  = inName;
                            juce::AudioDeviceManager::AudioDeviceSetup setup;
                            mDeviceManager->getAudioDeviceSetup(setup);
                            setup.outputDeviceName = juce::String(aggName);
                            setup.inputDeviceName  = juce::String(aggName);
                            setup.useDefaultInputChannels = false;
                            clampAggregateBufferIfNeeded(setup.bufferSize);
                            juce::BigInteger inputBits;
                            inputBits.setRange(0, reqIn, true);
                            setup.inputChannels = inputBits;
                            mLastSelfTriggeredChange = std::chrono::steady_clock::now();
                            auto aggErr = mDeviceManager->setAudioDeviceSetup(setup, true);
                            if (aggErr.isNotEmpty()) {
                                fprintf(stderr, "[audio-device] aggregate setup failed: %s — "
                                        "falling back to Combiner\n", aggErr.toRawUTF8());
                                AggregateDeviceHelper::destroy();
                                mRealOutputDeviceName.clear();
                                mRealInputDeviceName.clear();
                                // Fall back to Combiner
                                mLastSelfTriggeredChange = std::chrono::steady_clock::now();
                                mDeviceManager->initialiseWithDefaultDevices(
                                    reqIn, reqOut);
                            } else {
                                fprintf(stderr, "[audio-device] booted with aggregate: "
                                        "out='%s' in='%s'\n", outName.c_str(), inName.c_str());
                                // Suppress CFRunLoop until Spider has finished
                                // cold_swap_reinit — queued audioDeviceListChanged
                                // messages would trigger a second cold swap and
                                // crash ScopeOut2 during the rebuild.
                                mSuppressRunLoop.store(true);
                            }
                        }
                    } else if (inName == outName) {
                        mLastSelfTriggeredChange = std::chrono::steady_clock::now();
                        initError = mDeviceManager->initialiseWithDefaultDevices(
                            reqIn, reqOut);
                    }
                }
            }
#else
            mLastSelfTriggeredChange = std::chrono::steady_clock::now();
            initError = mDeviceManager->initialiseWithDefaultDevices(
                reqIn, reqOut);
            if (initError.isNotEmpty()) {
                fprintf(stderr, "[audio-device] init with %d in / %d out failed: %s\n",
                        reqIn, reqOut,
                        initError.toRawUTF8());
                mLastSelfTriggeredChange = std::chrono::steady_clock::now();
                initError = mDeviceManager->initialiseWithDefaultDevices(0, 2);
            }
            if (initError.isNotEmpty()) {
                fprintf(stderr, "[audio-device] init with 0 in / 2 out failed: %s\n",
                        initError.toRawUTF8());
                mLastSelfTriggeredChange = std::chrono::steady_clock::now();
                initError = mDeviceManager->initialiseWithDefaultDevices(0, 0);
            }
#endif
            if (initError.isNotEmpty()) {
                fprintf(stderr, "[audio-device] all init attempts failed: %s\n",
                        initError.toRawUTF8());
            }
        }

        // Negotiate sample rate and buffer size.
        // scsynth processes in fixed 128-sample blocks, so a hardware buffer
        // that is a multiple of 128 avoids prefetch overhead and eliminates
        // NTP timing discontinuities at callback boundaries.
        if (auto* dev = mDeviceManager->getCurrentAudioDevice()) {
            juce::AudioDeviceManager::AudioDeviceSetup setup;
            mDeviceManager->getAudioDeviceSetup(setup);
            bool changed = false;

            // Clamp sample rate to what the device actually supports
            if (static_cast<int>(setup.sampleRate) != cfg.sampleRate) {
                auto rates = dev->getAvailableSampleRates();
                bool supported = false;
                for (auto r : rates) {
                    if (static_cast<int>(r) == cfg.sampleRate) {
                        supported = true;
                        break;
                    }
                }
                if (supported) {
                    setup.sampleRate = cfg.sampleRate;
                    changed = true;
                } else {
                    fprintf(stderr, "[audio-device] requested sr %d not supported, "
                            "keeping %.0f\n", cfg.sampleRate, setup.sampleRate);
                }
            }

            if (cfg.bufferSize > 0) {
                // Honour the user's -z / -Z / TOML buffer size, clamped up
                // only when we're running a drift-comp aggregate (a stale
                // block_size=64 in the TOML would otherwise boot into a
                // drift storm).
                int wantedBuf = cfg.bufferSize;
                clampAggregateBufferIfNeeded(wantedBuf);
                setup.bufferSize = wantedBuf;
                changed = true;
            } else if (dev->getTypeName() != "DirectSound") {
                // Auto: pick the smallest available buffer that is at least
                // 128 samples. We no longer require a multiple of 128 —
                // scsynth's block size matches the HW buffer on native, so
                // any size works. Buffers below 128 mostly add callback
                // overhead without a latency win.
                constexpr int kMinBuf = 128;
                auto sizes = dev->getAvailableBufferSizes();
                int best = 0;
                for (auto s : sizes) {
                    if (s >= kMinBuf && s <= sonicpi::kMaxBlockSize) {
                        best = s;
                        break;  // sizes are sorted ascending
                    }
                }
                if (best > 0 && best != dev->getCurrentBufferSizeSamples()) {
                    setup.bufferSize = best;
                    changed = true;
                }
            }

            if (changed) {
                juce::String setupErr = mDeviceManager->setAudioDeviceSetup(setup, true);
                if (setupErr.isNotEmpty()) {
                    fprintf(stderr, "[audio-device] setAudioDeviceSetup error: %s\n",
                            setupErr.toRawUTF8());
                    // Recover: reinitialise with defaults rather than leaving device broken
                    fprintf(stderr, "[audio-device] recovering with device defaults\n");
                    mDeviceManager->initialiseWithDefaultDevices(
                        reqIn, reqOut);
                }
            }
        }

        // Read what the device actually settled on and override config to match
        if (auto* dev = mDeviceManager->getCurrentAudioDevice()) {
            double sr   = dev->getCurrentSampleRate();
            int    bs   = dev->getCurrentBufferSizeSamples();
            int    nOut = dev->getOutputChannelNames().size();
            int    nIn  = dev->getInputChannelNames().size();
            double latIn  = dev->getInputLatencyInSamples()  / sr;
            double latOut = dev->getOutputLatencyInSamples() / sr;

            // Use actual device parameters for World initialization
            mCurrentConfig.sampleRate       = static_cast<int>(sr);
            mCurrentConfig.numOutputChannels = nOut;
            mCurrentConfig.numInputChannels  = nIn;

        } else {
            fprintf(stderr, "[supersonic] warning: no audio device available\n");
        }
        fflush(stderr);
    }

    // Resolve any remaining auto-max sentinels to concrete counts before the
    // World is initialised. This covers headless mode and any path where the
    // device failed to open (readback block above didn't run).
    if (mCurrentConfig.numOutputChannels < 0) mCurrentConfig.numOutputChannels = 2;
    if (mCurrentConfig.numInputChannels  < 0) mCurrentConfig.numInputChannels  = 0;

    // -- Create shared memory (owned by engine, survives cold swaps) --------
    if (cfg.udpPort > 0) {
        server_shared_memory_creator::cleanup(cfg.udpPort);
        try {
            mShmemCreator = std::make_unique<server_shared_memory_creator>(
                cfg.udpPort, cfg.numControlBusChannels);
            // Tell init_memory()/World_New to reuse this instead of creating its own
            g_external_shared_memory = mShmemCreator.get();
            // Redirect the metrics pointer into the public segment so external
            // observers (Sonic Pi) can read it directly via shm.
            g_external_metrics = mShmemCreator->get_metrics();
            // Same for the audio-buffer slot array. Without this override
            // the audio thread writes into ring_buffer_storage (process-
            // local) and the GUI reader sees only zeros.
            g_external_audio_buffers = mShmemCreator->get_audio_buffers();
        } catch (const std::exception& e) {
            fprintf(stderr, "[supersonic] shared memory creation failed: %s\n", e.what());
            fflush(stderr);
            mShmemCreator.reset();
            g_external_metrics = nullptr;
            g_external_audio_buffers = nullptr;
        }
    } else {
        g_external_metrics = nullptr;
        g_external_audio_buffers = nullptr;
    }

    // -- Initialise scsynth World ------------------------------------------
    // scsynth's block size is fixed at kDefaultBlockSize (128) regardless
    // of the hardware callback buffer. JuceAudioCallback's accumulator +
    // prefetch decoupling handles HW-buffer ≠ scsynth-block, so the
    // smaller block buys us a finer OSC-bundle scheduling grid (~3 ms at
    // 48 kHz) without paying any latency cost on the audio thread.
    int chosenBufLen = sonicpi::kDefaultBlockSize;
    fprintf(stderr, "[supersonic] scsynth block size = %d samples\n", chosenBufLen);
    fflush(stderr);

    // Use actual device sample rate and channel counts (may differ from requested)
    mAudioCallback.initialiseWorld(
        ring_buffer_storage,
        mCurrentConfig.sampleRate,
        mCurrentConfig.numOutputChannels,
        mCurrentConfig.numInputChannels,
        cfg.numBuffers,
        cfg.maxNodes,
        cfg.maxGraphDefs,
        cfg.maxWireBufs,
        cfg.numAudioBusChannels,
        cfg.numControlBusChannels,
        cfg.realTimeMemorySize,
        cfg.numRGens,
        cfg.udpPort,  // sharedMemoryID — creates boost shm named "SuperColliderServer_<port>"
        chosenBufLen
    );

    // Derive pointers into ring_buffer_storage for worker threads
    uint8_t* base = ring_buffer_storage;
    ControlPointers*    ctrl = reinterpret_cast<ControlPointers*>(base + CONTROL_START);
    // Workers write metrics into the public POSIX shm segment when one was
    // created, so external observers (Sonic Pi) read the same struct without
    // OSC roundtrips. Falls back to the in-band slot for headless tests.
    mMetrics                 = mShmemCreator
                             ? mShmemCreator->get_metrics()
                             : reinterpret_cast<PerformanceMetrics*>(base + METRICS_START);
    PerformanceMetrics* met  = mMetrics;

    // -- Prescheduler -------------------------------------------------------
    mPrescheduler.initialise(
        base + IN_BUFFER_START,
        IN_BUFFER_SIZE,
        &ctrl->in_head,
        &ctrl->in_tail,
        &ctrl->in_sequence,
        &ctrl->in_write_lock,
        met,
        cfg.preschedulerLookaheadS
    );

    // -- ReplyReader --------------------------------------------------------
    mReplyReader.initialise(
        base + OUT_BUFFER_START,
        OUT_BUFFER_SIZE,
        &ctrl->out_head,
        &ctrl->out_tail,
        met,
        &mAudioCallback.processCount
    );

    // -- DebugReader --------------------------------------------------------
    mDebugReader.initialise(
        base + DEBUG_BUFFER_START,
        DEBUG_BUFFER_SIZE,
        &ctrl->debug_head,
        &ctrl->debug_tail,
        met,
        &mAudioCallback.processCount
    );

    // -- UDP OSC Server -----------------------------------------------------
    mUdpServer.initialise(
        cfg.udpPort,
        &mPrescheduler,
        base + IN_BUFFER_START,
        IN_BUFFER_SIZE,
        &ctrl->in_head,
        &ctrl->in_tail,
        &ctrl->in_sequence,
        &ctrl->in_write_lock,
        met,
        cfg.preschedulerLookaheadS,
        cfg.bindAddress
    );

    // Pass engine reference for /supersonic/* command handling
    mUdpServer.setEngine(this);

    // Test hook for issue #3526: close the device before the source
    // decision so startAudioSource() sees no current device and falls
    // back to the headless driver.
    if (testForceNoCurrentDeviceAfterInit && mDeviceManager) {
        fprintf(stderr,
                "[supersonic] testForceNoCurrentDeviceAfterInit: closing device "
                "to exercise headless fallback path\n");
        fflush(stderr);
        mDeviceManager->closeAudioDevice();
    }

    // -- SampleLoader + audio callback wiring ------------------------------
    // Wire to audio callback so installPendingBuffers() runs on the audio
    // thread. Done before startAudioSource() so the audio thread sees a
    // fully-configured callback the first time it fires.
    mSampleLoader.initialise();
    mAudioCallback.setSampleLoader(&mSampleLoader);
    mAudioCallback.onWake = [this]() { purge(); };

    // -- Start worker threads ----------------------------------------------
    // Workers must be running before the audio source starts, otherwise
    // OUT/DEBUG ring buffers can back up during the audio thread's first
    // few hundred ticks (~ms) before any reader is draining them.
    mPrescheduler.startThread(juce::Thread::Priority::normal);
    mReplyReader.startThread(juce::Thread::Priority::normal);
    mDebugReader.startThread(juce::Thread::Priority::low);
    mSampleLoader.startThread(juce::Thread::Priority::normal);
    if (mDeviceManager)
        mUdpServer.startThread(juce::Thread::Priority::normal);

    // -- Start the audio source (real callback or headless fallback) -------
    // Picks based on whether the device manager has a current device.
    // Blocks until process_audio has ticked at least once, or 5 s with a
    // warning. After this returns the engine is fully responsive: OSC sent
    // via sendOSC() / UDP will be drained on the next audio block.
    startAudioSource();

#ifdef __APPLE__
    // CoreAudio default-output listener: JUCE only watches device
    // connect/disconnect, not "user changed default in System Settings".
    // Only install on a real device; a headless-fallback engine has no
    // system default to track.
    if (mActiveSource == AudioSource::RealCallback &&
        !mDefaultDevicePropertyListenerInstalled) {
        AudioObjectPropertyAddress pa = {
            kAudioHardwarePropertyDefaultOutputDevice,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };
        if (AudioObjectAddPropertyListener(kAudioObjectSystemObject, &pa,
                                           &SupersonicEngine::defaultDevicePropertyListenerProc,
                                           this) == noErr) {
            mDefaultDevicePropertyListenerInstalled = true;
        }
    }
#endif

    if (testInitFailure) {
        auto msg = testInitFailure();
        if (!msg.empty()) throw std::runtime_error(msg);
    }

    mRunning.store(true);
    setEngineState(EngineState::Running, "boot");
}

void SupersonicEngine::shutdown() {
    // Don't early-out on !mRunning here: a partial init() (which
    // throws before mRunning becomes true) still needs the cleanup below
    // to run, particularly the macOS CoreAudio property listener removal,
    // which would otherwise fire against a destroyed `this`. Each cleanup
    // step below is individually guarded against missing resources.
    bool wasRunning = mRunning.exchange(false);
    if (wasRunning)
        setEngineState(EngineState::Stopped, "shutdown");

    // Stop recording if active
    if (isRecording())
        stopRecording();

    mHeadlessDriver.signalThreadShouldExit();
    mUdpServer.signalThreadShouldExit();
    mPrescheduler.signalThreadShouldExit();
    mReplyReader.signalThreadShouldExit();
    mDebugReader.signalThreadShouldExit();
    mSampleLoader.signalThreadShouldExit();

    // Wake atomic waiters so threads can exit.
    // Must increment value so wait() sees a change and returns.
    mAudioCallback.processCount.fetch_add(1, std::memory_order_release);
    mAudioCallback.processCount.notify_all();

    // Wake SampleLoader's WaitableEvent so it can see threadShouldExit
    mSampleLoader.wake();

    if (mDeviceManager) {
        mDeviceManager->removeChangeListener(this);
        mDeviceManager->removeAudioCallback(&mAudioCallback);
        mDeviceManager->closeAudioDevice();
        mDeviceManager.reset();
    }

#ifdef __APPLE__
    if (mDefaultDevicePropertyListenerInstalled) {
        AudioObjectPropertyAddress pa = {
            kAudioHardwarePropertyDefaultOutputDevice,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };
        AudioObjectRemovePropertyListener(
            kAudioObjectSystemObject, &pa,
            &SupersonicEngine::defaultDevicePropertyListenerProc,
            this);
        mDefaultDevicePropertyListenerInstalled = false;
    }
    AggregateDeviceHelper::destroy();
#endif

    mHeadlessDriver.stopThread(2000);
    mSampleLoader.stopThread(2000);
    mUdpServer.stopThread(2000);
    mPrescheduler.stopThread(2000);
    mReplyReader.stopThread(2000);
    mDebugReader.stopThread(2000);

    // Destroy engine-owned shared memory (after World is gone)
    g_external_shared_memory = nullptr;
    g_external_audio_buffers = nullptr;
    mShmemCreator.reset();
}

// --- OSC send with cache interception ---

void SupersonicEngine::sendOSC(const uint8_t* data, uint32_t size) {
    if (size >= 8 && data[0] == '/') {
        interceptForCache(data, size);
    }
    mUdpServer.sendInProcess(data, size);
}

bool SupersonicEngine::interceptBufferFreed(const uint8_t* data, uint32_t size) {
    // Quick prefix check — "/supersonic/buffer/freed" starts with '/'
    if (size < 28 || data[0] != '/') return false;
    try {
        osc::ReceivedPacket pkt(reinterpret_cast<const char*>(data),
                                static_cast<osc::osc_bundle_element_size_t>(size));
        osc::ReceivedMessage msg(pkt);
        if (std::strcmp(msg.AddressPattern(), "/supersonic/buffer/freed") != 0)
            return false;

        auto it = msg.ArgumentsBegin();
        int bufnum = 0;
        uintptr_t ptr = 0;
        if (it != msg.ArgumentsEnd()) { bufnum = it->AsInt32Unchecked(); ++it; }
        if (it != msg.ArgumentsEnd()) { ptr = static_cast<uintptr_t>(it->AsInt64Unchecked()); }

        if (ptr) zfree(reinterpret_cast<void*>(ptr));
        mStateCache.uncacheBuffer(bufnum);
        return true;
    } catch (...) {
        return false;
    }
}

void SupersonicEngine::interceptForCache(const uint8_t* data, uint32_t size) {
    try {
        osc::ReceivedPacket pkt(reinterpret_cast<const char*>(data),
                                static_cast<osc::osc_bundle_element_size_t>(size));
        osc::ReceivedMessage msg(pkt);
        const char* addr = msg.AddressPattern();

        if (std::strcmp(addr, "/d_recv") == 0) {
            // Extract synthdef blob, parse name, cache it
            auto it = msg.ArgumentsBegin();
            if (it != msg.ArgumentsEnd() && it->IsBlob()) {
                const void* blobData;
                osc::osc_bundle_element_size_t blobSize;
                it->AsBlob(blobData, blobSize);
                if (blobData && blobSize > 0) {
                    auto* blobBytes = static_cast<const uint8_t*>(blobData);
                    std::string name = StateCache::extractSynthDefName(blobBytes, blobSize);
                    if (!name.empty()) {
                        mStateCache.cacheSynthDef(name,
                            std::vector<uint8_t>(blobBytes, blobBytes + blobSize));
                    }
                }
            }
        } else if (std::strcmp(addr, "/d_free") == 0) {
            for (auto it = msg.ArgumentsBegin(); it != msg.ArgumentsEnd(); ++it) {
                if (it->IsString())
                    mStateCache.uncacheSynthDef(it->AsStringUnchecked());
            }
        } else if (std::strcmp(addr, "/d_freeAll") == 0) {
            mStateCache.clearSynthDefs();
        } else if (std::strcmp(addr, "/b_allocRead") == 0) {
            auto it = msg.ArgumentsBegin();
            int bufnum = 0, startFrame = 0, numFrames = 0;
            std::string path;
            if (it != msg.ArgumentsEnd()) { bufnum = it->AsInt32Unchecked(); ++it; }
            if (it != msg.ArgumentsEnd()) { path = it->AsStringUnchecked(); ++it; }
            if (it != msg.ArgumentsEnd()) { startFrame = it->AsInt32Unchecked(); ++it; }
            if (it != msg.ArgumentsEnd()) { numFrames = it->AsInt32Unchecked(); ++it; }
            mStateCache.cacheBuffer({bufnum, path, startFrame, numFrames, 0, 0});
        } else if (std::strcmp(addr, "/b_free") == 0) {
            auto it = msg.ArgumentsBegin();
            if (it != msg.ArgumentsEnd())
                mStateCache.uncacheBuffer(it->AsInt32Unchecked());
        }
    } catch (...) {
        // Don't let cache interception errors break message delivery
    }
}

// --- Variadic send helpers ---

void SupersonicEngine::sendBundle(double ntpTimeSec, std::initializer_list<OscPacket> messages) {
    // Convert NTP seconds (double) to NTP timetag (uint64)
    // NTP timetag: upper 32 bits = seconds, lower 32 bits = fractional
    uint32_t secs = static_cast<uint32_t>(ntpTimeSec);
    uint32_t frac = static_cast<uint32_t>((ntpTimeSec - secs) * 4294967296.0);
    uint64_t tag = (static_cast<uint64_t>(secs) << 32) | frac;
    auto pkt = OscBuilder::bundle(tag, messages);
    sendOSC(pkt.ptr(), pkt.size());
}

// --- Device management ---

std::vector<DeviceInfo> SupersonicEngine::listDevices(bool rescan) const {
    std::vector<DeviceInfo> result;
    if (!mDeviceManager) return result;

    // Cache hit — see SupersonicEngine.h for the rationale (JUCE WASAPI
    // probing is ~10 s for a typical device set, called multiple times
    // during boot). Skip the cache when rescan=true (explicit refresh) or
    // when the cache has been invalidated by an audioDeviceListChanged.
    if (!rescan) {
        std::lock_guard<std::mutex> lk(mListDevicesMutex);
        if (!mCachedDevices.empty()
            && mCachedDevicesAt.time_since_epoch().count() != 0)
            return mCachedDevices;
    }

#ifdef __APPLE__
    // Build a name→transportType map from CoreAudio for all devices
    std::map<std::string, uint32_t> transportMap;
    {
        AudioObjectPropertyAddress pa = {
            kAudioHardwarePropertyDevices,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };
        UInt32 dataSize = 0;
        if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &pa, 0, nullptr, &dataSize) == noErr) {
            auto count = dataSize / sizeof(AudioObjectID);
            std::vector<AudioObjectID> ids(count);
            if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &pa, 0, nullptr, &dataSize, ids.data()) == noErr) {
                for (auto id : ids) {
                    // Get name
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

                    // Get transport type
                    AudioObjectPropertyAddress tAddr = {
                        kAudioDevicePropertyTransportType,
                        kAudioObjectPropertyScopeGlobal,
                        kAudioObjectPropertyElementMain
                    };
                    UInt32 transport = 0;
                    UInt32 tSize = sizeof(transport);
                    AudioObjectGetPropertyData(id, &tAddr, 0, nullptr, &tSize, &transport);

                    transportMap[std::string(buf)] = transport;
                }
            }
        }
    }
#endif

    // JUCE appends " (N)" suffixes to disambiguate duplicate CoreAudio names.
    // This lambda strips the suffix for fallback matching against CoreAudio names.
#ifdef __APPLE__
    // Build a parallel name→AudioObjectID map (alongside transportMap) so
    // we can query per-device CoreAudio properties without opening JUCE
    // devices — critical when an aggregate is active and probing a sub-
    // device via JUCE would disrupt the live callback.
    std::map<std::string, AudioObjectID> idMap;
    {
        AudioObjectPropertyAddress pa = {
            kAudioHardwarePropertyDevices,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };
        UInt32 dataSize = 0;
        if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &pa, 0, nullptr, &dataSize) == noErr) {
            auto count = dataSize / sizeof(AudioObjectID);
            std::vector<AudioObjectID> ids(count);
            if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &pa, 0, nullptr, &dataSize, ids.data()) == noErr) {
                for (auto id : ids) {
                    AudioObjectPropertyAddress nameAddr = {
                        kAudioDevicePropertyDeviceNameCFString,
                        kAudioObjectPropertyScopeGlobal,
                        kAudioObjectPropertyElementMain
                    };
                    CFStringRef cfName = nullptr;
                    UInt32 sz = sizeof(cfName);
                    if (AudioObjectGetPropertyData(id, &nameAddr, 0, nullptr, &sz, &cfName) != noErr || !cfName)
                        continue;
                    char buf[256];
                    CFStringGetCString(cfName, buf, sizeof(buf), kCFStringEncodingUTF8);
                    CFRelease(cfName);
                    idMap[std::string(buf)] = id;
                }
            }
        }
    }

    auto lookupTransport = [&transportMap](const std::string& juceName) -> uint32_t {
        auto it = transportMap.find(juceName);
        if (it != transportMap.end()) return it->second;
        for (auto& [caName, transport] : transportMap)
            if (deviceNameMatches(juceName, caName)) return transport;
        return 0;
    };

    auto lookupID = [&idMap](const std::string& juceName) -> AudioObjectID {
        auto it = idMap.find(juceName);
        if (it != idMap.end()) return it->second;
        for (auto& [caName, id] : idMap)
            if (deviceNameMatches(juceName, caName)) return id;
        return kAudioObjectUnknown;
    };

    // Channel count via CoreAudio's kAudioDevicePropertyStreamConfiguration.
    // Sums channels across all streams in the requested scope. Works
    // without opening the device via JUCE — so it's safe to call when
    // the aggregate is active and skipAllProbing is true.
    auto scopeChannelCount = [](AudioObjectID devID, bool isInput) -> int {
        if (devID == kAudioObjectUnknown) return 0;
        AudioObjectPropertyAddress addr = {
            kAudioDevicePropertyStreamConfiguration,
            isInput ? kAudioObjectPropertyScopeInput : kAudioObjectPropertyScopeOutput,
            kAudioObjectPropertyElementMain
        };
        UInt32 sz = 0;
        if (AudioObjectGetPropertyDataSize(devID, &addr, 0, nullptr, &sz) != noErr || sz == 0)
            return 0;
        std::vector<uint8_t> buf(sz);
        auto* bl = reinterpret_cast<AudioBufferList*>(buf.data());
        if (AudioObjectGetPropertyData(devID, &addr, 0, nullptr, &sz, bl) != noErr)
            return 0;
        int total = 0;
        for (UInt32 i = 0; i < bl->mNumberBuffers; ++i)
            total += (int)bl->mBuffers[i].mNumberChannels;
        return total;
    };
#endif

    // When an aggregate is active, DON'T probe any devices via createDevice.
    // Creating a JUCE AudioIODevice wrapper on a subdevice of our aggregate
    // (e.g. MacBook Pro Microphone when the aggregate owns it) and then
    // destroying it at end of unique_ptr scope closes the HAL IOProc and
    // silences the aggregate. We return device names without sample-rate /
    // buffer-size info in this case; cached values from pre-aggregate
    // enumeration remain with the GUI until the aggregate is torn down.
    bool skipAllProbing = false;
#ifdef __APPLE__
    if (AggregateDeviceHelper::exists())
        skipAllProbing = true;
#endif
    // Also skip probing the currently open device — its handle is live.
    std::string activeDeviceName;
    if (auto* dev = mDeviceManager->getCurrentAudioDevice())
        activeDeviceName = dev->getName().toStdString();

    auto& types = mDeviceManager->getAvailableDeviceTypes();
    for (auto* type : types) {
        if (rescan) type->scanForDevices();

        auto populateFromDevice = [](DeviceInfo& info, juce::AudioIODevice* dev) {
            for (auto r : dev->getAvailableSampleRates())
                info.availableSampleRates.push_back(r);
            for (auto b : dev->getAvailableBufferSizes())
                info.availableBufferSizes.push_back(b);
            info.maxOutputChannels = dev->getOutputChannelNames().size();
            info.maxInputChannels  = dev->getInputChannelNames().size();
        };

        std::string typeNameStr = type->getTypeName().toStdString();
        auto shouldSkipProbe = [&](const std::string& name) {
            return skipAllProbing || name == activeDeviceName;
        };

        // Enumerate output devices
        auto outputNames = type->getDeviceNames(false);
        for (auto& devName : outputNames) {
            DeviceInfo info;
            info.name = devName.toStdString();
            info.typeName = typeNameStr;
#ifdef __APPLE__
            info.transportType = lookupTransport(info.name);
#endif

            if (!shouldSkipProbe(info.name)) {
                std::unique_ptr<juce::AudioIODevice> tempDev(
                    type->createDevice(devName, juce::String()));
                if (tempDev)
                    populateFromDevice(info, tempDev.get());
            }
#ifdef __APPLE__
            // Fill maxOutputChannels from CoreAudio if we don't already have
            // it (either because we skipped probing, or the JUCE probe
            // didn't return the full count). CoreAudio's stream
            // configuration is authoritative for a full-duplex multi-
            // channel device like a MOTU: no need to open the device,
            // so it's safe even while the aggregate is live.
            if (info.maxOutputChannels == 0) {
                AudioObjectID devID = lookupID(info.name);
                info.maxOutputChannels = scopeChannelCount(devID, false);
            }
#else
            if (info.maxOutputChannels == 0) info.maxOutputChannels = 2;
#endif

            result.push_back(std::move(info));
        }

        // Enumerate input devices. A full-duplex device (e.g. a MOTU
        // soundcard with both playback and capture) shows up in both the
        // output and input enumerations with the same name. Merge the
        // input-side info into the existing entry rather than skipping —
        // previously we just `continue`d on alreadyListed, which meant
        // full-duplex inputs never got recorded and the GUI's input
        // dropdown silently omitted them.
        auto inputNames = type->getDeviceNames(true);
        for (auto& devName : inputNames) {
            std::string nameStr = devName.toStdString();

            DeviceInfo* existing = nullptr;
            for (auto& e : result) {
                if (e.name == nameStr && e.typeName == typeNameStr) {
                    existing = &e;
                    break;
                }
            }

            if (existing) {
                // Same device seen on the output side already. Add its
                // input capability to the existing entry.
                if (!shouldSkipProbe(existing->name)) {
                    std::unique_ptr<juce::AudioIODevice> tempDev(
                        type->createDevice(juce::String(), devName));
                    if (tempDev) {
                        existing->maxInputChannels =
                            tempDev->getInputChannelNames().size();
                    }
                }
#ifdef __APPLE__
                // Always check CoreAudio for the actual input count,
                // so full-duplex devices (MOTU etc.) report the true
                // number even when probing was skipped.
                if (existing->maxInputChannels == 0) {
                    AudioObjectID devID = lookupID(existing->name);
                    existing->maxInputChannels = scopeChannelCount(devID, true);
                }
#else
                if (existing->maxInputChannels == 0)
                    existing->maxInputChannels = 1;
#endif
                continue;
            }

            // Input-only device (e.g. MacBook Pro Microphone).
            DeviceInfo info;
            info.name = std::move(nameStr);
            info.typeName = typeNameStr;
#ifdef __APPLE__
            info.transportType = lookupTransport(info.name);
#endif

            if (!shouldSkipProbe(info.name)) {
                std::unique_ptr<juce::AudioIODevice> tempDev(
                    type->createDevice(juce::String(), devName));
                if (tempDev)
                    populateFromDevice(info, tempDev.get());
            }
#ifdef __APPLE__
            if (info.maxInputChannels == 0) {
                AudioObjectID devID = lookupID(info.name);
                info.maxInputChannels = scopeChannelCount(devID, true);
            }
#else
            if (info.maxInputChannels == 0) info.maxInputChannels = 1;
#endif

            result.push_back(std::move(info));
        }
    }

#ifdef __APPLE__
    // Filter out our managed aggregate device — it's an implementation detail.
    if (AggregateDeviceHelper::exists()) {
        auto aggName = AggregateDeviceHelper::currentName();
        result.erase(
            std::remove_if(result.begin(), result.end(),
                [&aggName](const DeviceInfo& d) { return d.name == aggName; }),
            result.end());
    }
#endif

    {
        std::lock_guard<std::mutex> lk(mListDevicesMutex);
        mCachedDevices = result;
        mCachedDevicesAt = std::chrono::steady_clock::now();
    }
    return result;
}

CurrentDeviceInfo SupersonicEngine::currentDevice() const {
    CurrentDeviceInfo info;
    if (!mDeviceManager) return info;

    auto* dev = mDeviceManager->getCurrentAudioDevice();
    if (!dev) return info;

    info.name     = dev->getName().toStdString();
    info.typeName = dev->getTypeName().toStdString();
    info.activeSampleRate    = dev->getCurrentSampleRate();
    info.activeBufferSize    = dev->getCurrentBufferSizeSamples();
    info.activeOutputChannels = dev->getActiveOutputChannels().countNumberOfSetBits();
    info.activeInputChannels  = dev->getActiveInputChannels().countNumberOfSetBits();
    info.outputLatencySamples = dev->getOutputLatencyInSamples();
    info.inputLatencySamples  = dev->getInputLatencyInSamples();

    juce::AudioDeviceManager::AudioDeviceSetup setup;
    mDeviceManager->getAudioDeviceSetup(setup);
    info.inputDeviceName = setup.inputDeviceName.toStdString();

    // If running on an aggregate device, report the real underlying names
    // so the GUI sees the actual hardware, not "SuperSonic".
    if (!mRealOutputDeviceName.empty())
        info.name = mRealOutputDeviceName;
    if (!mRealInputDeviceName.empty())
        info.inputDeviceName = mRealInputDeviceName;

    for (auto r : dev->getAvailableSampleRates())
        info.availableSampleRates.push_back(r);
    for (auto b : dev->getAvailableBufferSizes())
        info.availableBufferSizes.push_back(b);

    // Populate max channel counts. When on aggregate, query the real
    // underlying devices (not the aggregate wrapper — which reports
    // the union of sub-device channels). When on a plain device,
    // JUCE's channel-name lists are accurate.
    info.maxOutputChannels = dev->getOutputChannelNames().size();
    info.maxInputChannels  = dev->getInputChannelNames().size();
#ifdef __APPLE__
    auto caChannelCount = [](const std::string& name, bool isInput) -> int {
        if (name.empty()) return 0;
        AudioObjectPropertyAddress listAddr = {
            kAudioHardwarePropertyDevices,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };
        UInt32 listSize = 0;
        if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &listAddr, 0, nullptr, &listSize) != noErr)
            return 0;
        std::vector<AudioObjectID> ids(listSize / sizeof(AudioObjectID));
        if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &listAddr, 0, nullptr, &listSize, ids.data()) != noErr)
            return 0;
        for (auto id : ids) {
            AudioObjectPropertyAddress nameAddr = {
                kAudioDevicePropertyDeviceNameCFString,
                kAudioObjectPropertyScopeGlobal,
                kAudioObjectPropertyElementMain
            };
            CFStringRef cfName = nullptr;
            UInt32 sz = sizeof(cfName);
            if (AudioObjectGetPropertyData(id, &nameAddr, 0, nullptr, &sz, &cfName) != noErr || !cfName)
                continue;
            char buf[256];
            CFStringGetCString(cfName, buf, sizeof(buf), kCFStringEncodingUTF8);
            CFRelease(cfName);
            if (name != buf) continue;
            AudioObjectPropertyAddress scopeAddr = {
                kAudioDevicePropertyStreamConfiguration,
                isInput ? kAudioObjectPropertyScopeInput : kAudioObjectPropertyScopeOutput,
                kAudioObjectPropertyElementMain
            };
            UInt32 cfgSize = 0;
            if (AudioObjectGetPropertyDataSize(id, &scopeAddr, 0, nullptr, &cfgSize) != noErr || cfgSize == 0)
                return 0;
            std::vector<uint8_t> cfgBuf(cfgSize);
            auto* bl = reinterpret_cast<AudioBufferList*>(cfgBuf.data());
            if (AudioObjectGetPropertyData(id, &scopeAddr, 0, nullptr, &cfgSize, bl) != noErr)
                return 0;
            int total = 0;
            for (UInt32 i = 0; i < bl->mNumberBuffers; ++i)
                total += (int)bl->mBuffers[i].mNumberChannels;
            return total;
        }
        return 0;
    };
    // Prefer real-device counts when we're on an aggregate.
    if (!mRealOutputDeviceName.empty()) {
        int n = caChannelCount(mRealOutputDeviceName, false);
        if (n > 0) info.maxOutputChannels = n;
    }
    if (!mRealInputDeviceName.empty()) {
        int n = caChannelCount(mRealInputDeviceName, true);
        if (n > 0) info.maxInputChannels = n;
    }
#endif

    return info;
}

// ── Audio source state machine ──────────────────────────────────────────────
//
// See the contract on the enum/helpers in SupersonicEngine.h.

SupersonicEngine::AudioSource SupersonicEngine::desiredAudioSource() const {
    if (mDeviceManager && mDeviceManager->getCurrentAudioDevice())
        return AudioSource::RealCallback;
    return AudioSource::Headless;
}

void SupersonicEngine::startAudioSource() {
    if (mActiveSource != AudioSource::None) {
        // Should be unreachable: every caller stops before starting.
        // Assert in debug so a regression fails CI loudly; log+return in
        // release so we don't crash a user session.
        jassertfalse;
        fprintf(stderr,
                "[supersonic] BUG: startAudioSource called while %s already active\n",
                mActiveSource == AudioSource::RealCallback ? "RealCallback" : "Headless");
        fflush(stderr);
        return;
    }

    uint32_t before = mAudioCallback.processCount.load(std::memory_order_acquire);

    if (desiredAudioSource() == AudioSource::RealCallback) {
        mDeviceManager->addAudioCallback(&mAudioCallback);
        // addChangeListener is idempotent (JUCE's ListenerList dedupes), so
        // re-attaching across hot-plug / swap sequences is harmless.
        mDeviceManager->addChangeListener(this);
        mActiveSource = AudioSource::RealCallback;
    } else {
        if (mDeviceManager) {
            fprintf(stderr,
                    "[supersonic] no audio device opened, running via "
                    "headless fallback driver\n");
            fflush(stderr);
        }
        mHeadlessDriver.configure(&mAudioCallback, &mSampleLoader,
                                   mCurrentConfig.sampleRate,
                                   mCurrentConfig.numOutputChannels,
                                   mCurrentConfig.numInputChannels);
        mHeadlessDriver.startThread(juce::Thread::Priority::highest);
        mActiveSource = AudioSource::Headless;
    }

    waitForFirstAudioTick(before);
}

void SupersonicEngine::stopAudioSource() {
    switch (mActiveSource) {
    case AudioSource::None:
        return;
    case AudioSource::RealCallback:
        if (mDeviceManager)
            mDeviceManager->removeAudioCallback(&mAudioCallback);
        // Change listener is NOT removed here; it survives swaps and is
        // removed only in shutdown(). Removing it would lose hot-plug
        // events between stop and the next start.
        break;
    case AudioSource::Headless:
        mHeadlessDriver.signalThreadShouldExit();
        mHeadlessDriver.stopThread(2000);
        break;
    }
    mActiveSource = AudioSource::None;
}

void SupersonicEngine::waitForFirstAudioTick(uint32_t before) {
    constexpr int kTimeoutMs = 5000;
    auto start = std::chrono::steady_clock::now();
    auto deadline = start + std::chrono::milliseconds(kTimeoutMs);
    while (mAudioCallback.processCount.load(std::memory_order_acquire) == before
           && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    bool ticked = mAudioCallback.processCount.load(std::memory_order_acquire) != before;
    if (!ticked) {
        fprintf(stderr,
                "[supersonic] WARNING: audio callbacks not firing after %d ms, "
                "engine is alive but the audio thread has not started\n", kTimeoutMs);
        fflush(stderr);
    } else {
        auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        fprintf(stderr, "[supersonic] audio callbacks started (%lld ms)\n",
                static_cast<long long>(elapsedMs));
        fflush(stderr);
    }
}

juce::String SupersonicEngine::reinitialiseWithDefaultsPreservingConfig() {
    int prevRate = mCurrentConfig.sampleRate;
    auto prevSetup = mDeviceManager->getAudioDeviceSetup();
    int prevBufSize = prevSetup.bufferSize;

    // Refresh suppression timestamp before each JUCE call to prevent
    // changeListenerCallback feedback storms.
    mLastSelfTriggeredChange = std::chrono::steady_clock::now();
    auto err = mDeviceManager->initialiseWithDefaultDevices(0, 2);
    if (err.isNotEmpty()) {
        mLastSelfTriggeredChange = std::chrono::steady_clock::now();
        err = mDeviceManager->initialiseWithDefaultDevices(0, 0);
    }
    if (err.isNotEmpty()) return err;

    // Don't second-guess JUCE's negotiation on wireless devices. AirPlay
    // / Bluetooth negotiate a specific buffer size with the remote end
    // (often 512+); JUCE reports many buffer sizes as "available" but in
    // practice only the negotiated one actually delivers audio. Forcing
    // a different size runs the IOProc (scope stays happy) but produces
    // silence at the speakers. For non-wireless devices we still
    // preserve the user's previous rate/buffer so GUI selections stick
    // across System Output toggles.
    auto setup = mDeviceManager->getAudioDeviceSetup();
    bool isWireless = false;
#ifdef __APPLE__
    if (auto* dev = mDeviceManager->getCurrentAudioDevice()) {
        std::string curName = dev->getName().toStdString();
        for (auto& d : listDevices(false)) {
            if (deviceNameMatches(d.name, curName) && d.isWirelessTransport()) {
                isWireless = true;
                break;
            }
        }
    }
#endif
    if (!isWireless) {
        setup.sampleRate = static_cast<double>(prevRate);
        setup.bufferSize = prevBufSize;
        mLastSelfTriggeredChange = std::chrono::steady_clock::now();
        mDeviceManager->setAudioDeviceSetup(setup, true);
    } else {
        // Wireless device is now active. AirPlay 1 negotiates 44.1 kHz
        // with the receiver, but AirPlay 2 receivers commonly support
        // 48 kHz. Probe the device's available rates — if prevRate is
        // in the list, force it rather than silently accepting the
        // receiver's default. That way a modern AirPlay 2 speaker
        // doesn't downgrade a 48 kHz session to 44.1.
        bool forcedPrev = false;
        if (auto* dev = mDeviceManager->getCurrentAudioDevice()) {
            bool prevSupported = false;
            for (auto r : dev->getAvailableSampleRates()) {
                if (static_cast<int>(r) == prevRate) {
                    prevSupported = true;
                    break;
                }
            }
            if (prevSupported && static_cast<int>(setup.sampleRate) != prevRate) {
                setup.sampleRate = static_cast<double>(prevRate);
                mLastSelfTriggeredChange = std::chrono::steady_clock::now();
                auto err2 = mDeviceManager->setAudioDeviceSetup(setup, true);
                if (err2.isEmpty()) {
                    forcedPrev = true;
                    fprintf(stderr, "[audio-device] reinit: wireless device supports "
                            "prev rate %d — forcing it (was %.0f)\n",
                            prevRate, dev->getCurrentSampleRate());
                    fflush(stderr);
                } else {
                    fprintf(stderr, "[audio-device] reinit: setAudioDeviceSetup at "
                            "prev rate %d failed on wireless (%s), keeping negotiated\n",
                            prevRate, err2.toRawUTF8());
                    fflush(stderr);
                }
            }
        }
        if (!forcedPrev) {
            fprintf(stderr, "[audio-device] reinit: keeping JUCE's negotiated rate=%.0f buf=%d "
                    "for wireless device (prev rate=%d buf=%d)\n",
                    setup.sampleRate, setup.bufferSize, prevRate, prevBufSize);
            fflush(stderr);
        }
    }

    // Post-init cleanup: we just switched away from whatever device we
    // were on (aggregate or otherwise) to JUCE's default. If we were on
    // an aggregate, drop the stale real-device-name state so
    // currentDevice() reports the actual new device, not the previous
    // aggregate's sub-devices. Same story for leftover aggregates in
    // CoreAudio — clean them up now that JUCE has switched away.
#ifdef __APPLE__
    mRealOutputDeviceName.clear();
    mRealInputDeviceName.clear();
    AggregateDeviceHelper::destroyPrevious();
    if (AggregateDeviceHelper::exists()) {
        AggregateDeviceHelper::destroy();
    }
#endif
    return {};
}

SwapResult SupersonicEngine::switchDevice(const std::string& rawOutputName,
                                           double sampleRate,
                                           int bufferSize,
                                           bool forceCold,
                                           const std::string& rawInputName) {
    // Normalise raw CoreAudio names to JUCE's disambiguated form. Callers
    // that source names from CoreAudio APIs (setDeviceMode's default-
    // output resolution, platform property listeners) otherwise hand raw
    // names straight to setAudioDeviceSetup, which errors "No such device"
    // when CoreAudio has duplicate base names (two identical USB
    // interfaces, two AirPlay endpoints with the same base name).
    // Caller-supplied names from the GUI dropdown are already JUCE-form
    // and pass through unchanged. Sentinels like "__system__" / "__none__"
    // don't match anything and also pass through. Shadowing the function
    // parameters keeps the rest of this function free to use the names
    // without thinking about which layer they came from.
    std::string deviceName = rawOutputName;
    std::string inputDeviceName = rawInputName;
    if (mDeviceManager && (!deviceName.empty() || !inputDeviceName.empty())) {
        std::vector<std::string> visibleNames;
        for (auto& d : listDevices(false)) visibleNames.push_back(d.name);
        deviceName      = sonicpi::device::resolveJuceDeviceName(deviceName, visibleNames);
        inputDeviceName = sonicpi::device::resolveJuceDeviceName(inputDeviceName, visibleNames);
    }

    SwapResult result;
    result.deviceName = deviceName;
    bool recovered = false;

    // No-op detection: destroying and recreating an identical aggregate is
    // fragile — CoreAudio sometimes stops the new instance within a
    // callback or two. If the caller asked for exactly what we already
    // have, short-circuit.
    if (!deviceName.empty() && inputDeviceName.empty() && sampleRate <= 0 && bufferSize <= 0 && !forceCold) {
        std::string activeReal = mRealOutputDeviceName.empty()
            ? (mDeviceManager && mDeviceManager->getCurrentAudioDevice()
               ? mDeviceManager->getCurrentAudioDevice()->getName().toStdString() : "")
            : mRealOutputDeviceName;
        if (activeReal == deviceName) {
            result.success = true;
            result.type = SwapType::Hot;
            result.deviceName = deviceName;
            result.sampleRate = mCurrentConfig.sampleRate;
            result.bufferSize = mCurrentConfig.bufferSize;
            return result;
        }
    }

    // Reject "add mic while current output is wireless" upfront, BEFORE
    // any cold-swap work. The aggregate filter later in this function
    // would drop the mic anyway, but by that point we've already
    // triggered a cold swap that can race the audio thread's scope UGen
    // and crash in ScopeOut2_next.
    if (auto err = refuseWirelessMicAddition(deviceName, inputDeviceName);
        !err.empty()) {
        result.error = err;
        return result;
    }

    // Reject swap with an unknown output / input device name BEFORE any
    // destructive state mutation. Without this, switchDevice mutates
    // mCurrentConfig.numInputChannels, opts[], destroys the World, and
    // pauses the audio callback before discovering at setAudioDeviceSetup
    // time that the name doesn't resolve. The half-built state poisons
    // the next cold-swap reinit (mixer_group never confirms via /n_go).
    if (auto err = refuseUnknownDeviceName(deviceName, inputDeviceName);
        !err.empty()) {
        result.error = err;
        fprintf(stderr, "[switchDevice] refused: %s\n", err.c_str());
        fflush(stderr);
        return result;
    }

    // Try to acquire swap mutex (non-blocking)
    if (!mSwapMutex.try_lock()) {
        result.error = "swap already in progress";
        return result;
    }
    std::lock_guard<std::mutex> guard(mSwapMutex, std::adopt_lock);

    // Resolve the requested device name(s) against the scoped driver:
    // mIntendedDriver if a switchDriver-no-pref pick is pending, else
    // JUCE's actual current type. Pending-driver scoping is required
    // for the two-step driver→device flow — a switchDevice for the
    // intended driver's device would otherwise scope against JUCE's
    // unchanged type and fail to resolve.
    bool        crossDriver       = false;
    std::string crossDriverTarget;
    std::string crossDriverDevice;
    if (mDeviceManager) {
        std::string juceCurrentType;
        if (auto* dev = mDeviceManager->getCurrentAudioDevice())
            juceCurrentType = dev->getTypeName().toStdString();
        else
            juceCurrentType = mDeviceManager->getCurrentAudioDeviceType().toStdString();

        const std::string scopedDriver =
            mIntendedDriver.empty() ? juceCurrentType : mIntendedDriver;

        std::vector<std::pair<std::string, std::string>> deviceTable;
        for (auto& d : listDevices(false))
            deviceTable.emplace_back(d.typeName, d.name);

        auto considerName = [&](const std::string& name) -> std::string {
            if (name.empty() || name == "__system__" || name == "__none__")
                return {};
            auto plan = sonicpi::device::planDeviceSwitch(
                scopedDriver, name, deviceTable);
            if (!plan.deviceFound) {
                return "device '" + name + "' not available on driver '"
                     + (scopedDriver.empty() ? "(none)" : scopedDriver) + "'";
            }
            // Compare against JUCE's actual type — that's what
            // setCurrentAudioDeviceType has to be called for, regardless
            // of the pending-intent scope used for the lookup.
            if (plan.targetDriver != juceCurrentType && !crossDriver) {
                crossDriver       = true;
                crossDriverTarget = plan.targetDriver;
                crossDriverDevice = plan.targetDevice;
                fprintf(stderr,
                    "[audio-device] cross-driver: '%s' -> '%s' (device '%s')\n",
                    juceCurrentType.c_str(), crossDriverTarget.c_str(),
                    name.c_str());
                fflush(stderr);
            }
            return {};
        };
        if (auto err = considerName(deviceName); !err.empty()) {
            result.error = err;
            return result;
        }
        if (auto err = considerName(inputDeviceName); !err.empty()) {
            result.error = err;
            return result;
        }

        // ASIO is full-duplex single-device by spec — one open call
        // delivers both directions. On a cross-driver switch to ASIO
        // with an explicit output but no input, mirror the output to
        // the input. Without this, the info display reports "in 8"
        // while the input dropdown shows "-- None --".
        if (crossDriver && crossDriverTarget == "ASIO"
            && !deviceName.empty()
            && (inputDeviceName.empty() || inputDeviceName == "__none__")) {
            inputDeviceName = crossDriverDevice;
            fprintf(stderr,
                "[audio-device] ASIO full-duplex: mirroring '%s' to input\n",
                crossDriverDevice.c_str());
            fflush(stderr);
        }
    }

    // Determine current rate — from device if available, else from config
    double currentRate = 0.0;
    if (mDeviceManager) {
        auto* currentDev = mDeviceManager->getCurrentAudioDevice();
        currentRate = currentDev ? currentDev->getCurrentSampleRate() : 0.0;
    } else {
        currentRate = static_cast<double>(mCurrentConfig.sampleRate);
    }

#ifdef __APPLE__
    // If we're leaving a wireless device (AirPlay/Bluetooth) for a
    // non-wireless one, the currentRate is whatever the wireless receiver
    // negotiated (often 44.1 kHz — AirPlay 1's fixed rate). Sticking with
    // it on the new device would carry the AirPlay detour's rate onto
    // e.g. MacBook Pro Speakers, which doesn't want to run at 44.1. Use
    // the remembered pre-wireless rate instead so the probe below picks
    // it up and the new device opens at the right rate.
    if (sampleRate <= 0 && mPreWirelessRate > 0 && mDeviceManager) {
        bool currentIsWireless = false;
        bool targetIsWireless  = false;
        std::string curName;
        if (auto* curDev = mDeviceManager->getCurrentAudioDevice())
            curName = curDev->getName().toStdString();
        std::string targetName = deviceName.empty() ? mDeviceMode : deviceName;
        if (!curName.empty() || !targetName.empty()) {
            for (auto& d : listDevices(false)) {
                if (!curName.empty() && deviceNameMatches(d.name, curName)
                    && d.isWirelessTransport())
                    currentIsWireless = true;
                if (!targetName.empty() && deviceNameMatches(d.name, targetName)
                    && d.isWirelessTransport())
                    targetIsWireless = true;
            }
        }
        const double resolved = sonicpi::device::resolveWirelessExitRate(
            sampleRate, mPreWirelessRate, currentRate,
            currentIsWireless, targetIsWireless);
        if (resolved != sampleRate) {
            fprintf(stderr, "[audio-device] restoring pre-wireless rate %d "
                    "(current=%.0f, target='%s')\n",
                    mPreWirelessRate, currentRate, targetName.c_str());
            fflush(stderr);
            sampleRate = resolved;
        }
    }
#endif

    // When no explicit sample rate requested, probe the target device to see
    // if the current rate is supported. If not, auto-select the nearest
    // available rate — this makes the swap a cold swap (world rebuild).
    probeAndAdjustForTargetRate(deviceName,      false, sampleRate, currentRate);
    probeAndAdjustForTargetRate(inputDeviceName, true,  sampleRate, currentRate);

    // Auto-enable inputs if the caller named a device but we have 0 inputs.
    // Must be BEFORE isCold so forceCold takes effect.
    if (!inputDeviceName.empty() && inputDeviceName != "__none__"
        && mCurrentConfig.numInputChannels == 0) {
#ifdef __APPLE__
        // Log mic permission status for diagnostics — but don't refuse
        // enabling inputs. supersonic's TCC query may return notDetermined
        // when launched as a child of the GUI, while CoreAudio's actual
        // mic stream honours the GUI's grant via responsible-process
        // attribution. Trying anyway may work; if buffers come back zero,
        // we'll know TCC really is denying.
        std::string micStat = MicPermission::status();
        if (micStat != "authorized") {
            fprintf(stderr, "[audio-device] mic permission status=%s (proceeding anyway; "
                    "CoreAudio may still grant via GUI's responsible process)\n",
                    micStat.c_str());
            fflush(stderr);
        }
#endif
        int requested;
        if (mBootInputChannels > 0)        requested = mBootInputChannels;
        else if (mBootInputChannels < 0)   requested = kRequestMaxChannels;
        else                               requested = 2;

        // Clamp the requested count to the device's actual input
        // capacity. JUCE/WASAPI rejects setAudioDeviceSetup outright
        // when asked for more inputs than the device exposes; the
        // default -i sentinel asks for kRequestMaxChannels (64),
        // which exceeds most devices. probeDeviceChannelCount opens
        // a transient AudioIODevice and reads getInputChannelNames()
        // — true count, no live-device disturbance. Probe failure
        // (-1) means "unknown"; `requested` is used as-is.
        int probed = probeDeviceChannelCount(inputDeviceName, true);
        int reEnableCount = (probed > 0 && probed < requested) ? probed : requested;
        if (reEnableCount != requested) {
            fprintf(stderr, "[audio-device] auto-enabling %d input channels for '%s' "
                    "(requested %d, device max %d)\n",
                    reEnableCount, inputDeviceName.c_str(), requested, probed);
        } else {
            fprintf(stderr, "[audio-device] auto-enabling %d input channels for '%s'\n",
                    reEnableCount, inputDeviceName.c_str());
        }
        mCurrentConfig.numInputChannels = reEnableCount;
        uint32_t* opts = reinterpret_cast<uint32_t*>(ring_buffer_storage + WORLD_OPTIONS_START);
        opts[sonicpi::WorldOpts::kNumInputBusChannels] = static_cast<uint32_t>(reEnableCount);
        forceCold = true;
    }

    // Restore per-device sample rate if no explicit rate given.
    if (sampleRate <= 0 && !deviceName.empty()) {
        auto it = mDeviceRateMemory.find(deviceName);
        if (it != mDeviceRateMemory.end() && it->second > 0)
            sampleRate = static_cast<double>(it->second);
    }

    // Force cold swap when the target device will change the scsynth
    // world's bus count. Hot swaps keep the existing World; opts[5] /
    // opts[6] only get re-read by World_New on rebuild, so a hot swap
    // to a device with more channels leaves the World at the old count
    // and writes to higher buses land on internal private buses instead
    // of hardware. Probe the target device(s) and compare against the
    // current config.
    bool forceColdForChannels = false;
    if (mDeviceManager && !forceCold) {
        int probedOut = probeDeviceChannelCount(deviceName,      false);
        int probedIn  = probeDeviceChannelCount(inputDeviceName, true);
        if (probedOut > 0 && probedOut != mCurrentConfig.numOutputChannels)
            forceColdForChannels = true;
        if (probedIn  > 0 && probedIn  != mCurrentConfig.numInputChannels)
            forceColdForChannels = true;
        if (forceColdForChannels) {
            fprintf(stderr, "[audio-device] channel-count change detected "
                    "(probedOut=%d probedIn=%d currentOut=%d currentIn=%d) "
                    "— forcing cold swap so World rebuilds at new bus count\n",
                    probedOut, probedIn,
                    mCurrentConfig.numOutputChannels,
                    mCurrentConfig.numInputChannels);
            fflush(stderr);
        }
    }

    bool inputWasDropped = false;
    // Cross-driver swaps need a cold swap: the new AudioIODeviceType
    // may report different rate / channel-count / buffer-size ranges,
    // so the World must be rebuilt against the new device's specs.
    bool isCold = forceCold || forceColdForChannels || crossDriver
                || (sampleRate > 0 && sampleRate != currentRate);
    result.type = isCold ? SwapType::Cold : SwapType::Hot;

    if (isCold) setEngineState(EngineState::Restarting, "rate-change");
    if (onSwapEvent) onSwapEvent("swap:start", result);

    // --- Pause and optionally capture state ---
    if (isCold) {
        purge();
        mSampleLoader.pauseLoading();
    }
    mAudioCallback.pause();
    if (isCold) mStateCache.captureAll();

    // --- Stop audio ---
    stopAudioSource();

    if (isCold) destroy_world();

    // Snapshot the current device setup so we can restore it if the new
    // setAudioDeviceSetup call below fails. JUCE's AudioDeviceManager can
    // be left with no bound device after a failed setAudioDeviceSetup
    // (e.g. user-selected name not in the device list, exclusive-mode
    // contention, sample-rate-not-supported). Without restoring, the
    // rollback below re-attaches the audio callback to a manager with
    // no device — symptom: device-report shows currentIn=''/currentOut='',
    // GUI prefs read 0hz | 0buf | 0out | 0in, every scsynth reply times
    // out because the audio thread isn't ticking.
    juce::AudioDeviceManager::AudioDeviceSetup prevSetup;
    if (mDeviceManager) mDeviceManager->getAudioDeviceSetup(prevSetup);

    // --- Apply new device configuration ---
    std::string errStr;
    if (mDeviceManager) {
        // Cross-driver: move JUCE to the new AudioIODeviceType before
        // reading the setup. setCurrentAudioDeviceType internally calls
        // setAudioDeviceSetup with the new type's saved (often empty)
        // config; insertDefaultDeviceNames fills the empty field with
        // the alphabetical-first device of the type. The transient
        // open is discardable — outputDeviceName is overridden below
        // and setAudioDeviceSetup is re-run authoritatively. On
        // Windows ASIO an unplugged-but-registered driver can hang
        // here in IASIO::init().
        if (crossDriver) {
            mLastSelfTriggeredChange = std::chrono::steady_clock::now();
            mDeviceManager->setCurrentAudioDeviceType(
                juce::String(crossDriverTarget), false);
        }

        juce::AudioDeviceManager::AudioDeviceSetup setup;
        mDeviceManager->getAudioDeviceSetup(setup);

        if (crossDriver) {
            // The transient open above left setup carrying the alpha-
            // first device (or empty). Force-set to the resolved
            // crossDriverDevice so the setAudioDeviceSetup below is
            // unambiguous.
            setup.outputDeviceName = juce::String(crossDriverDevice);
            setup.inputDeviceName  = juce::String();
        }

#ifdef __APPLE__
        // If currently on an aggregate device, resolve back to real device names
        // so we don't recursively wrap aggregates.
        if (AggregateDeviceHelper::exists()) {
            if (setup.outputDeviceName.toStdString() == AggregateDeviceHelper::currentName()) {
                if (!mRealOutputDeviceName.empty())
                    setup.outputDeviceName = juce::String(mRealOutputDeviceName);
                if (!mRealInputDeviceName.empty())
                    setup.inputDeviceName = juce::String(mRealInputDeviceName);
            }
        }
#endif

        if (!deviceName.empty()) {
            setup.outputDeviceName = juce::String(deviceName);
        } else if (!mDeviceMode.empty()) {
            // Re-assert the user's explicit output choice on input-only switches
            setup.outputDeviceName = juce::String(mDeviceMode);
        }
        if (mCurrentConfig.numInputChannels > 0) {
            if (!inputDeviceName.empty()) {
                // Explicit input device requested — save for future re-enable
                setup.useDefaultInputChannels = false;
                setup.inputDeviceName = juce::String(inputDeviceName);
                mLastInputDeviceName = inputDeviceName;
                juce::BigInteger inputBits;
                inputBits.setRange(0, mCurrentConfig.numInputChannels, true);
                setup.inputChannels = inputBits;
            } else {
                // Re-enable inputs — must explicitly set the input device name.
                // useDefaultInputChannels won't auto-fill the device name when
                // numInputChansNeeded was 0 at init() time (boot with -i 0).
                setup.useDefaultInputChannels = false;
                juce::BigInteger inputBits;
                inputBits.setRange(0, mCurrentConfig.numInputChannels, true);
                setup.inputChannels = inputBits;

                if (setup.inputDeviceName.isEmpty()) {
                    if (!mLastInputDeviceName.empty()) {
                        setup.inputDeviceName = juce::String(mLastInputDeviceName);
                    } else {
                        auto* currentType = mDeviceManager->getCurrentDeviceTypeObject();
                        if (currentType) {
                            auto inputNames = currentType->getDeviceNames(true);
                            if (!inputNames.isEmpty())
                                setup.inputDeviceName = inputNames[0];
                        }
                    }
                }
            }
        } else {
            // Disable inputs — save the current input device name, then release
            if (!setup.inputDeviceName.isEmpty())
                mLastInputDeviceName = setup.inputDeviceName.toStdString();
            setup.useDefaultInputChannels = false;
            setup.inputChannels.clear();
            setup.inputDeviceName = "";
        }
        // Explicitly set output bits rather than relying on
        // useDefaultOutputChannels. JUCE's "default" is derived from the
        // numOutputChannelsNeeded passed at init() time — with
        // auto-max that's kRequestMaxChannels, but under some swap
        // sequences JUCE re-evaluates and reports 0 active outputs on
        // setAudioDeviceSetup (symptom: activeOut=0 on Loopback despite
        // the device having 4 channels). Setting kRequestMaxChannels
        // bits directly lets CoreAudio clamp to the device's real
        // channel count — same pattern as the input side.
        setup.useDefaultOutputChannels = false;
        {
            juce::BigInteger outputBits;
            outputBits.setRange(0, kRequestMaxChannels, true);
            setup.outputChannels = outputBits;
        }
        if (sampleRate > 0) setup.sampleRate = sampleRate;
        if (bufferSize > 0) setup.bufferSize = bufferSize;

#ifdef __APPLE__
        // On macOS, if input and output are different devices, create an
        // Aggregate Device with drift correction instead of relying on
        // JUCE's AudioIODeviceCombiner (which has no drift correction).
        // Skip aggregation for Bluetooth/AirPlay inputs — they force
        // low-quality codec modes and don't support drift correction.
        bool wasOnAggregate = AggregateDeviceHelper::exists();
        bool needsAggregate = !setup.outputDeviceName.isEmpty()
            && !setup.inputDeviceName.isEmpty()
            && setup.outputDeviceName != setup.inputDeviceName;

        bool dropInput = false;
        if (needsAggregate) {
            // Check transport types — skip aggregate for wireless (Bluetooth/
            // AirPlay) or virtual (Loopback, Blackhole) devices:
            //   * Wireless: can't be opened as HAL at all
            //   * Virtual:  no hardware clock; CoreAudio aggregate AND JUCE's
            //               combiner both crash inside AudioUnitRender.
            // In either case we drop the input — Sonic Pi can't mix mic +
            // virtual-output reliably. Users who need both must use macOS
            // aggregation in Audio MIDI Setup and pick that aggregate here.
            auto devices = listDevices();
            std::string outName = setup.outputDeviceName.toStdString();
            std::string inName  = setup.inputDeviceName.toStdString();
            // Look for a wireless / virtual sub-device and drop the input
            // if we find one — those transports can't be HAL-aggregated
            // (Bluetooth / AirPlay negotiate codec modes; Loopback-class
            // virtual devices crash inside AudioUnitRender via the HAL
            // combiner). The skip line below IS user-actionable and
            // always logs; per-device match tracing was noise.
            bool matched = false;
            for (auto& dev : devices) {
                bool nameMatch = (dev.name == outName || dev.name == inName);
                if (nameMatch) matched = true;
                if (nameMatch && !dev.isSuitableForAggregate()) {
                    needsAggregate = false;
                    dropInput = true;
                    const char* why = dev.isVirtualTransport() ? "virtual" : "wireless";
                    fprintf(stderr, "[audio-device] skipping aggregate — '%s' is %s; input disabled\n",
                            dev.name.c_str(), why);
                    fflush(stderr);
                    break;
                }
            }
            if (!matched) {
                fprintf(stderr, "[agg-filter] WARNING: no device matched outName='%s' inName='%s' "
                        "— filter never fired\n", outName.c_str(), inName.c_str());
                fflush(stderr);
            }
        }

        if (needsAggregate) {
            // Remember the real device names before replacing with aggregate
            mRealOutputDeviceName = setup.outputDeviceName.toStdString();
            mRealInputDeviceName  = setup.inputDeviceName.toStdString();

            // Pause CFRunLoop pumping to prevent JUCE's audioDeviceListChanged
            // from firing during aggregate destroy/create — it crashes trying
            // to reinitialise with a stale device reference.
            mSuppressRunLoop.store(true);
            // Pass the engine's current sample rate so the aggregate's
            // sub-devices are forced to the same rate — otherwise
            // CoreAudio will apply aggregate-level SRC inside the
            // IOProc to bridge a rate mismatch, producing "hideous
            // distortion" (user-reported symptom).
            double wantedRate = sampleRate > 0
                ? sampleRate
                : static_cast<double>(mCurrentConfig.sampleRate);
            auto aggName = AggregateDeviceHelper::createOrUpdate(
                mRealOutputDeviceName, mRealInputDeviceName, wantedRate);
            if (!aggName.empty()) {
                // Use the aggregate as a single device for both I/O
                setup.outputDeviceName = juce::String(aggName);
                setup.inputDeviceName  = juce::String(aggName);
                clampAggregateBufferIfNeeded(setup.bufferSize);

                // Let CoreAudio settle then rescan so JUCE sees the new aggregate
                juce::Thread::sleep(200);
                if (auto* type = mDeviceManager->getCurrentDeviceTypeObject())
                    type->scanForDevices();
            }
        } else {
            // Same device for both I/O, or no input — no aggregate needed.
            // Save the real input device name before clearing — when switching
            // to AirPlay (no aggregate), we want to restore the input device
            // when switching back to local speakers.
            if (!mRealInputDeviceName.empty())
                mLastInputDeviceName = mRealInputDeviceName;
            mRealOutputDeviceName.clear();
            mRealInputDeviceName.clear();

            // If we skipped aggregate because the output is unsuitable
            // (wireless or virtual), drop the input. Keeping it would make
            // JUCE fall back to its combiner, which has the same crash
            // as our aggregate (both use AudioUnitRender under the hood).
            if (dropInput && !setup.inputDeviceName.isEmpty()) {
                fprintf(stderr, "[audio-device] clearing input (was '%s') because output "
                        "can't be combined with it\n",
                        setup.inputDeviceName.toRawUTF8());
                fflush(stderr);
                mLastInputDeviceName = setup.inputDeviceName.toStdString();
                setup.inputDeviceName = "";
                setup.inputChannels.clear();
                inputWasDropped = true;
            }

            // Don't destroy aggregate yet — JUCE still references it.
            // setAudioDeviceSetup below will switch JUCE to the new device,
            // then we destroy the orphaned aggregate safely.
        }
#endif

        // Don't call closeAudioDevice() here — it races with JUCE's internal
        // CoreAudio lock on destruction of aggregates that contained virtual
        // sub-devices (_os_unfair_lock_unowned_abort). We rely on each new
        // aggregate having a unique name (see AggregateDeviceHelper) so
        // JUCE's setAudioDeviceSetup sees it as a different device and
        // reopens properly.

        fprintf(stderr, "[audio-device] calling setAudioDeviceSetup: out='%s' in='%s' sr=%.0f buf=%d\n",
                setup.outputDeviceName.toRawUTF8(),
                setup.inputDeviceName.toRawUTF8(),
                setup.sampleRate, setup.bufferSize);
        fflush(stderr);
        mLastSelfTriggeredChange = std::chrono::steady_clock::now();
        juce::String err = mDeviceManager->setAudioDeviceSetup(setup, true);
        fprintf(stderr, "[audio-device] setAudioDeviceSetup returned: '%s'\n",
                err.isEmpty() ? "OK" : err.toRawUTF8());
        fflush(stderr);
        if (err.isNotEmpty()) errStr = err.toStdString();

        // Input-fallback: if the setup failed specifically because the input
        // device couldn't be opened (Windows mic privacy denied, exclusive-
        // mode contention, …), retry with the input cleared. Output keeps
        // working and the user sees an empty input in prefs rather than the
        // whole rate change rolling back into a cold-swap rebuild loop.
        if (!errStr.empty()
            && setup.inputDeviceName.isNotEmpty()
            && errStr.find("input device") != std::string::npos)
        {
            const std::string firstError = errStr;
            const std::string failedInputName = setup.inputDeviceName.toStdString();
            const std::string pairedOutputName = setup.outputDeviceName.toStdString();
            juce::AudioDeviceManager::AudioDeviceSetup outOnly = setup;
            outOnly.inputDeviceName = juce::String();
            outOnly.useDefaultInputChannels = false;
            outOnly.inputChannels.clear();
            fprintf(stderr,
                    "[audio-device] input '%s' failed when paired with output '%s' "
                    "(%s) — retrying output-only\n",
                    failedInputName.c_str(), pairedOutputName.c_str(),
                    firstError.c_str());
            fflush(stderr);
            juce::String retryErr = mDeviceManager->setAudioDeviceSetup(outOnly, true);
            if (retryErr.isEmpty()) {
                setup = outOnly;
                errStr.clear();
                result.inputUnavailable = true;
                result.inputUnavailableReason = firstError;
                // Remember the (output, input) pair as known-bad so
                // sendDeviceReport hides this input from the dropdown
                // while pairedOutputName is the active output. Per-
                // output scoping: the same input can pair fine with a
                // different output (typical with WASAPI Shared vs
                // ASIO on the same hardware).
                {
                    std::lock_guard<std::mutex> lock(mUngatableInputPairsMutex);
                    mUngatableInputPairs.emplace(pairedOutputName, failedInputName);
                }
            } else {
                errStr = retryErr.toStdString();
                fprintf(stderr,
                        "[audio-device] output-only retry also failed: %s\n",
                        errStr.c_str());
                fflush(stderr);
            }
        }

#ifdef __APPLE__
        // Now JUCE has switched away from the old aggregate — safe to
        // destroy it. AggregateDeviceHelper stashes the previous ID in
        // sPrevAggregateID precisely so this happens after JUCE has moved.
        AggregateDeviceHelper::destroyPrevious();
        juce::Thread::sleep(150);
        // Also destroy the current one if we're no longer using an aggregate
        // (e.g. single-device setup that doesn't need input combining).
        if (wasOnAggregate && !needsAggregate) {
            AggregateDeviceHelper::destroy();
            juce::Thread::sleep(150);
        }
        mSuppressRunLoop.store(false);
#endif
    } else {
        // Headless: no real device to configure; use failure hook for testing.
        // Mirrors the real-device input-fallback above so the same code path
        // can be exercised by unit tests via the testSwapFailure hook.
        if (testSwapFailure) {
            const bool inputRequested = !inputDeviceName.empty();
            errStr = testSwapFailure(inputRequested);
            if (!errStr.empty() && inputRequested
                && errStr.find("input device") != std::string::npos)
            {
                const std::string firstError = errStr;
                std::string retryErr = testSwapFailure(false);
                if (retryErr.empty()) {
                    errStr.clear();
                    result.inputUnavailable = true;
                    result.inputUnavailableReason = firstError;
                } else {
                    errStr = retryErr;
                }
            }
        }
    }

    if (!errStr.empty()) {
        if (isCold) { rebuild_world(currentRate); mWorldRebuilt = true; }
        // --- Restart audio (failure path) ---
        if (mDeviceManager) {
            // Restore the previous device setup. A failed setAudioDeviceSetup
            // typically leaves the manager with no bound device. If we can't
            // restore (and the default-device fallback below also fails),
            // startAudioSource() sees no current device and brings up the
            // headless driver so the engine stays responsive (Spider /done
            // syncs return) instead of silently dead with the audio thread
            // never ticking.
            fprintf(stderr,
                    "[audio-device] swap failed (%s), restoring previous setup: out='%s' in='%s' sr=%.0f buf=%d\n",
                    errStr.c_str(),
                    prevSetup.outputDeviceName.toRawUTF8(),
                    prevSetup.inputDeviceName.toRawUTF8(),
                    prevSetup.sampleRate, prevSetup.bufferSize);
            fflush(stderr);
            juce::String restoreErr = mDeviceManager->setAudioDeviceSetup(prevSetup, true);
            if (restoreErr.isNotEmpty()) {
                fprintf(stderr,
                        "[audio-device] WARNING: failed to restore previous setup: %s\n",
                        restoreErr.toRawUTF8());
                fflush(stderr);
                // Last-resort recovery: try the system default with output-only.
                // If even this fails, startAudioSource() will choose the headless
                // fallback (no current device, so Headless) and the engine
                // stays up; the next user-driven swap can take it from there.
                mLastSelfTriggeredChange = std::chrono::steady_clock::now();
                juce::String fallbackErr =
                    mDeviceManager->initialiseWithDefaultDevices(0, mCurrentConfig.numOutputChannels);
                if (fallbackErr.isNotEmpty()) {
                    fprintf(stderr,
                            "[audio-device] WARNING: default-device fallback also failed: %s, "
                            "engine will run via headless driver\n",
                            fallbackErr.toRawUTF8());
                    fflush(stderr);
                } else {
                    fprintf(stderr,
                            "[audio-device] recovered to system default after rollback failure\n");
                    fflush(stderr);
                    // Clear aggregate-bookkeeping; we're on a single device now.
                    mRealOutputDeviceName.clear();
                    mRealInputDeviceName.clear();
                    // Drop input — fallback is output-only. Caller can
                    // re-enable inputs explicitly afterward.
                    mCurrentConfig.numInputChannels = 0;
                    uint32_t* opts = reinterpret_cast<uint32_t*>(
                        ring_buffer_storage + WORLD_OPTIONS_START);
                    opts[sonicpi::WorldOpts::kNumInputBusChannels] = 0;
                }
            }
        }
        startAudioSource();
        mAudioCallback.resume();
        if (isCold) mSampleLoader.resumeLoading();
        result.error = errStr;
        if (isCold) setEngineState(EngineState::Running, "swap-failed-rollback");
        if (onSwapEvent) onSwapEvent("swap:failed", result);
        return result;
    }

    if (isCold) {
        double newRate = (sampleRate > 0) ? sampleRate : currentRate;
        if (mDeviceManager) {
            auto* newDev = mDeviceManager->getCurrentAudioDevice();
            newRate = newDev ? newDev->getCurrentSampleRate() : newRate;
        }
        mCurrentConfig.sampleRate = static_cast<int>(newRate);

        uint32_t* opts = reinterpret_cast<uint32_t*>(ring_buffer_storage + WORLD_OPTIONS_START);
        opts[sonicpi::WorldOpts::kSampleRate] = static_cast<uint32_t>(newRate);

        // Update the world's input/output bus counts to match the new
        // device. rebuild_world() reads these to size the scsynth
        // World's audio buses; without the update the rebuilt World
        // stays at the boot-time channel count, so Out.ar to higher
        // buses (e.g. Out.ar(2, sig) with 4-channel Loopback) lands on
        // internal private buses instead of hardware.
        if (mDeviceManager) {
            if (auto* dev = mDeviceManager->getCurrentAudioDevice()) {
                int newOut = dev->getActiveOutputChannels().countNumberOfSetBits();
                int newIn  = dev->getActiveInputChannels().countNumberOfSetBits();
                if (newOut > 0) {
                    mCurrentConfig.numOutputChannels = newOut;
                    opts[sonicpi::WorldOpts::kNumOutputBusChannels]
                        = static_cast<uint32_t>(newOut);
                }
                // Respect inputWasDropped: when we've dropped input because
                // the new output can't be aggregated (wireless / virtual),
                // keep the previously-remembered input count in config but
                // tell the world there are zero inputs for this rebuild.
                if (!inputWasDropped || newIn > 0) {
                    mCurrentConfig.numInputChannels = newIn;
                }
                opts[sonicpi::WorldOpts::kNumInputBusChannels]
                    = static_cast<uint32_t>(inputWasDropped ? 0 : newIn);
            }
        }

        try {
            if (testRebuildFailure) {
                std::string failMsg = testRebuildFailure();
                if (!failMsg.empty())
                    throw std::runtime_error(failMsg);
            }
            rebuild_world(newRate);
            mWorldRebuilt = true;
        } catch (const std::exception& e) {
            fprintf(stderr, "[supersonic] rebuild_world failed: %s — recovering with safe defaults\n",
                    e.what());
            fflush(stderr);

            double safeRate = currentRate;
            int safeBuffer = 128;
            opts[sonicpi::WorldOpts::kSampleRate] = static_cast<uint32_t>(safeRate);
            mCurrentConfig.sampleRate = static_cast<int>(safeRate);
            mCurrentConfig.bufferSize = safeBuffer;

            try {
                rebuild_world(safeRate);
                mWorldRebuilt = true;
                recovered = true;
                result.error = std::string("rebuild failed (") + e.what()
                             + "), recovered at safe defaults";
                result.sampleRate = safeRate;
                result.bufferSize = safeBuffer;
            } catch (const std::exception& e2) {
                fprintf(stderr, "[supersonic] rebuild recovery ALSO failed: %s\n", e2.what());
                fflush(stderr);
                result.error = std::string("rebuild failed and recovery failed: ") + e2.what();
                setEngineState(EngineState::Error, "rebuild-failed");
                if (onSwapEvent) onSwapEvent("swap:failed", result);
                return result;
            }
        }
    }

    // --- Restart audio (success path) ---
    startAudioSource();
    mAudioCallback.resume();

    if (isCold) {
        // Don't restore synthdefs, buffers, or module state here.
        // The client (Spider) receives /supersonic/setup and handles
        // all reinitialisation — reloading synthdefs, clearing sample
        // caches, recreating groups/mixer/scope.  Restoring from the
        // StateCache would create duplicate state and cause distortion.
        mSampleLoader.resumeLoading();
    }

    if (mDeviceManager) {
        auto* finalDev = mDeviceManager->getCurrentAudioDevice();
        if (finalDev) {
            result.sampleRate = finalDev->getCurrentSampleRate();
            result.bufferSize = finalDev->getCurrentBufferSizeSamples();
            mCurrentConfig.sampleRate = static_cast<int>(result.sampleRate);
            mCurrentConfig.numOutputChannels = finalDev->getActiveOutputChannels().countNumberOfSetBits();
            // Preserve the user's desired input channel count when we had to
            // drop inputs for an unsuitable output (wireless/virtual). Without
            // this, a detour through e.g. AirPlay would permanently erase the
            // mic setting — switching back to speakers wouldn't re-aggregate.
            int actualIn = finalDev->getActiveInputChannels().countNumberOfSetBits();
            if (!inputWasDropped || actualIn > 0) {
                mCurrentConfig.numInputChannels = actualIn;
            }

            juce::AudioDeviceManager::AudioDeviceSetup finalSetup;
            mDeviceManager->getAudioDeviceSetup(finalSetup);
            result.inputDeviceName = finalSetup.inputDeviceName.toStdString();

            fprintf(stderr, "[audio-device] switched to %s: %s %.0fHz buf=%d %dch\n",
                    finalDev->getTypeName().toRawUTF8(),
                    finalDev->getName().toRawUTF8(),
                    result.sampleRate, result.bufferSize,
                    mCurrentConfig.numOutputChannels);

#ifdef __APPLE__
            // Remember the rate of the last non-wireless settle so a
            // future detour through AirPlay/Bluetooth doesn't leave the
            // engine stuck at the wireless receiver's negotiated rate.
            if (result.sampleRate > 0) {
                bool finalIsWireless = false;
                std::string finalName = mRealOutputDeviceName.empty()
                    ? finalDev->getName().toStdString()
                    : mRealOutputDeviceName;
                for (auto& d : listDevices(false)) {
                    if (deviceNameMatches(d.name, finalName) && d.isWirelessTransport()) {
                        finalIsWireless = true;
                        break;
                    }
                }
                if (!finalIsWireless)
                    mPreWirelessRate = static_cast<int>(result.sampleRate);
            }
#endif
        }
    } else {
        if (!recovered) {
            result.sampleRate = isCold ? sampleRate : currentRate;
        }
        result.bufferSize = mCurrentConfig.bufferSize;
    }
    result.success = true;
    recordSwapPreferences(deviceName, inputDeviceName, result.sampleRate);
    if (isCold) {
        if (recovered) {
            setEngineState(EngineState::Running, "swap-recovered");
            if (onSwapEvent) onSwapEvent("swap:recovered", result);
        } else {
            setEngineState(EngineState::Running, "rate-change");
            if (onSwapEvent) onSwapEvent("swap:complete", result);
        }
    } else {
        if (onSwapEvent) onSwapEvent("swap:complete", result);
    }
    fprintf(stderr, "[switchDevice] EXIT success=%d type=%s sr=%.0f buf=%d out=%d in=%d err='%s'\n",
            result.success ? 1 : 0,
            (result.type == SwapType::Cold) ? "Cold" : "Hot",
            result.sampleRate, result.bufferSize,
            mCurrentConfig.numOutputChannels, mCurrentConfig.numInputChannels,
            result.error.c_str());
    fflush(stderr);
    printDeviceList();
    return result;
}

SwapResult SupersonicEngine::reopenCurrentDevice() {
    SwapResult result;

    if (!mDeviceManager) {
        result.error = "no audio device manager (headless)";
        return result;
    }

    // Always delegate to switchDevice with forceCold=true so aggregates
    // get rebuilt with the same sub-device pair (and pick up any fresh
    // channel-count change). System / manual / aggregate all share this
    // path — mRealOutputDeviceName / mRealInputDeviceName track sub-
    // device names behind an aggregate and are the right thing to pass
    // whether we're in system mode or pinned to an explicit selection.
    // For direct (non-aggregate) devices those fields are empty and we
    // fall back to the JUCE device name.
    std::string outName = mRealOutputDeviceName;
    std::string inName  = mRealInputDeviceName;
    if (outName.empty()) {
        if (auto* dev = mDeviceManager->getCurrentAudioDevice())
            outName = dev->getName().toStdString();
    }
    if (outName.empty()) {
        result.error = "no current output device to reopen";
        return result;
    }
    fprintf(stderr, "[reopen] forceCold switch out='%s' in='%s' mode='%s'\n",
            outName.c_str(), inName.c_str(),
            mDeviceMode.empty() ? "system" : mDeviceMode.c_str());
    fflush(stderr);
    return switchDevice(outName, 0, 0, /*forceCold=*/true, inName);
}

// --- Input channel management ---

SwapResult SupersonicEngine::enableInputChannels(int numChannels) {
    // -1 means "re-enable inputs". Resolve to a concrete count:
    //   * mBootInputChannels > 0: user asked for an explicit count at boot
    //   * mBootInputChannels < 0: boot requested auto-max — ask JUCE for
    //     kRequestMaxChannels; CoreAudio clamps to the device's real count
    //   * mBootInputChannels == 0: boot explicitly disabled inputs — default
    //     to stereo when the user enables them later
    if (numChannels < 0) {
        static constexpr int kDefaultInputChannels = 2;
        if (mBootInputChannels > 0) {
            numChannels = mBootInputChannels;
        } else if (mBootInputChannels < 0) {
            numChannels = kRequestMaxChannels;
        } else {
            numChannels = kDefaultInputChannels;
        }
    }

    // Check if this is actually a change
    if (numChannels == mCurrentConfig.numInputChannels) {
        SwapResult result;
        result.success = true;
        result.type = SwapType::Hot;  // no-op
        result.sampleRate = mCurrentConfig.sampleRate;
        result.bufferSize = mCurrentConfig.bufferSize;
        return result;
    }

    // Refuse "disable inputs" on ASIO. ASIO drivers are full-duplex
    // single-device by spec — one stream owns both directions.
    // Reconfiguring with input=0 while keeping output crashes real
    // drivers (MOTU Pro Audio observed). Output-only is served by
    // switching driver to Windows Audio / DirectSound.
    if (numChannels == 0 && mDeviceManager) {
        if (auto* dev = mDeviceManager->getCurrentAudioDevice()) {
            if (dev->getTypeName().toStdString() == "ASIO") {
                SwapResult result;
                result.error = "Cannot disable input on ASIO — ASIO drivers "
                               "are full-duplex by spec. Switch driver to "
                               "Windows Audio / DirectSound to run output-only.";
                fprintf(stderr, "[enable-inputs] refusing disable on ASIO "
                        "(would crash the driver)\n");
                fflush(stderr);
                return result;
            }
        }
    }

    // Save old values for rollback on failure
    int oldNumInputChannels = mCurrentConfig.numInputChannels;
    uint32_t* opts = reinterpret_cast<uint32_t*>(ring_buffer_storage + WORLD_OPTIONS_START);
    uint32_t oldNumInputBusChannelsOpt =
        opts[sonicpi::WorldOpts::kNumInputBusChannels];

    // Update config and worldOptions before the cold swap
    mCurrentConfig.numInputChannels = numChannels;
    opts[sonicpi::WorldOpts::kNumInputBusChannels] = static_cast<uint32_t>(numChannels);

    // When disabling (numChannels == 0) we pass __none__ so switchDevice
    // tears down the input path. When enabling, we resolve an explicit
    // input device name rather than letting switchDevice fall back to
    // "first in JUCE's input list" — that fallback can pick a virtual
    // device (e.g. NDI Audio) over the real hardware mic, producing silent
    // zeros. Prefer, in order: saved mLastInputDeviceName, the macOS system
    // default input, then an empty string (switchDevice falls back).
    std::string inputName;
    const char* inputSource = "disable";
    if (numChannels > 0) {
        inputName = mLastInputDeviceName;
        inputSource = inputName.empty() ? "none" : "mLastInputDeviceName";
    }
#ifdef __APPLE__
    if (numChannels > 0 && inputName.empty()) {
        AudioObjectPropertyAddress addr = {
            kAudioHardwarePropertyDefaultInputDevice,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };
        AudioDeviceID devId = 0;
        UInt32 sz = sizeof(devId);
        if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, nullptr, &sz, &devId) == noErr
            && devId != 0) {
            CFStringRef cfName = nullptr;
            UInt32 nsz = sizeof(cfName);
            AudioObjectPropertyAddress nameAddr = {
                kAudioDevicePropertyDeviceNameCFString,
                kAudioObjectPropertyScopeGlobal,
                kAudioObjectPropertyElementMain
            };
            if (AudioObjectGetPropertyData(devId, &nameAddr, 0, nullptr, &nsz, &cfName) == noErr && cfName) {
                char buf[256];
                CFStringGetCString(cfName, buf, sizeof(buf), kCFStringEncodingUTF8);
                CFRelease(cfName);
                inputName = buf;
                inputSource = "kAudioHardwarePropertyDefaultInputDevice";
            }
        }
    }
#endif

    fprintf(stderr, "[enable-inputs] resolved input='%s' (source=%s) channels=%d\n",
            inputName.c_str(), inputSource, numChannels);
    fflush(stderr);
    // For disable, pass __none__ sentinel so switchDevice takes the disable
    // path (clears setup.inputDeviceName + inputChannels) instead of trying
    // to treat an empty string as "re-enable with last known input".
    auto result = switchDevice("", 0, 0, true, numChannels > 0 ? inputName : std::string("__none__"));

    if (!result.success) {
        mCurrentConfig.numInputChannels = oldNumInputChannels;
        opts[sonicpi::WorldOpts::kNumInputBusChannels] = oldNumInputBusChannelsOpt;
    }

    return result;
}

// --- Audio driver management ---

std::vector<std::string> SupersonicEngine::listDrivers() const {
    if (!mDeviceManager) return {};

    // Cache hit — skip the rescan. sendDeviceReport() is called many
    // times during boot (notify registration, first info push, device
    // change settles, aggregate build) and once per user-initiated
    // switch; re-running scanForDevices() on every call is wasteful
    // and on Linux without a JACK server produces libjack connect()
    // stderr spam. Short TTL so a freshly-started jackd shows up.
    {
        std::lock_guard<std::mutex> lk(mListDriversMutex);
        auto now = std::chrono::steady_clock::now();
        if (!mCachedDrivers.empty()
            && (now - mCachedDriversAt) < std::chrono::seconds(3))
            return mCachedDrivers;
    }

    std::vector<std::string> result;
    auto& types = mDeviceManager->getAvailableDeviceTypes();
    for (auto* type : types) {
        // JUCE registers every compiled-in device type regardless of
        // runtime availability (e.g. JackAudioIODeviceType appears in
        // getAvailableDeviceTypes() whenever JUCE_JACK=1, even if no
        // jackd / pipewire-jack server is running). Offering a driver
        // the user can't switch to is worse than hiding it, so rescan
        // and only advertise drivers that enumerate at least one output
        // device right now. ALSA / CoreAudio / WASAPI always have the
        // hardware on their side so they pass; JACK / ASIO only show
        // when a server or driver is actually reachable.
        type->scanForDevices();
        if (type->getDeviceNames(false).isEmpty()) continue;
        result.push_back(type->getTypeName().toStdString());
    }

    {
        std::lock_guard<std::mutex> lk(mListDriversMutex);
        mCachedDrivers = result;
        mCachedDriversAt = std::chrono::steady_clock::now();
    }
    return result;
}

std::string SupersonicEngine::currentDriver() const {
    if (!mDeviceManager) return "";
    if (auto* dev = mDeviceManager->getCurrentAudioDevice())
        return dev->getTypeName().toStdString();
    // No device open: fall back to the active type so the GUI's
    // driver dropdown stays on the right entry instead of going
    // blank. The type can be set without a device during cross-
    // driver swaps and after open failures.
    return mDeviceManager->getCurrentAudioDeviceType().toStdString();
}

std::string SupersonicEngine::intendedDriver() const {
    return mIntendedDriver;
}

bool SupersonicEngine::isInputKnownBadFor(const std::string& outputName,
                                          const std::string& inputName) const {
    if (outputName.empty() || inputName.empty()) return false;
    std::lock_guard<std::mutex> lock(mUngatableInputPairsMutex);
    return mUngatableInputPairs.count({outputName, inputName}) > 0;
}

SwapResult SupersonicEngine::switchDriver(const std::string& driverName) {
    SwapResult result;
    result.deviceName = driverName;

    // ── Real-driver path ────────────────────────────────────────────────
    // Always carry an explicit device name into setAudioDeviceSetup —
    // never let JUCE's insertDefaultDeviceNames pick alphabetical-first
    // for the new type. On Windows ASIO that's the registered-but-
    // unplugged-driver hang hazard (IASIO::init() can block in COM);
    // on every driver it's a quiet UX surprise (the device dropdown
    // shows one thing, the audio is routed through another).
    if (mDeviceManager) {
        // (a) Saved per-driver preference → delegate to switchDevice
        //     with the remembered name. switchDevice's cross-driver
        //     path moves JUCE atomically.
        auto pref = mPreferredDeviceByDriver.find(driverName);
        if (pref != mPreferredDeviceByDriver.end() && !pref->second.empty()) {
            fprintf(stderr, "[audio-device] switchDriver('%s'): delegating to "
                    "switchDevice('%s') (saved preference)\n",
                    driverName.c_str(), pref->second.c_str());
            fflush(stderr);
            mIntendedDriver = driverName;
            return switchDevice(pref->second);
        }

        // (b) No saved preference, non-ASIO driver with at least one
        //     device visible → pick the driver's system-default device
        //     and delegate. Keeps the transition atomic (one cold swap)
        //     and avoids leaving the GUI in a "driver=X but no device"
        //     limbo for drivers that have a sensible default.
        if (driverName != "ASIO") {
            auto& types = mDeviceManager->getAvailableDeviceTypes();
            for (auto* type : types) {
                if (type->getTypeName().toStdString() != driverName) continue;
                type->scanForDevices();
                auto names = type->getDeviceNames(false);
                if (names.isEmpty()) break;  // fall through to (c)
                int idx = type->getDefaultDeviceIndex(false);
                if (idx < 0 || idx >= names.size()) idx = 0;
                std::string defaultName = names[idx].toStdString();
                fprintf(stderr, "[audio-device] switchDriver('%s'): no saved pref, "
                        "auto-selecting default '%s'\n",
                        driverName.c_str(), defaultName.c_str());
                fflush(stderr);
                mIntendedDriver = driverName;
                return switchDevice(defaultName);
            }
        }

        // (c) ASIO with no saved preference, or any driver with no
        //     visible devices → don't touch JUCE. setCurrentAudio-
        //     DeviceType + initialiseWithDefaultDevices stops the
        //     audio callback, so scsynth stops ticking; any user-code
        //     call hitting trigger_synth before a follow-up
        //     switchDevice would hang on /n_go. Record intent and
        //     wait for the caller's explicit device pick.
        mIntendedDriver                 = driverName;
        result.success                  = true;
        result.requiresDeviceSelection  = true;
        fprintf(stderr, "[audio-device] switchDriver('%s'): intent recorded, "
                "no device opened — caller must follow with switchDevice\n",
                driverName.c_str());
        fflush(stderr);
        if (onSwapEvent) onSwapEvent("swap:complete", result);
        return result;
    }

    // ── Headless mode ───────────────────────────────────────────────────
    // No real audio driver to switch. If the test hook is set, simulate
    // the rate the new driver's default device would report.
    if (!testDriverSwitchRate) {
        result.error = "no audio device in headless mode";
        return result;
    }
    double newRate = testDriverSwitchRate();
    if (static_cast<int>(newRate) == mCurrentConfig.sampleRate) {
        result.success    = true;
        result.type       = SwapType::Hot;
        result.sampleRate = newRate;
        result.bufferSize = mCurrentConfig.bufferSize;
        if (onSwapEvent) onSwapEvent("swap:start", result);
        if (onSwapEvent) onSwapEvent("swap:complete", result);
        return result;
    }
    return switchDevice("", newRate);
}

// --- Device change detection ---

void SupersonicEngine::changeListenerCallback(juce::ChangeBroadcaster* source) {
    if (source != mDeviceManager.get()) return;
    if (!mRunning.load()) return;

    auto elapsed = std::chrono::steady_clock::now() - mLastSelfTriggeredChange;
    if (elapsed < std::chrono::seconds(1)) return;
    if (!mSwapMutex.try_lock()) {
        DEV_LOG("[hotplug] changeListenerCallback skipped — swap in progress\n");
        return;
    }

    mLastSelfTriggeredChange = std::chrono::steady_clock::now();

    // Collected hot-plug work to schedule after the mutex is released.
    std::string pendingSwitchOutput;
    std::string pendingSwitchInput;
    bool schedulePreferredReattach = false;
    bool scheduleInputReattach = false;

    {
        std::lock_guard<std::mutex> guard(mSwapMutex, std::adopt_lock);

        auto devices = listDevices(true);
        auto* dev = mDeviceManager->getCurrentAudioDevice();
        std::string currentOutput = mRealOutputDeviceName.empty()
            ? (dev ? dev->getName().toStdString() : "")
            : mRealOutputDeviceName;
        int currentActiveIn = dev ? dev->getActiveInputChannels().countNumberOfSetBits() : 0;

        std::vector<std::string> visibleNames;
        visibleNames.reserve(devices.size());
        for (auto& d : devices) visibleNames.push_back(d.name);

        auto decision = sonicpi::device::decideHotplugAction(
            mPreferredOutputDevice, mPreferredInputDevice,
            currentOutput, currentActiveIn, visibleNames);

        schedulePreferredReattach = decision.switchOutput;
        scheduleInputReattach     = decision.switchInput;
        pendingSwitchOutput       = decision.outputName;
        pendingSwitchInput        = decision.inputName;

        if (mDeviceMode.empty()) {
            // System mode: don't reinitialise here — that would destroy our
            // aggregate device (needed for input). The dedicated
            // handleSystemDefaultOutputChanged listener handles actual
            // default-output changes. Just update channel counts.
            if (dev) {
                mCurrentConfig.numOutputChannels =
                    dev->getActiveOutputChannels().countNumberOfSetBits();
                mCurrentConfig.numInputChannels =
                    dev->getActiveInputChannels().countNumberOfSetBits();
            }
        }

        printDeviceList();
    }

    // Schedule switchDevice async so it can take mSwapMutex itself.
    if (schedulePreferredReattach) {
        std::string outName = pendingSwitchOutput;
        std::string inName  = pendingSwitchInput;
        fprintf(stderr, "[hotplug] preferred output '%s' returned — scheduling switch "
                "(preferred input='%s')\n", outName.c_str(), inName.c_str());
        fflush(stderr);
        juce::MessageManager::callAsync([this, outName, inName]() {
            switchDevice(outName, 0, 0, false, inName);
        });
    } else if (scheduleInputReattach) {
        std::string inName = pendingSwitchInput;
        fprintf(stderr, "[hotplug] preferred input '%s' returned — scheduling input re-attach\n",
                inName.c_str());
        fflush(stderr);
        juce::MessageManager::callAsync([this, inName]() {
            switchDevice("", 0, 0, false, inName);
        });
    }

    mLastSelfTriggeredChange = std::chrono::steady_clock::now();
}

#ifdef __APPLE__
OSStatus SupersonicEngine::defaultDevicePropertyListenerProc(
    AudioObjectID, UInt32, const AudioObjectPropertyAddress*, void* inClientData)
{
    fprintf(stderr, "[default-output-listener] fired\n"); fflush(stderr);
    auto* self = static_cast<SupersonicEngine*>(inClientData);
    juce::MessageManager::callAsync([self]() {
        fprintf(stderr, "[default-output-listener] dispatched to handler\n"); fflush(stderr);
        self->handleSystemDefaultOutputChanged();
    });
    return noErr;
}

void SupersonicEngine::handleSystemDefaultOutputChanged() {
    if (!mDeviceMode.empty()) {
        fprintf(stderr, "[default-output-handler] bail: mDeviceMode='%s' (not empty — not in system mode)\n",
                mDeviceMode.c_str()); fflush(stderr);
        return;
    }
    if (!mRunning.load()) {
        fprintf(stderr, "[default-output-handler] bail: not running\n"); fflush(stderr);
        return;
    }
    if (!mDeviceManager) return;
    auto elapsed = std::chrono::steady_clock::now() - mLastSelfTriggeredChange;
    if (elapsed < std::chrono::seconds(2)) {
        fprintf(stderr, "[default-output-handler] bail: %lld ms since last self-triggered change (< 2 s)\n",
                (long long)std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
        fflush(stderr);
        return;
    }

    // Read the new macOS system-default output device.
    AudioDeviceID defaultID = kAudioObjectUnknown;
    AudioObjectPropertyAddress addr = {
        kAudioHardwarePropertyDefaultOutputDevice,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    UInt32 sz = sizeof(defaultID);
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, nullptr, &sz, &defaultID) != noErr
        || defaultID == kAudioObjectUnknown) {
        return;
    }
    CFStringRef nameCF = nullptr;
    UInt32 nameSz = sizeof(nameCF);
    AudioObjectPropertyAddress nameAddr = {
        kAudioDevicePropertyDeviceNameCFString,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    if (AudioObjectGetPropertyData(defaultID, &nameAddr, 0, nullptr, &nameSz, &nameCF) != noErr
        || !nameCF) {
        return;
    }
    char buf[256];
    CFStringGetCString(nameCF, buf, sizeof(buf), kCFStringEncodingUTF8);
    CFRelease(nameCF);
    std::string newDefault(buf);

    // Ignore if CoreAudio reports one of our own aggregates as the new
    // default — creating the aggregate can briefly elevate it to default,
    // and treating that as a "change" would trigger a nested aggregation.
    if (newDefault.compare(0, 10, "SuperSonic") == 0) return;

    // If we're on an aggregate, compare the new default against the real
    // (underlying) output we're aggregating, not the aggregate's own name.
    std::string currentOutput = mRealOutputDeviceName.empty()
        ? (mDeviceManager->getCurrentAudioDevice()
           ? mDeviceManager->getCurrentAudioDevice()->getName().toStdString() : "")
        : mRealOutputDeviceName;
    if (newDefault == currentOutput) return;

    fprintf(stderr, "[audio-device] system default output changed: '%s' -> '%s'\n",
            currentOutput.c_str(), newDefault.c_str());
    fflush(stderr);
    // Route through setDeviceMode("") so we get the wireless/non-wireless
    // branching: non-wireless defaults go via switchDevice (aggregate
    // preserved); wireless defaults go via reinitialiseWithDefaults
    // (JUCE's default-device abstraction, which CoreAudio routes through
    // AirPlay correctly).
    setDeviceMode("");
}
#endif

std::string SupersonicEngine::setDeviceMode(const std::string& mode) {
    std::string previousMode = mDeviceMode;

    if (mode == "system" || mode.empty()) {
        mDeviceMode.clear();
        // Entering system mode means "follow macOS default" — user has
        // opted out of sticking to a specific hardware device, so drop
        // the hot-plug preference that would otherwise pull them back.
        mPreferredOutputDevice.clear();
    } else {
        mDeviceMode = mode;
        mPreferredOutputDevice = mode;
    }

    if (!mRunning.load()) return "";

    if (mDeviceMode.empty()) {
        // System mode — switch to the current macOS default output while
        // keeping the input device (mic) so live_audio follows the output.
        if (mDeviceManager) {
            fprintf(stderr, "[audio-device] switching to system default\n");
#ifdef __APPLE__
            AudioDeviceID defaultID = kAudioObjectUnknown;
            AudioObjectPropertyAddress addr = {
                kAudioHardwarePropertyDefaultOutputDevice,
                kAudioObjectPropertyScopeGlobal,
                kAudioObjectPropertyElementMain
            };
            UInt32 sz = sizeof(defaultID);
            std::string newDefault;
            if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, nullptr, &sz, &defaultID) == noErr
                && defaultID != kAudioObjectUnknown) {
                CFStringRef nameCF = nullptr;
                UInt32 nameSz = sizeof(nameCF);
                AudioObjectPropertyAddress nameAddr = {
                    kAudioDevicePropertyDeviceNameCFString,
                    kAudioObjectPropertyScopeGlobal,
                    kAudioObjectPropertyElementMain
                };
                if (AudioObjectGetPropertyData(defaultID, &nameAddr, 0, nullptr, &nameSz, &nameCF) == noErr && nameCF) {
                    char buf[256];
                    CFStringGetCString(nameCF, buf, sizeof(buf), kCFStringEncodingUTF8);
                    CFRelease(nameCF);
                    newDefault = buf;
                }
            }
            if (!newDefault.empty()) {
                // Branch based on the new default's transport type:
                //
                //  * Wireless (AirPlay/Bluetooth): go through
                //    reinitialiseWithDefaultsPreservingConfig. JUCE uses
                //    CoreAudio's default-output abstraction, which
                //    correctly routes through AirPlay. Opening AirPlay
                //    by explicit device name (via switchDevice) has
                //    never been reliable in our tests.
                //  * Non-wireless: go through switchDevice so we
                //    preserve/restore the mic via aggregate.
                AudioObjectPropertyAddress tAddr = {
                    kAudioDevicePropertyTransportType,
                    kAudioObjectPropertyScopeGlobal,
                    kAudioObjectPropertyElementMain
                };
                UInt32 tType = 0, tSize = sizeof(tType);
                bool newIsWireless = false;
                if (AudioObjectGetPropertyData(defaultID, &tAddr, 0, nullptr, &tSize, &tType) == noErr) {
                    newIsWireless = CoreAudioTransport::isWireless(tType);
                }
                if (!newIsWireless) {
                    std::string inputName = mRealInputDeviceName;
                    if (inputName.empty() && mCurrentConfig.numInputChannels > 0) {
                        auto setup = mDeviceManager->getAudioDeviceSetup();
                        inputName = setup.inputDeviceName.toStdString();
                    }
                    auto result = switchDevice(newDefault, 0, 0, false, inputName);
                    if (!result.success) return result.error;
                    return {};
                }
                // Wireless default — fall through to the reinitialise
                // path below.
                fprintf(stderr, "[audio-device] system default '%s' is wireless; "
                        "using JUCE default-device init\n", newDefault.c_str());
                fflush(stderr);
            }
#endif
            auto err = reinitialiseWithDefaultsPreservingConfig();
            if (err.isNotEmpty()) {
                fprintf(stderr, "[audio-device] system mode init failed: %s\n",
                        err.toRawUTF8());
                return err.toStdString();
            }

            std::string newDevName;
            double newRate = 0.0;
            if (auto* dev = mDeviceManager->getCurrentAudioDevice()) {
                mCurrentConfig.numOutputChannels =
                    dev->getActiveOutputChannels().countNumberOfSetBits();
                mCurrentConfig.numInputChannels =
                    dev->getActiveInputChannels().countNumberOfSetBits();
                newRate = dev->getCurrentSampleRate();
                newDevName = dev->getName().toStdString();
            }

            if (newRate > 0 && static_cast<int>(newRate) != mCurrentConfig.sampleRate) {
                fprintf(stderr,
                        "[audio-device] system default has different rate "
                        "(%d -> %.0f Hz) — performing cold swap\n",
                        mCurrentConfig.sampleRate, newRate);
                // Force cold even though JUCE is already at newRate (we
                // just opened it via reinitialiseWithDefaultsPreservingConfig).
                // Without forceCold, switchDevice sees currentRate ==
                // sampleRate and skips the World rebuild, leaving scsynth
                // running at the old rate while JUCE delivers samples at
                // the new rate — mismatch, pitched-down audio.
                switchDevice(newDevName, newRate, 0, /*forceCold=*/true);
            } else {
                printDeviceList();
            }
        }
    } else {
        // Manual mode: switch to the named device.
        // setDeviceMode is called from the Sonic Pi GUI which cannot handle
        // cold swaps (world rebuild destroys the audio graph).  Pre-check
        // whether the target device supports the current sample rate and
        // reject if not — callers that can tolerate cold swaps should use
        // switchDevice() directly (via /supersonic/devices/switch).
        if (mDeviceManager) {
            auto* curDev = mDeviceManager->getCurrentAudioDevice();
            double curRate = curDev ? curDev->getCurrentSampleRate() : 0.0;
            if (curRate > 0) {
                auto rates = probeDeviceSampleRates(mDeviceMode, false);
                bool rateOk = false;
                for (auto r : rates)
                    if (static_cast<int>(r) == static_cast<int>(curRate))
                        rateOk = true;
                if (!rateOk) {
                    fprintf(stderr,
                        "[audio-device] rejecting mode switch to %s: "
                        "current rate %.0f not supported — restart required\n",
                        mDeviceMode.c_str(), curRate);
                    mDeviceMode = previousMode;
                    printDeviceList();
                    return "device requires different sample rate — restart required";
                }
            }
        }

        auto result = switchDevice(mDeviceMode);
        if (!result.success) {
            mDeviceMode = previousMode;  // revert mode on failure
            printDeviceList();
            return result.error;
        }
    }

    printDeviceList();
    return "";
}

void SupersonicEngine::printDeviceList() {
    if (!mDeviceManager) return;

    // Skip rescan — calling scanForDevices() right after a device switch
    // can close the just-opened CoreAudio device. The switch path already
    // rescanned when needed. This path only reports state.
    auto devices = listDevices(false);
    auto current = currentDevice();

    fprintf(stderr, "[audio-devices-start]\n");
    for (auto& dev : devices) {
        char tt[5] = {};
        if (dev.transportType) {
            tt[0] = (char)((dev.transportType >> 24) & 0xFF);
            tt[1] = (char)((dev.transportType >> 16) & 0xFF);
            tt[2] = (char)((dev.transportType >> 8) & 0xFF);
            tt[3] = (char)(dev.transportType & 0xFF);
        }
        fprintf(stderr, "[audio-device-entry] %s|%s|%d|%d|%s\n",
                dev.name.c_str(), dev.typeName.c_str(),
                dev.maxOutputChannels, dev.maxInputChannels,
                dev.transportType ? tt : "?");
    }
    fprintf(stderr, "[audio-device-current] %s|%s|%.0f|%d|%d|%d\n",
            current.name.c_str(), current.typeName.c_str(),
            current.activeSampleRate, current.activeBufferSize,
            current.activeOutputChannels, current.activeInputChannels);
    fprintf(stderr, "[audio-device-mode] %s\n",
            mDeviceMode.empty() ? "system" : mDeviceMode.c_str());
    fprintf(stderr, "[audio-devices-end]\n");
    fflush(stderr);

    // Also push device info via OSC to registered GUI listener
    mUdpServer.sendDeviceReport();
}

// --- Recording ---

SupersonicEngine::RecordResult SupersonicEngine::startRecording(
    const std::string& path, const std::string& format, int bitDepth) {
    RecordResult result;
    result.path = path;

    if (isRecording()) {
        result.error = "already recording";
        return result;
    }

    auto* dev = mDeviceManager ? mDeviceManager->getCurrentAudioDevice() : nullptr;
    double sampleRate = dev ? dev->getCurrentSampleRate()
                            : static_cast<double>(mCurrentConfig.sampleRate);
    // Use the actual device output channel count, not the scsynth internal bus count.
    // ThreadedWriter::write() receives JUCE's outputChannelData which has device channels.
    int numChannels = dev ? dev->getActiveOutputChannels().countNumberOfSetBits()
                          : mCurrentConfig.numOutputChannels;

    juce::File file{juce::String(path)};
    file.getParentDirectory().createDirectory();

    auto outputStream = std::make_unique<juce::FileOutputStream>(file);
    if (outputStream->failedToOpen()) {
        result.error = "failed to open file: " + path;
        return result;
    }

    // Create format-specific writer
    std::unique_ptr<juce::AudioFormat> audioFormat;
    if (format == "flac")
        audioFormat = std::make_unique<juce::FlacAudioFormat>();
    else
        audioFormat = std::make_unique<juce::WavAudioFormat>();

    juce::AudioFormatWriter* formatWriter = audioFormat->createWriterFor(
        outputStream.get(), sampleRate,
        static_cast<unsigned int>(numChannels),
        bitDepth, {}, 0);

    if (!formatWriter) {
        result.error = "unsupported format/bitDepth: " + format + "/" + std::to_string(bitDepth);
        return result;
    }

    // Writer takes ownership of the stream
    outputStream.release();

    if (!mRecordThread.isThreadRunning())
        mRecordThread.startThread();

    auto* threadedWriter = new juce::AudioFormatWriter::ThreadedWriter(
        formatWriter, mRecordThread, static_cast<int>(sampleRate) * 10);

    mAudioCallback.mRecordWriter.store(threadedWriter, std::memory_order_release);
    mRecordPath = path;

    result.success = true;
    fprintf(stderr, "[recording] started: %s (%s, %dbit, %.0fHz, %dch)\n",
            path.c_str(), format.c_str(), bitDepth, sampleRate, numChannels);
    return result;
}

SupersonicEngine::RecordResult SupersonicEngine::stopRecording() {
    RecordResult result;
    result.path = mRecordPath;

    if (!isRecording()) {
        result.error = "not recording";
        return result;
    }

    // Pause audio to ensure no in-flight write() calls
    mAudioCallback.pause();
    auto* old = static_cast<juce::AudioFormatWriter::ThreadedWriter*>(
        mAudioCallback.mRecordWriter.exchange(nullptr, std::memory_order_acq_rel));
    mAudioCallback.resume();

    // Delete flushes remaining data and closes the file
    delete old;

    result.success = true;
    fprintf(stderr, "[recording] stopped: %s\n", mRecordPath.c_str());
    mRecordPath.clear();
    return result;
}

bool SupersonicEngine::isRecording() const {
    return mAudioCallback.mRecordWriter.load(std::memory_order_acquire) != nullptr;
}

// --- Purge ---

void SupersonicEngine::purge() {
    uint8_t* base = ring_buffer_storage;
    ControlPointers* ctrl = reinterpret_cast<ControlPointers*>(base + CONTROL_START);

    // Reset IN ring buffer
    ctrl->in_head.store(0, std::memory_order_release);
    ctrl->in_tail.store(0, std::memory_order_release);

    // Cancel prescheduler events
    mPrescheduler.cancelAll();

    // Clear scsynth BundleScheduler
    clear_scheduler();
}

// --- Clock offset ---

void SupersonicEngine::setClockOffset(double offsetSeconds) {
    auto* globalOffset = reinterpret_cast<std::atomic<int32_t>*>(
        ring_buffer_storage + GLOBAL_OFFSET_START);
    globalOffset->store(static_cast<int32_t>(offsetSeconds * 1000.0),
                        std::memory_order_relaxed);
}

double SupersonicEngine::getClockOffset() const {
    auto* globalOffset = reinterpret_cast<const std::atomic<int32_t>*>(
        ring_buffer_storage + GLOBAL_OFFSET_START);
    return globalOffset->load(std::memory_order_relaxed) / 1000.0;
}

/*
 * SupersonicEngine.cpp
 */
#include "SupersonicEngine.h"
#include "AggregateDeviceHelper.h"
#include "src/audio_processor.h"
#include "src/shared_memory.h"
#include "osc/OscReceivedElements.h"
#include "RingBufferWriter.h"
#include "scsynth/server/SC_Prototypes.h"  // zfree
#include <juce_audio_formats/juce_audio_formats.h>
#include "FuzzyMatch.h"
#include <chrono>
#include <cstring>
#include <thread>
#ifdef __APPLE__
#include <CoreAudio/CoreAudio.h>
#endif

extern "C" {
    // Global used by init_memory() to pass external shared memory to World_New.
    // Declared extern "C" because init_memory() references it from an extern "C" block.
    void* g_external_shared_memory = nullptr;

    void destroy_world();
    void rebuild_world(double sample_rate);
}

SupersonicEngine::SupersonicEngine() = default;

void SupersonicEngine::setEngineState(EngineState state, const std::string& reason) {
    EngineState prev = mEngineState.exchange(state);
    if (prev == state) return;  // no transition

    const char* stateStr = engineStateToString(state);
    fprintf(stderr, "[supersonic] state: %s -> %s (%s)\n",
            engineStateToString(prev), stateStr,
            reason.empty() ? "-" : reason.c_str());
    fflush(stderr);

    mUdpServer.sendStateChange(stateStr, reason.c_str());

    // /supersonic/setup fires on transitions to Running (world is ready)
    if (state == EngineState::Running) {
        auto* dev = mDeviceManager ? mDeviceManager->getCurrentAudioDevice() : nullptr;
        int sr  = dev ? static_cast<int>(dev->getCurrentSampleRate()) : mCurrentConfig.sampleRate;
        int buf = dev ? dev->getCurrentBufferSizeSamples() : mCurrentConfig.bufferSize;
        mUdpServer.sendSetup(sr, buf);
    }
}

SupersonicEngine::~SupersonicEngine() {
    shutdown();
}


void SupersonicEngine::initialise(const Config& cfg) {
    if (mRunning.load()) return;
    setEngineState(EngineState::Booting, "init");

    mHeadless = cfg.headless;
    mCurrentConfig = cfg;
    mBootInputChannels = cfg.numInputChannels;

    // -- Wire callbacks ---------------------------------------------------
    // onReply/onDebug should be set before initialise() — worker threads
    // read them via captured `this` pointer without synchronisation.
    // Setting them after initialise() is a data race.
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
        // Tell CoreAudio to dispatch HAL property notifications on its own
        // internal threads instead of the main CFRunLoop.  Without this,
        // AirPlay device setup hangs because it needs CFRunLoop dispatch
        // to complete network negotiation — and we don't run a Cocoa event
        // loop (same pattern Ardour uses for non-Cocoa audio apps).
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

        // Open with requested channel counts; if that fails (e.g. device has
        // fewer channels), fall back to stereo out / no in, then zero/zero.
        juce::String initError = mDeviceManager->initialiseWithDefaultDevices(
            cfg.numInputChannels, cfg.numOutputChannels);
        if (initError.isNotEmpty()) {
            fprintf(stderr, "[audio-device] init with %d in / %d out failed: %s\n",
                    cfg.numInputChannels, cfg.numOutputChannels,
                    initError.toRawUTF8());
            initError = mDeviceManager->initialiseWithDefaultDevices(0, 2);
        }
        if (initError.isNotEmpty()) {
            fprintf(stderr, "[audio-device] init with 0 in / 2 out failed: %s\n",
                    initError.toRawUTF8());
            initError = mDeviceManager->initialiseWithDefaultDevices(0, 0);
        }
        if (initError.isNotEmpty()) {
            fprintf(stderr, "[audio-device] all init attempts failed: %s\n",
                    initError.toRawUTF8());
        }

        // If -H was specified, fuzzy-match against combined "Driver : Device"
        // strings — matching scsynth's PortAudio convention.  Each word in
        // the pattern is an independent substring filter, so:
        //   -H "speakers"           → any device with Speakers
        //   -H "direct"             → any DirectSound device
        //   -H "direct headphones"  → DirectSound + Headphones
        if (!cfg.hardwareDevice.empty()) {
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

            std::string matched = fuzzyMatch(cfg.hardwareDevice, combinedNames);
            if (matched.empty()) {
                fprintf(stderr, "[audio-device] -H '%s' not found, available:\n",
                        cfg.hardwareDevice.c_str());
                for (auto& e : entries)
                    fprintf(stderr, "    %s\n", e.combined.c_str());
            } else {
                // Find the matching entry
                for (auto& e : entries) {
                    if (e.combined != matched) continue;

                    // Switch driver if needed
                    auto* curType = mDeviceManager->getCurrentDeviceTypeObject();
                    if (!curType || curType->getTypeName().toStdString() != e.typeName) {
                        mDeviceManager->setCurrentAudioDeviceType(
                            juce::String(e.typeName), true);
                        mDeviceManager->initialiseWithDefaultDevices(
                            cfg.numInputChannels, cfg.numOutputChannels);
                    }

                    juce::AudioDeviceManager::AudioDeviceSetup setup;
                    mDeviceManager->getAudioDeviceSetup(setup);
                    setup.outputDeviceName = juce::String(e.devName);
                    setup.useDefaultOutputChannels = true;
                    setup.useDefaultInputChannels = true;
                    if (cfg.sampleRate > 0) setup.sampleRate = cfg.sampleRate;
                    juce::String hwErr = mDeviceManager->setAudioDeviceSetup(setup, true);
                    if (hwErr.isNotEmpty()) {
                        fprintf(stderr, "[audio-device] -H '%s' matched '%s' but failed: %s\n",
                                cfg.hardwareDevice.c_str(), e.combined.c_str(),
                                hwErr.toRawUTF8());
                        mDeviceManager->initialiseWithDefaultDevices(
                            cfg.numInputChannels, cfg.numOutputChannels);
                    } else {
                        fprintf(stderr, "  -H '%s' -> %s\n",
                                cfg.hardwareDevice.c_str(), e.combined.c_str());
                        mDeviceMode = e.devName;
                    }
                    break;
                }
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
                // User explicitly requested a buffer size — honour it
                setup.bufferSize = cfg.bufferSize;
                changed = true;
            } else if (dev->getTypeName() != "DirectSound") {
                // Auto: pick the smallest available buffer that is a multiple of 128.
                // DirectSound manages its own large circular buffer internally —
                // overriding its default causes distortion, so we leave it alone.
                constexpr int kBlockSize = 128;
                int minBuf = kBlockSize;
                auto sizes = dev->getAvailableBufferSizes();
                int best = 0;
                for (auto s : sizes) {
                    if (s >= minBuf && s % kBlockSize == 0) {
                        best = s;
                        break;  // sizes are sorted ascending — first match is smallest
                    }
                }
                if (best > 0 && best != dev->getCurrentBufferSizeSamples()) {
                    setup.bufferSize = best;
                    changed = true;
                    (void)kBlockSize; // used in the calculation above
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
                        cfg.numInputChannels, cfg.numOutputChannels);
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

    // -- Create shared memory (owned by engine, survives cold swaps) --------
    if (cfg.udpPort > 0) {
        server_shared_memory_creator::cleanup(cfg.udpPort);
        try {
            mShmemCreator = new server_shared_memory_creator(
                cfg.udpPort, cfg.numControlBusChannels);
            // Tell init_memory()/World_New to reuse this instead of creating its own
            g_external_shared_memory = mShmemCreator;
        } catch (const std::exception& e) {
            fprintf(stderr, "[supersonic] shared memory creation failed: %s\n", e.what());
            fflush(stderr);
            mShmemCreator = nullptr;
        }
    }

    // -- Initialise scsynth World ------------------------------------------
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
        cfg.udpPort   // sharedMemoryID — creates boost shm named "SuperColliderServer_<port>"
    );

    // Derive pointers into ring_buffer_storage for worker threads
    uint8_t* base = ring_buffer_storage;
    ControlPointers*    ctrl = reinterpret_cast<ControlPointers*>(base + CONTROL_START);
    PerformanceMetrics* met  = reinterpret_cast<PerformanceMetrics*>(base + METRICS_START);

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

    if (mDeviceManager) {
        // -- Register audio callback (triggers audioDeviceAboutToStart) ----
        mDeviceManager->addAudioCallback(&mAudioCallback);
        mDeviceManager->addChangeListener(this);
    } else {
        // -- Headless: start a timer thread to drive process_audio() ------
        mHeadlessDriver.configure(&mAudioCallback, &mSampleLoader,
                                   mCurrentConfig.sampleRate,
                                   mCurrentConfig.numOutputChannels,
                                   mCurrentConfig.numInputChannels);
        mHeadlessDriver.startThread(juce::Thread::Priority::highest);
    }

    // -- SampleLoader (background file I/O for /b_allocRead) ----------------
    // Wire to audio callback so installPendingBuffers() runs on the audio thread.
    // Replies go through the OUT ring buffer -> ReplyReader (matching WASM arch).
    mSampleLoader.initialise();
    mAudioCallback.setSampleLoader(&mSampleLoader);
    mAudioCallback.onWake = [this]() { purge(); };

    // -- Start worker threads ----------------------------------------------
    mPrescheduler.startThread(juce::Thread::Priority::normal);
    mReplyReader.startThread(juce::Thread::Priority::normal);
    mDebugReader.startThread(juce::Thread::Priority::low);
    mSampleLoader.startThread(juce::Thread::Priority::normal);
    if (mDeviceManager)
        mUdpServer.startThread(juce::Thread::Priority::normal);

    // Block until the audio callback fires at least once — ensures
    // sendOsc() calls made immediately after initialise() don't race
    // device startup (observed on macOS Intel CI).
    {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        uint32_t snapshot = mAudioCallback.processCount.load(std::memory_order_acquire);
        while (mAudioCallback.processCount.load(std::memory_order_acquire) == snapshot
               && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    mRunning.store(true);
    setEngineState(EngineState::Running, "boot");
}

void SupersonicEngine::shutdown() {
    if (!mRunning.exchange(false)) return;
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
    delete mShmemCreator;
    mShmemCreator = nullptr;
}

// --- OSC send with cache interception ---

void SupersonicEngine::sendOsc(const uint8_t* data, uint32_t size) {
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
    sendOsc(pkt.ptr(), pkt.size());
}

// --- Device management ---

std::vector<DeviceInfo> SupersonicEngine::listDevices() const {
    std::vector<DeviceInfo> result;
    if (!mDeviceManager) return result;

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
    auto lookupTransport = [&transportMap](const std::string& juceName) -> uint32_t {
        auto it = transportMap.find(juceName);
        if (it != transportMap.end()) return it->second;
        // JUCE appends suffixes to disambiguate duplicates — match by prefix
        for (auto& [caName, transport] : transportMap) {
            if (juceName.size() > caName.size()
                && juceName.compare(0, caName.size(), caName) == 0
                && juceName[caName.size()] == ' ') {
                return transport;
            }
        }
        return 0;
    };
#endif

    auto& types = mDeviceManager->getAvailableDeviceTypes();
    for (auto* type : types) {
        type->scanForDevices();

        auto populateFromDevice = [](DeviceInfo& info, juce::AudioIODevice* dev) {
            for (auto r : dev->getAvailableSampleRates())
                info.availableSampleRates.push_back(r);
            for (auto b : dev->getAvailableBufferSizes())
                info.availableBufferSizes.push_back(b);
            info.maxOutputChannels = dev->getOutputChannelNames().size();
            info.maxInputChannels  = dev->getInputChannelNames().size();
        };

        std::string typeNameStr = type->getTypeName().toStdString();

        // Enumerate output devices
        auto outputNames = type->getDeviceNames(false);
        for (auto& devName : outputNames) {
            DeviceInfo info;
            info.name = devName.toStdString();
            info.typeName = typeNameStr;
#ifdef __APPLE__
            info.transportType = lookupTransport(info.name);
#endif

            std::unique_ptr<juce::AudioIODevice> tempDev(
                type->createDevice(devName, juce::String()));
            if (tempDev)
                populateFromDevice(info, tempDev.get());

            result.push_back(std::move(info));
        }

        // Enumerate input-only devices (e.g. macOS microphones)
        auto inputNames = type->getDeviceNames(true);
        for (auto& devName : inputNames) {
            std::string nameStr = devName.toStdString();

            bool alreadyListed = false;
            for (auto& existing : result) {
                if (existing.name == nameStr && existing.typeName == typeNameStr) {
                    alreadyListed = true;
                    break;
                }
            }
            if (alreadyListed) continue;

            DeviceInfo info;
            info.name = std::move(nameStr);
            info.typeName = typeNameStr;
#ifdef __APPLE__
            info.transportType = lookupTransport(info.name);
#endif

            std::unique_ptr<juce::AudioIODevice> tempDev(
                type->createDevice(juce::String(), devName));
            if (tempDev)
                populateFromDevice(info, tempDev.get());

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

    return info;
}

void SupersonicEngine::restartHeadlessDriver(double sampleRate) {
    mHeadlessDriver.configure(&mAudioCallback, &mSampleLoader,
                               static_cast<int>(sampleRate),
                               mCurrentConfig.numOutputChannels,
                               mCurrentConfig.numInputChannels);
    mHeadlessDriver.startThread(juce::Thread::Priority::highest);
    // Wait for first audio tick after restart
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    uint32_t snapshot = mAudioCallback.processCount.load(std::memory_order_acquire);
    while (mAudioCallback.processCount.load(std::memory_order_acquire) == snapshot
           && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

juce::String SupersonicEngine::reinitialiseWithDefaultsPreservingConfig() {
    int prevRate = mCurrentConfig.sampleRate;
    auto prevSetup = mDeviceManager->getAudioDeviceSetup();
    int prevBufSize = prevSetup.bufferSize;

    auto err = mDeviceManager->initialiseWithDefaultDevices(0, 2);
    if (err.isNotEmpty()) {
        err = mDeviceManager->initialiseWithDefaultDevices(0, 0);
    }
    if (err.isNotEmpty()) return err;

    // Re-apply the previous sample rate and buffer size
    auto setup = mDeviceManager->getAudioDeviceSetup();
    setup.sampleRate = static_cast<double>(prevRate);
    setup.bufferSize = prevBufSize;
    mDeviceManager->setAudioDeviceSetup(setup, true);
    return {};
}

SwapResult SupersonicEngine::switchDevice(const std::string& deviceName,
                                           double sampleRate,
                                           int bufferSize,
                                           bool forceCold,
                                           const std::string& inputDeviceName) {
    SwapResult result;
    result.deviceName = deviceName;
    bool recovered = false;

    // Try to acquire swap mutex (non-blocking)
    if (!mSwapMutex.try_lock()) {
        result.error = "swap already in progress";
        return result;
    }
    std::lock_guard<std::mutex> guard(mSwapMutex, std::adopt_lock);

    // Determine current rate — from device if available, else from config
    double currentRate = 0.0;
    if (mDeviceManager) {
        auto* currentDev = mDeviceManager->getCurrentAudioDevice();
        currentRate = currentDev ? currentDev->getCurrentSampleRate() : 0.0;
    } else {
        currentRate = static_cast<double>(mCurrentConfig.sampleRate);
    }

    // When no explicit sample rate requested, probe the target device to see
    // if the current rate is supported.  If not, auto-select the first available
    // rate — this makes the swap a cold swap (world rebuild).
    auto probeDeviceRate = [&](const std::string& name, bool isInput) {
        if (name.empty() || sampleRate > 0 || currentRate <= 0 || !mDeviceManager) return;
        auto& types = mDeviceManager->getAvailableDeviceTypes();
        for (auto* type : types) {
            type->scanForDevices();
            auto names = type->getDeviceNames(isInput);
            for (auto& n : names) {
                auto juceStr = n.toStdString();
                // Match exact name or prefix (JUCE appends " (N)" suffixes
                // to disambiguate duplicate CoreAudio device names).
                bool match = (juceStr == name);
                if (!match && juceStr.size() > name.size()
                    && juceStr.compare(0, name.size(), name) == 0
                    && juceStr[name.size()] == ' ') {
                    match = true;
                }
                if (match) {
                    auto outArg = isInput ? juce::String() : n;
                    auto inArg  = isInput ? n : juce::String();
                    std::unique_ptr<juce::AudioIODevice> tempDev(
                        type->createDevice(outArg, inArg));
                    if (tempDev) {
                        auto rates = tempDev->getAvailableSampleRates();
                        bool supported = false;
                        for (auto r : rates)
                            if (static_cast<int>(r) == static_cast<int>(currentRate))
                                supported = true;
                        if (!supported && !rates.isEmpty()) {
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
                    }
                    return;
                }
            }
        }
    };

    probeDeviceRate(deviceName, false);
    probeDeviceRate(inputDeviceName, true);

    bool isCold = forceCold || (sampleRate > 0 && sampleRate != currentRate);
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
    if (mDeviceManager) {
        mDeviceManager->removeAudioCallback(&mAudioCallback);
    } else {
        mHeadlessDriver.signalThreadShouldExit();
        mHeadlessDriver.stopThread(2000);
    }

    if (isCold) destroy_world();

    // --- Apply new device configuration ---
    std::string errStr;
    if (mDeviceManager) {
        juce::AudioDeviceManager::AudioDeviceSetup setup;
        mDeviceManager->getAudioDeviceSetup(setup);

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

        if (!deviceName.empty())
            setup.outputDeviceName = juce::String(deviceName);
        if (mCurrentConfig.numInputChannels > 0) {
            if (!inputDeviceName.empty()) {
                // Explicit input device requested — save for future re-enable
                setup.useDefaultInputChannels = false;
                setup.inputDeviceName = juce::String(inputDeviceName);
                mLastInputDeviceName = inputDeviceName;
                juce::BigInteger inputBits;
                for (int i = 0; i < mCurrentConfig.numInputChannels; ++i)
                    inputBits.setBit(i);
                setup.inputChannels = inputBits;
            } else {
                // Re-enable inputs — must explicitly set the input device name.
                // useDefaultInputChannels won't auto-fill the device name when
                // numInputChansNeeded was 0 at initialise() time (boot with -i 0).
                setup.useDefaultInputChannels = false;
                juce::BigInteger inputBits;
                for (int i = 0; i < mCurrentConfig.numInputChannels; ++i)
                    inputBits.setBit(i);
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
        setup.useDefaultOutputChannels = true;
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

        if (needsAggregate) {
            // Check transport types — skip aggregate for Bluetooth/AirPlay
            // on either side (wireless codecs don't support drift correction)
            auto devices = listDevices();
            std::string outName = setup.outputDeviceName.toStdString();
            std::string inName  = setup.inputDeviceName.toStdString();
            for (auto& dev : devices) {
                if ((dev.name == outName || dev.name == inName)
                    && !dev.isSuitableForAggregate()) {
                    needsAggregate = false;
                    fprintf(stderr, "[audio-device] skipping aggregate — '%s' is Bluetooth/AirPlay\n",
                            dev.name.c_str());
                    break;
                }
            }
        }

        if (needsAggregate) {
            // Remember the real device names before replacing with aggregate
            mRealOutputDeviceName = setup.outputDeviceName.toStdString();
            mRealInputDeviceName  = setup.inputDeviceName.toStdString();

            auto aggName = AggregateDeviceHelper::createOrUpdate(
                mRealOutputDeviceName, mRealInputDeviceName);
            if (!aggName.empty()) {
                // Use the aggregate as a single device for both I/O
                setup.outputDeviceName = juce::String(aggName);
                setup.inputDeviceName  = juce::String(aggName);

                // Need to rescan so JUCE sees the new aggregate device
                if (auto* type = mDeviceManager->getCurrentDeviceTypeObject())
                    type->scanForDevices();
            }
        } else {
            // Same device for both I/O, or no input — no aggregate needed
            mRealOutputDeviceName.clear();
            mRealInputDeviceName.clear();
            AggregateDeviceHelper::destroy();

            // Let CoreAudio settle after aggregate teardown before opening
            // a new device — Ardour uses similar pauses between aggregate steps.
            if (wasOnAggregate)
                juce::Thread::sleep(150);
        }
#endif

        // Timestamp so changeListenerCallback ignores ALL async notifications
        // triggered by our device setup.  A single setup can generate multiple
        // CoreAudio notifications (device change, rate change, aggregate events).
        mLastSelfTriggeredChange = std::chrono::steady_clock::now();
        juce::String err = mDeviceManager->setAudioDeviceSetup(setup, true);
        if (err.isNotEmpty()) errStr = err.toStdString();
    } else {
        // Headless: no real device to configure; use failure hook for testing
        if (testSwapFailure) errStr = testSwapFailure();
    }

    if (!errStr.empty()) {
        if (isCold) rebuild_world(currentRate);
        // --- Restart audio (failure path) ---
        if (mDeviceManager) {
            mDeviceManager->addAudioCallback(&mAudioCallback);
        } else {
            restartHeadlessDriver(currentRate);
        }
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
        opts[14] = static_cast<uint32_t>(newRate);

        try {
            if (testRebuildFailure) {
                std::string failMsg = testRebuildFailure();
                if (!failMsg.empty())
                    throw std::runtime_error(failMsg);
            }
            rebuild_world(newRate);
        } catch (const std::exception& e) {
            fprintf(stderr, "[supersonic] rebuild_world failed: %s — recovering with safe defaults\n",
                    e.what());
            fflush(stderr);

            double safeRate = currentRate;
            int safeBuffer = 128;
            opts[14] = static_cast<uint32_t>(safeRate);
            mCurrentConfig.sampleRate = static_cast<int>(safeRate);
            mCurrentConfig.bufferSize = safeBuffer;

            try {
                rebuild_world(safeRate);
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
    if (mDeviceManager) {
        mDeviceManager->addAudioCallback(&mAudioCallback);
    } else {
        restartHeadlessDriver(mCurrentConfig.sampleRate);
    }
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
            mCurrentConfig.numInputChannels = finalDev->getActiveInputChannels().countNumberOfSetBits();

            juce::AudioDeviceManager::AudioDeviceSetup finalSetup;
            mDeviceManager->getAudioDeviceSetup(finalSetup);
            result.inputDeviceName = finalSetup.inputDeviceName.toStdString();

            fprintf(stderr, "[audio-device] switched to %s: %s %.0fHz buf=%d %dch\n",
                    finalDev->getTypeName().toRawUTF8(),
                    finalDev->getName().toRawUTF8(),
                    result.sampleRate, result.bufferSize,
                    mCurrentConfig.numOutputChannels);
        }
    } else {
        if (!recovered) {
            result.sampleRate = isCold ? sampleRate : currentRate;
        }
        result.bufferSize = mCurrentConfig.bufferSize;
    }
    result.success = true;
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
    printDeviceList();
    return result;
}

// --- Input channel management ---

SwapResult SupersonicEngine::enableInputChannels(int numChannels) {
    // -1 means "re-enable inputs". Use the boot-time channel count if it
    // was non-zero, otherwise fall back to stereo. This handles the case
    // where the daemon booted with -i 0 but the user later enables inputs.
    if (numChannels < 0) {
        static constexpr int kDefaultInputChannels = 2;
        numChannels = (mBootInputChannels > 0) ? mBootInputChannels : kDefaultInputChannels;
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

    // Save old values for rollback on failure
    int oldNumInputChannels = mCurrentConfig.numInputChannels;
    uint32_t* opts = reinterpret_cast<uint32_t*>(ring_buffer_storage + WORLD_OPTIONS_START);
    uint32_t oldOpts5 = opts[5];

    // Update config and worldOptions before the cold swap
    mCurrentConfig.numInputChannels = numChannels;
    opts[5] = static_cast<uint32_t>(numChannels);

    auto result = switchDevice("", 0, 0, true);

    if (!result.success) {
        mCurrentConfig.numInputChannels = oldNumInputChannels;
        opts[5] = oldOpts5;
    }

    return result;
}

// --- Audio driver management ---

std::vector<std::string> SupersonicEngine::listDrivers() const {
    std::vector<std::string> result;
    if (!mDeviceManager) return result;

    auto& types = mDeviceManager->getAvailableDeviceTypes();
    for (auto* type : types)
        result.push_back(type->getTypeName().toStdString());
    return result;
}

std::string SupersonicEngine::currentDriver() const {
    if (!mDeviceManager) return "";
    auto* dev = mDeviceManager->getCurrentAudioDevice();
    if (!dev) return "";
    return dev->getTypeName().toStdString();
}

SwapResult SupersonicEngine::switchDriver(const std::string& driverName) {
    SwapResult result;
    result.deviceName = driverName;

    // ── Headless mode ───────────────────────────────────────────────────
    // No real audio driver to switch.  If the test hook is set, simulate
    // the rate the new driver's default device would report.
    if (!mDeviceManager) {
        if (!testDriverSwitchRate) {
            result.error = "no audio device in headless mode";
            return result;
        }
        double newRate = testDriverSwitchRate();
        if (static_cast<int>(newRate) == mCurrentConfig.sampleRate) {
            // Same rate — hot swap, no world rebuild needed
            result.success    = true;
            result.type       = SwapType::Hot;
            result.sampleRate = newRate;
            result.bufferSize = mCurrentConfig.bufferSize;
            if (onSwapEvent) onSwapEvent("swap:start", result);
            if (onSwapEvent) onSwapEvent("swap:complete", result);
            return result;
        }
        // Different rate — delegate to switchDevice for a cold swap.
        // Mutex is not held here so switchDevice can acquire it.
        return switchDevice("", newRate);
    }

    // ── Real driver switch ──────────────────────────────────────────────
    double coldSwapRate = 0;

    {   // Mutex scope — released before any cold swap delegation
        if (!mSwapMutex.try_lock()) {
            result.error = "swap already in progress";
            return result;
        }
        std::lock_guard<std::mutex> guard(mSwapMutex, std::adopt_lock);

        // Pause audio and remove callback
        mAudioCallback.pause();
        mDeviceManager->removeAudioCallback(&mAudioCallback);

        // Switch driver type
        mDeviceManager->setCurrentAudioDeviceType(juce::String(driverName), true);
        juce::String err = mDeviceManager->initialiseWithDefaultDevices(
            mCurrentConfig.numInputChannels, mCurrentConfig.numOutputChannels);
        if (err.isNotEmpty()) {
            err = mDeviceManager->initialiseWithDefaultDevices(0, mCurrentConfig.numOutputChannels);
        }
        if (err.isNotEmpty()) {
            err = mDeviceManager->initialiseWithDefaultDevices(0, 0);
        }

        if (err.isNotEmpty()) {
            // Fallback: try to restore previous driver
            mDeviceManager->addAudioCallback(&mAudioCallback);
            mAudioCallback.resume();
            result.error = err.toStdString();
            if (onSwapEvent) onSwapEvent("swap:failed", result);
            return result;
        }

        // Try to match sample rate (let driver pick its own buffer size)
        if (auto* dev = mDeviceManager->getCurrentAudioDevice()) {
            juce::AudioDeviceManager::AudioDeviceSetup setup;
            mDeviceManager->getAudioDeviceSetup(setup);
            if (static_cast<int>(setup.sampleRate) != mCurrentConfig.sampleRate) {
                setup.sampleRate = mCurrentConfig.sampleRate;
                mDeviceManager->setAudioDeviceSetup(setup, true);
            }
        }

        // Re-add callback and resume
        mDeviceManager->addAudioCallback(&mAudioCallback);
        mAudioCallback.resume();

        // Read actual device state after switch
        auto* finalDev = mDeviceManager->getCurrentAudioDevice();
        if (finalDev) {
            result.sampleRate = finalDev->getCurrentSampleRate();
            result.bufferSize = finalDev->getCurrentBufferSizeSamples();
            mCurrentConfig.numOutputChannels = finalDev->getActiveOutputChannels().countNumberOfSetBits();
            mCurrentConfig.numInputChannels  = finalDev->getActiveInputChannels().countNumberOfSetBits();

            fprintf(stderr, "[audio-device] switched to %s: %s %.0fHz buf=%d %dch\n",
                    finalDev->getTypeName().toRawUTF8(),
                    finalDev->getName().toRawUTF8(),
                    result.sampleRate, result.bufferSize,
                    mCurrentConfig.numOutputChannels);

            // Check for rate mismatch — if the new driver can't match the
            // World's rate, we need a cold swap to rebuild at the actual rate.
            if (static_cast<int>(result.sampleRate) != mCurrentConfig.sampleRate) {
                coldSwapRate = result.sampleRate;
            }
        }

        if (coldSwapRate == 0) {
            // No rate mismatch — complete as a hot swap
            result.success = true;
            result.type    = SwapType::Hot;
            if (onSwapEvent) onSwapEvent("swap:start", result);
            if (onSwapEvent) onSwapEvent("swap:complete", result);
            return result;
        }
    } // mutex released

    // Rate mismatch detected — cold swap to rebuild World at the new rate.
    // Mutex is released so switchDevice can acquire it.
    fprintf(stderr, "[audio-device] driver rate mismatch (world=%d, device=%.0f) — cold swap\n",
            mCurrentConfig.sampleRate, coldSwapRate);
    return switchDevice("", coldSwapRate);
}

// --- Device change detection ---

void SupersonicEngine::changeListenerCallback(juce::ChangeBroadcaster* source) {
    if (source != mDeviceManager.get()) return;
    if (!mRunning.load()) return;

    // Ignore async notifications triggered by our own setAudioDeviceSetup.
    // A single device change can generate multiple CoreAudio notifications
    // (especially with aggregate devices), so suppress for 1 second.
    auto elapsed = std::chrono::steady_clock::now() - mLastSelfTriggeredChange;
    if (elapsed < std::chrono::seconds(1)) return;

    // Don't fight with an in-progress device swap
    if (!mSwapMutex.try_lock()) return;

    bool needsColdSwap = false;
    std::string coldSwapDevice;
    double coldSwapRate = 0;

    {
        std::lock_guard<std::mutex> guard(mSwapMutex, std::adopt_lock);

        if (mDeviceMode.empty()) {
            // System mode: reinitialise with default device, preserving sample rate and buffer
            fprintf(stderr, "[audio-device] device change detected (system mode) — reinitialising\n");
            reinitialiseWithDefaultsPreservingConfig();

            auto* dev = mDeviceManager->getCurrentAudioDevice();
            if (dev) {
                int newRate = static_cast<int>(dev->getCurrentSampleRate());
                if (newRate > 0 && newRate != mCurrentConfig.sampleRate) {
                    fprintf(stderr, "[audio-device] system device sample rate changed "
                            "(%d -> %d Hz) — will perform cold swap\n",
                            mCurrentConfig.sampleRate, newRate);
                    needsColdSwap = true;
                    coldSwapDevice = dev->getName().toStdString();
                    coldSwapRate = static_cast<double>(newRate);
                } else {
                    mCurrentConfig.numOutputChannels =
                        dev->getActiveOutputChannels().countNumberOfSetBits();
                    mCurrentConfig.numInputChannels =
                        dev->getActiveInputChannels().countNumberOfSetBits();
                }
            }
        }

        if (!needsColdSwap) {
            printDeviceList();
        }
    }

    // Cold swap outside the lock — switchDevice acquires the mutex itself
    if (needsColdSwap) {
        switchDevice(coldSwapDevice, coldSwapRate, 0);
    }
}

std::string SupersonicEngine::setDeviceMode(const std::string& mode) {
    std::string previousMode = mDeviceMode;

    if (mode == "system" || mode.empty()) {
        mDeviceMode.clear();
    } else {
        mDeviceMode = mode;
    }

    if (!mRunning.load()) return "";

    if (mDeviceMode.empty()) {
        // System mode — switch to system default device while preserving
        // sample rate, buffer size, and channel count.
        if (!previousMode.empty() && mDeviceManager) {
            fprintf(stderr, "[audio-device] switching to system default\n");
            auto err = reinitialiseWithDefaultsPreservingConfig();
            if (err.isNotEmpty()) {
                fprintf(stderr, "[audio-device] system mode init failed: %s\n",
                        err.toRawUTF8());
                return err.toStdString();
            }

            if (auto* dev = mDeviceManager->getCurrentAudioDevice()) {
                mCurrentConfig.numOutputChannels = dev->getActiveOutputChannels().countNumberOfSetBits();
                mCurrentConfig.numInputChannels = dev->getActiveInputChannels().countNumberOfSetBits();
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
                bool rateOk = false;
                auto& types = mDeviceManager->getAvailableDeviceTypes();
                for (auto* type : types) {
                    if (rateOk) break;
                    type->scanForDevices();
                    for (auto& name : type->getDeviceNames(false)) {
                        if (name.toStdString() == mDeviceMode) {
                            std::unique_ptr<juce::AudioIODevice> probe(
                                type->createDevice(name, juce::String()));
                            if (probe) {
                                for (auto r : probe->getAvailableSampleRates())
                                    if (static_cast<int>(r) == static_cast<int>(curRate))
                                        rateOk = true;
                            }
                            break;
                        }
                    }
                }
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

    auto devices = listDevices();
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

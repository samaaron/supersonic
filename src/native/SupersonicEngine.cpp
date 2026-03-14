/*
 * SupersonicEngine.cpp
 */
#include "SupersonicEngine.h"
#include "src/audio_processor.h"
#include "src/shared_memory.h"
#include "osc/OscReceivedElements.h"
#include "RingBufferWriter.h"
#include "scsynth/server/SC_Prototypes.h"  // zfree
#include <juce_audio_formats/juce_audio_formats.h>
#include <chrono>
#include <cstring>
#include <thread>

extern "C" {
    void destroy_world();
    void rebuild_world(double sample_rate);
}

SupersonicEngine::SupersonicEngine() = default;

SupersonicEngine::~SupersonicEngine() {
    shutdown();
}


void SupersonicEngine::initialise(const Config& cfg) {
    if (mRunning.load()) return;

    mHeadless = cfg.headless;
    mCurrentConfig = cfg;

    // -- Wire callbacks ---------------------------------------------------
    // onReply/onDebug should be set before initialise() — worker threads
    // read them via captured `this` pointer without synchronisation.
    // Setting them after initialise() is a data race.
    mReplyReader.onReply = [this](const uint8_t* d, uint32_t s) {
        // Intercept /supersonic/buffer/freed — free the buffer memory
        // and don't forward this internal message to external listeners.
        if (interceptBufferFreed(d, s)) return;

        if (mDeviceManager) mUdpServer.sendReply(d, s);
        if (onReply) onReply(d, s);
    };
    mDebugReader.onDebug = [this](const std::string& s) {
        if (onDebug) onDebug(s);
    };

    if (!cfg.headless) {
        mDeviceManager = std::make_unique<juce::AudioDeviceManager>();

        // -- Select audio driver --------------------------------------------------
        if (!cfg.audioDriver.empty()) {
            mDeviceManager->setCurrentAudioDeviceType(
                juce::String(cfg.audioDriver), true);
        }

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

        // If -H was specified (via audio-settings.toml), switch to that device
        // and lock device mode so the GUI doesn't override it.
        if (!cfg.hardwareDevice.empty()) {
            juce::AudioDeviceManager::AudioDeviceSetup setup;
            mDeviceManager->getAudioDeviceSetup(setup);
            setup.outputDeviceName = juce::String(cfg.hardwareDevice);
            setup.useDefaultOutputChannels = true;
            setup.useDefaultInputChannels = true;
            if (cfg.sampleRate > 0) setup.sampleRate = cfg.sampleRate;
            juce::String hwErr = mDeviceManager->setAudioDeviceSetup(setup, true);
            if (hwErr.isNotEmpty()) {
                fprintf(stderr, "[audio-device] -H device '%s' not available, using default\n",
                        cfg.hardwareDevice.c_str());
                // setAudioDeviceSetup may have closed the previous device — reopen defaults
                mDeviceManager->initialiseWithDefaultDevices(
                    cfg.numInputChannels, cfg.numOutputChannels);
            } else {
                fprintf(stderr, "[audio-device] -H selected device: %s\n",
                        cfg.hardwareDevice.c_str());
                mDeviceMode = cfg.hardwareDevice;  // lock mode so GUI knows TOML set it
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
            } else {
                // Auto: pick the smallest available buffer that is a multiple of 128
                constexpr int kBlockSize = 128;
                auto sizes = dev->getAvailableBufferSizes();
                int best = 0;
                for (auto s : sizes) {
                    if (s >= kBlockSize && s % kBlockSize == 0) {
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
        &mClock,
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
        &mClock,
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
}

void SupersonicEngine::shutdown() {
    if (!mRunning.exchange(false)) return;

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

    mHeadlessDriver.stopThread(2000);
    mSampleLoader.stopThread(2000);
    mUdpServer.stopThread(2000);
    mPrescheduler.stopThread(2000);
    mReplyReader.stopThread(2000);
    mDebugReader.stopThread(2000);
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

    auto& types = mDeviceManager->getAvailableDeviceTypes();
    for (auto* type : types) {
        // Scan output devices for each type
        type->scanForDevices();
        auto outputNames = type->getDeviceNames(false);

        for (auto& devName : outputNames) {
            DeviceInfo info;
            info.name = devName.toStdString();
            info.typeName = type->getTypeName().toStdString();

            // Create a temporary device to query capabilities
            std::unique_ptr<juce::AudioIODevice> tempDev(
                type->createDevice(devName, juce::String()));
            if (tempDev) {
                auto rates = tempDev->getAvailableSampleRates();
                for (auto r : rates) info.availableSampleRates.push_back(r);

                auto bufs = tempDev->getAvailableBufferSizes();
                for (auto b : bufs) info.availableBufferSizes.push_back(b);

                auto outNames = tempDev->getOutputChannelNames();
                auto inNames  = tempDev->getInputChannelNames();
                info.maxOutputChannels = outNames.size();
                info.maxInputChannels  = inNames.size();
            }

            result.push_back(std::move(info));
        }
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
    info.activeOutputChannels = dev->getOutputChannelNames().size();
    info.activeInputChannels  = dev->getInputChannelNames().size();
    info.outputLatencySamples = dev->getOutputLatencyInSamples();
    info.inputLatencySamples  = dev->getInputLatencyInSamples();

    return info;
}

SwapResult SupersonicEngine::switchDevice(const std::string& deviceName,
                                           double sampleRate,
                                           int bufferSize) {
    SwapResult result;
    result.deviceName = deviceName;

    if (!mDeviceManager) {
        result.error = "no audio device in headless mode";
        return result;
    }

    // Try to acquire swap mutex (non-blocking)
    if (!mSwapMutex.try_lock()) {
        result.error = "swap already in progress";
        return result;
    }
    std::lock_guard<std::mutex> guard(mSwapMutex, std::adopt_lock);

    // Determine swap type
    auto* currentDev = mDeviceManager->getCurrentAudioDevice();
    double currentRate = currentDev ? currentDev->getCurrentSampleRate() : 0.0;

    // When no explicit sample rate requested, probe the target device to see
    // if the current rate is supported.  If not, auto-select the first available
    // rate — this makes the swap a cold swap (world rebuild).
    if (sampleRate == 0 && currentRate > 0) {
        bool found = false;
        auto& types = mDeviceManager->getAvailableDeviceTypes();
        for (auto* type : types) {
            if (found) break;
            type->scanForDevices();
            auto names = type->getDeviceNames(false);
            for (auto& name : names) {
                if (name.toStdString() == deviceName) {
                    std::unique_ptr<juce::AudioIODevice> tempDev(
                        type->createDevice(name, juce::String()));
                    if (tempDev) {
                        auto rates = tempDev->getAvailableSampleRates();
                        bool supported = false;
                        for (auto r : rates)
                            if (static_cast<int>(r) == static_cast<int>(currentRate))
                                supported = true;
                        if (!supported && !rates.isEmpty()) {
                            sampleRate = rates[0];
                            fprintf(stderr,
                                "[audio-device] current rate %.0f not supported "
                                "by %s, will use %.0f (cold swap)\n",
                                currentRate, deviceName.c_str(), sampleRate);
                        }
                    }
                    found = true;
                    break;
                }
            }
        }
    }

    bool isCold = (sampleRate > 0 && sampleRate != currentRate);
    result.type = isCold ? SwapType::Cold : SwapType::Hot;

    if (onSwapEvent) onSwapEvent("swap:start", result);

    // --- Pause and optionally capture state ---
    if (isCold) {
        purge();
        mSampleLoader.pauseLoading();
    }
    mAudioCallback.pause();
    if (isCold) mStateCache.captureAll();
    mDeviceManager->removeAudioCallback(&mAudioCallback);
    if (isCold) destroy_world();

    // Configure new device
    juce::AudioDeviceManager::AudioDeviceSetup setup;
    mDeviceManager->getAudioDeviceSetup(setup);
    setup.outputDeviceName = juce::String(deviceName);
    setup.useDefaultInputChannels = true;
    setup.useDefaultOutputChannels = true;
    if (sampleRate > 0) setup.sampleRate = sampleRate;
    if (bufferSize > 0) setup.bufferSize = bufferSize;

    juce::String err = mDeviceManager->setAudioDeviceSetup(setup, true);
    if (err.isNotEmpty()) {
        if (isCold) rebuild_world(currentRate);
        mDeviceManager->addAudioCallback(&mAudioCallback);
        mAudioCallback.resume();
        if (isCold) mSampleLoader.resumeLoading();
        result.error = err.toStdString();
        if (onSwapEvent) onSwapEvent("swap:failed", result);
        return result;
    }

    if (isCold) {
        auto* newDev = mDeviceManager->getCurrentAudioDevice();
        double newRate = newDev ? newDev->getCurrentSampleRate() : sampleRate;
        mCurrentConfig.sampleRate = static_cast<int>(newRate);

        uint32_t* opts = reinterpret_cast<uint32_t*>(ring_buffer_storage + 65536);
        opts[14] = static_cast<uint32_t>(newRate);
        rebuild_world(newRate);
    }

    mDeviceManager->addAudioCallback(&mAudioCallback);
    mAudioCallback.resume();

    if (isCold) {
        // Restore synthdefs
        uint8_t* base = ring_buffer_storage;
        ControlPointers* ctrl = reinterpret_cast<ControlPointers*>(base + CONTROL_START);
        for (auto& [name, defData] : mStateCache.synthDefs()) {
            auto pkt = OscBuilder::message("/d_recv",
                OscBuilder::Blob{defData.data(), defData.size()});
            RingBufferWriter::write(
                base + IN_BUFFER_START, IN_BUFFER_SIZE,
                &ctrl->in_head, &ctrl->in_tail,
                &ctrl->in_sequence, &ctrl->in_write_lock,
                pkt.ptr(), pkt.size());
        }

        // Restore buffers
        extern World* g_world;
        for (auto& buf : mStateCache.buffers()) {
            if (!buf.path.empty()) {
                mSampleLoader.load(g_world, buf.bufnum, buf.path.c_str(),
                                   buf.startFrame, buf.numFrames);
            }
        }
        mSampleLoader.resumeLoading();
        mStateCache.restoreAll();
    }

    auto* finalDev = mDeviceManager->getCurrentAudioDevice();
    if (finalDev) {
        result.sampleRate = finalDev->getCurrentSampleRate();
        result.bufferSize = finalDev->getCurrentBufferSizeSamples();
        mCurrentConfig.sampleRate = static_cast<int>(result.sampleRate);
        mCurrentConfig.numOutputChannels = finalDev->getActiveOutputChannels().countNumberOfSetBits();
        mCurrentConfig.numInputChannels = finalDev->getActiveInputChannels().countNumberOfSetBits();

        fprintf(stderr, "[audio-device] switched to %s: %s %.0fHz buf=%d %dch\n",
                finalDev->getTypeName().toRawUTF8(),
                finalDev->getName().toRawUTF8(),
                result.sampleRate, result.bufferSize,
                mCurrentConfig.numOutputChannels);
    }
    result.success = true;
    if (onSwapEvent) onSwapEvent("swap:complete", result);
    printDeviceList();
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

    if (!mDeviceManager) {
        result.error = "no audio device in headless mode";
        return result;
    }

    // Try to acquire swap mutex (non-blocking)
    if (!mSwapMutex.try_lock()) {
        result.error = "swap already in progress";
        return result;
    }
    std::lock_guard<std::mutex> guard(mSwapMutex, std::adopt_lock);

    result.type = SwapType::Hot;
    if (onSwapEvent) onSwapEvent("swap:start", result);

    // Pause audio and remove callback
    mAudioCallback.pause();
    mDeviceManager->removeAudioCallback(&mAudioCallback);

    // Switch driver type
    mDeviceManager->setCurrentAudioDeviceType(juce::String(driverName), true);
    juce::String err = mDeviceManager->initialiseWithDefaultDevices(0, 2);
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

    // Ensure sample rate matches (let driver pick its own buffer size)
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

    // Fill result
    auto* finalDev = mDeviceManager->getCurrentAudioDevice();
    if (finalDev) {
        result.sampleRate = finalDev->getCurrentSampleRate();
        result.bufferSize = finalDev->getCurrentBufferSizeSamples();

        fprintf(stderr, "[audio-device] switched to %s: %s %.0fHz buf=%d\n",
                finalDev->getTypeName().toRawUTF8(),
                finalDev->getName().toRawUTF8(),
                result.sampleRate, result.bufferSize);
    }
    result.success = true;
    if (onSwapEvent) onSwapEvent("swap:complete", result);
    return result;
}

// --- Device change detection ---

void SupersonicEngine::changeListenerCallback(juce::ChangeBroadcaster* source) {
    if (source != mDeviceManager.get()) return;
    if (!mRunning.load()) return;

    // Don't fight with an in-progress device swap
    if (!mSwapMutex.try_lock()) return;
    std::lock_guard<std::mutex> guard(mSwapMutex, std::adopt_lock);

    if (mDeviceMode.empty()) {
        // System mode: reinitialise with default devices
        fprintf(stderr, "[audio-device] device change detected (system mode) — reinitialising\n");
        auto err = mDeviceManager->initialiseWithDefaultDevices(0, 2);
        if (err.isNotEmpty()) {
            mDeviceManager->initialiseWithDefaultDevices(0, 0);
        }

        auto* dev = mDeviceManager->getCurrentAudioDevice();
        if (dev) {
            int newRate = static_cast<int>(dev->getCurrentSampleRate());
            if (newRate > 0 && newRate != mCurrentConfig.sampleRate) {
                fprintf(stderr, "[audio-device] WARNING: system device changed to "
                        "different sample rate (%d -> %d Hz) — restart required\n",
                        mCurrentConfig.sampleRate, newRate);
            }

            mCurrentConfig.numOutputChannels =
                dev->getActiveOutputChannels().countNumberOfSetBits();
            mCurrentConfig.numInputChannels =
                dev->getActiveInputChannels().countNumberOfSetBits();
        }
    }

    printDeviceList();
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
        // System mode — only reinitialise if we were previously in manual mode.
        if (!previousMode.empty() && mDeviceManager) {
            fprintf(stderr, "[audio-device] switching to system default\n");
            auto err = mDeviceManager->initialiseWithDefaultDevices(0, 2);
            if (err.isNotEmpty()) {
                err = mDeviceManager->initialiseWithDefaultDevices(0, 0);
            }
            if (err.isNotEmpty()) {
                fprintf(stderr, "[audio-device] system mode init failed: %s\n",
                        err.toRawUTF8());
                return err.toStdString();
            }
            // Update config from actual device
            if (auto* dev = mDeviceManager->getCurrentAudioDevice()) {
                mCurrentConfig.sampleRate = static_cast<int>(dev->getCurrentSampleRate());
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
        fprintf(stderr, "[audio-device-entry] %s|%s|%d|%d\n",
                dev.name.c_str(), dev.typeName.c_str(),
                dev.maxOutputChannels, dev.maxInputChannels);
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

/*
 * SupersonicEngine.h — Top-level orchestrator
 */
#pragma once

#include <juce_audio_devices/juce_audio_devices.h>
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#ifdef __APPLE__
#include <CoreAudio/CoreAudio.h>
#endif

#include "Prescheduler.h"
#include "ReplyReader.h"
#include "DebugReader.h"
#include "OscUdpServer.h"
#include "JuceAudioCallback.h"
#include "SampleLoader.h"
#include "DeviceInfo.h"
#include "StateCache.h"
#include "OscBuilder.h"
#include "HeadlessDriver.h"
#include "src/engine_state.h"
#include "scsynth/common/server_shm.hpp"

class SupersonicEngine : private juce::ChangeListener {
    friend class EngineFixture;  // test fixture needs access to mAudioCallback
public:
    struct Config {
        int    sampleRate               = 48000;
        int    bufferSize               = 0;   // 0 = auto (smallest multiple of 128)
        int    udpPort                  = 57110;
        double preschedulerLookaheadS   = 0.500;
        int    maxNodes                 = 1024;
        int    numBuffers               = 1024;
        int    numOutputChannels        = 2;
        int    numInputChannels         = 2;
        int    numAudioBusChannels      = 1024;
        int    maxGraphDefs             = 512;
        int    maxWireBufs              = 64;
        int    numControlBusChannels    = 16384;
        int    realTimeMemorySize       = 8192;    // KB — World_New multiplies by 1024
        int    numRGens                 = 64;
        bool   headless                 = false;   // skip audio device (for tests)
        std::string bindAddress       = "127.0.0.1"; // localhost only; use -B to override
        std::string hardwareDevice;                // -H flag: fuzzy match on "Driver : Device"
    };

    SupersonicEngine();
    ~SupersonicEngine();

    void initialise(const Config& config);
    void shutdown();

    std::function<void(const uint8_t*, uint32_t)> onReply;
    std::function<void(const std::string&)>        onDebug;

    void sendOsc(const uint8_t* data, uint32_t size);
    bool isRunning() const { return mRunning.load(); }

    // --- Engine lifecycle state ---
    EngineState engineState() const { return mEngineState.load(); }
    void        setEngineState(EngineState state, const std::string& reason = "");

    // --- Variadic OSC send (builds message + dispatches through sendOsc) ---
    template<typename... Args>
    void send(const char* address, Args&&... args) {
        auto pkt = OscBuilder::message(address, std::forward<Args>(args)...);
        sendOsc(pkt.ptr(), pkt.size());
    }

    // Bundle send — ntpTimeSec is NTP time in seconds (double -> uint64 timetag)
    void sendBundle(double ntpTimeSec, std::initializer_list<OscPacket> messages);

    // --- Device management ---
    std::vector<DeviceInfo>  listDevices() const;
    CurrentDeviceInfo        currentDevice() const;     // resolves aggregate to real names
    std::string              realOutputDeviceName() const { return mRealOutputDeviceName; }
    std::string              realInputDeviceName() const  { return mRealInputDeviceName; }
    SwapResult               switchDevice(const std::string& deviceName,
                                          double sampleRate = 0,
                                          int bufferSize = 0,
                                          bool forceCold = false,
                                          const std::string& inputDeviceName = "");

    // --- Input channel management ---
    // Enable/disable audio input. Triggers a cold swap (world rebuild).
    // numChannels=0 disables input, >0 enables that many input channels,
    // -1 re-enables with the configured input channel count.
    // On macOS, enabling input triggers the OS microphone permission dialog.
    SwapResult               enableInputChannels(int numChannels);

    // Set the configured input channel count (used when re-enabling via -1).
    // Does NOT trigger a swap — call enableInputChannels() afterward if needed.
    void setConfiguredInputChannels(int numChannels) { mBootInputChannels = numChannels; }
    int  configuredInputChannels() const { return mBootInputChannels; }

    // --- Audio driver management ---
    std::vector<std::string> listDrivers() const;
    std::string              currentDriver() const;
    SwapResult               switchDriver(const std::string& driverName);

    // --- Recording (JUCE-side output tap) ---
    struct RecordResult {
        bool success = false;
        std::string path;
        std::string error;
    };
    RecordResult startRecording(const std::string& path,
                                const std::string& format = "wav",
                                int bitDepth = 24);
    RecordResult stopRecording();
    bool         isRecording() const;

    // Device swap event callback
    std::function<void(const std::string& event, const SwapResult& result)> onSwapEvent;

    // Injectable hook for testing: if set and returns non-empty string,
    // the device configuration step is treated as failed with that error.
    std::function<std::string()> testSwapFailure;

    // Injectable hook for testing: if set and returns non-empty string,
    // rebuild_world() is skipped and the error triggers recovery to safe defaults.
    std::function<std::string()> testRebuildFailure;

    // Injectable hook for testing: if set, switchDriver() in headless mode
    // simulates a successful driver switch where the new driver's default
    // device reports this sample rate. Allows testing rate-mismatch cold swaps.
    std::function<double()> testDriverSwitchRate;

    // --- State cache ---
    StateCache& stateCache() { return mStateCache; }
    const StateCache& stateCache() const { return mStateCache; }

    // --- Clock offset ---
    void   setClockOffset(double offsetSeconds);
    double getClockOffset() const;

    // --- Audio callback access (for preTick hook, pause/resume) ---
    JuceAudioCallback& audioCallback() { return mAudioCallback; }

    // --- Device mode (system/auto vs manual device name) ---
    std::string setDeviceMode(const std::string& mode);
    void forceDeviceMode(const std::string& mode) { mDeviceMode = mode; }
    std::string deviceMode() const { return mDeviceMode; }
    void printDeviceList();

    // --- Purge stale messages ---
    void purge();

private:
    void changeListenerCallback(juce::ChangeBroadcaster* source) override;
    void interceptForCache(const uint8_t* data, uint32_t size);
    bool interceptBufferFreed(const uint8_t* data, uint32_t size);
    void restartHeadlessDriver(double sampleRate);
    juce::String reinitialiseWithDefaultsPreservingConfig();

#ifdef __APPLE__
    void handleSystemDefaultOutputChanged();
    static OSStatus defaultDevicePropertyListenerProc(
        AudioObjectID, UInt32, const AudioObjectPropertyAddress*, void* inClientData);
    bool mDefaultDevicePropertyListenerInstalled = false;
#endif

    JuceAudioCallback mAudioCallback;
    Prescheduler      mPrescheduler;
    ReplyReader       mReplyReader;
    DebugReader       mDebugReader;
    OscUdpServer      mUdpServer;
    SampleLoader      mSampleLoader;
    StateCache        mStateCache;

    HeadlessDriver               mHeadlessDriver;
    std::unique_ptr<juce::AudioDeviceManager> mDeviceManager;
    std::atomic<bool>        mRunning{false};
    std::atomic<EngineState> mEngineState{EngineState::Stopped};
    bool                     mHeadless{false};
    Config                   mCurrentConfig;
    int                      mBootInputChannels = 2;  // original -i value, for re-enabling inputs
    std::string              mLastInputDeviceName;    // saved on disable, restored on re-enable
    std::string              mRealOutputDeviceName;   // actual output device behind aggregate
    std::string              mRealInputDeviceName;    // actual input device behind aggregate
    std::mutex               mSwapMutex;
    std::chrono::steady_clock::time_point mLastSelfTriggeredChange{}; // suppress async change notifications from our own setAudioDeviceSetup
    std::string              mDeviceMode;   // empty = system/auto, non-empty = manual device name
    bool                     mWorldRebuilt{false};
    std::map<std::string, int> mDeviceRateMemory; // per-device remembered sample rate

    // Shared memory — owned by the engine, survives across cold swaps.
    server_shared_memory_creator* mShmemCreator = nullptr;

    // Recording
    juce::TimeSliceThread    mRecordThread{"SuperSonic-RecordIO"};
    std::string              mRecordPath;
};

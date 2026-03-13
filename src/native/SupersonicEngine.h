/*
 * SupersonicEngine.h — Top-level orchestrator
 */
#pragma once

#include <juce_audio_devices/juce_audio_devices.h>
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

#include "NTPClock.h"
#include "Prescheduler.h"
#include "ReplyReader.h"
#include "DebugReader.h"
#include "OscUdpServer.h"
#include "JuceAudioCallback.h"
#include "SampleLoader.h"
#include "DeviceInfo.h"
#include "StateCache.h"
#include "OscBuilder.h"

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
        std::string bindAddress;                   // empty = all interfaces
        std::string audioDriver;                   // empty = auto-select; e.g. "DirectSound", "Windows Audio"
        std::string hardwareDevice;                // empty = system default; set by -H flag (audio-settings.toml)
    };

    SupersonicEngine();
    ~SupersonicEngine();

    void initialise(const Config& config);
    void shutdown();

    std::function<void(const uint8_t*, uint32_t)> onReply;
    std::function<void(const std::string&)>        onDebug;

    void sendOsc(const uint8_t* data, uint32_t size);
    bool isRunning() const { return mRunning.load(); }

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
    CurrentDeviceInfo        currentDevice() const;
    SwapResult               switchDevice(const std::string& deviceName,
                                          double sampleRate = 0,
                                          int bufferSize = 0);

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
    std::string deviceMode() const { return mDeviceMode; }
    void printDeviceList();

    // --- Purge stale messages ---
    void purge();

private:
    void changeListenerCallback(juce::ChangeBroadcaster* source) override;
    void interceptForCache(const uint8_t* data, uint32_t size);

    NTPClock          mClock;
    JuceAudioCallback mAudioCallback;
    Prescheduler      mPrescheduler;
    ReplyReader       mReplyReader;
    DebugReader       mDebugReader;
    OscUdpServer      mUdpServer;
    SampleLoader      mSampleLoader;
    StateCache        mStateCache;

    std::unique_ptr<juce::AudioDeviceManager> mDeviceManager;
    std::atomic<bool>        mRunning{false};
    bool                     mHeadless{false};
    Config                   mCurrentConfig;
    std::mutex               mSwapMutex;
    std::string              mDeviceMode;   // empty = system/auto, non-empty = manual device name

    // Recording
    juce::TimeSliceThread    mRecordThread{"SuperSonic-RecordIO"};
    std::string              mRecordPath;
};

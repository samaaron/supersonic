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
#include <vector>
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
    // Channel-count sentinel: negative means "open the device with all its
    // channels active (let JUCE/CoreAudio clamp to the hardware max)".
    // 0 still means "disabled" for inputs; positive N means "exactly N".
    static constexpr int kAutoChannelCount   = -1;
    // Upper bound on channels we'll ever request. JUCE's BigInteger bitmask
    // gets clamped to the device's real channel count by CoreAudio, so
    // over-requesting is safe — we just set this many bits and JUCE drops
    // the excess. 64 covers commodity audio interfaces (MOTU, RME, etc.).
    static constexpr int kRequestMaxChannels = 64;
    // Minimum buffer size on an aggregate device. Aggregates combine two
    // independent clocks (e.g. MBP Speakers + motu-xaero over USB) and run
    // kernel-level sample-rate conversion in the IOProc for drift
    // correction. Anything below ~256 samples starves the SRC and the
    // audio warbles ("drift storm"). Enforced at aggregate creation
    // paths regardless of -z / -Z / TOML.
    static constexpr int kMinAggregateBufferSize = 256;

    struct Config {
        int    sampleRate               = 48000;
        int    bufferSize               = 0;   // 0 = auto (smallest multiple of 128)
        int    udpPort                  = 57110;
        double preschedulerLookaheadS   = 0.500;
        int    maxNodes                 = 1024;
        int    numBuffers               = 1024;
        int    numOutputChannels        = kAutoChannelCount;
        int    numInputChannels         = kAutoChannelCount;
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

    void init(const Config& config);
    void shutdown();

    std::function<void(const uint8_t*, uint32_t)> onReply;
    std::function<void(const std::string&)>        onDebug;

    void sendOSC(const uint8_t* data, uint32_t size);
    bool isRunning() const { return mRunning.load(); }

    // --- Engine lifecycle state ---
    EngineState engineState() const { return mEngineState.load(); }
    void        setEngineState(EngineState state, const std::string& reason = "");

    // --- Variadic OSC send (builds message + dispatches through sendOSC) ---
    template<typename... Args>
    void send(const char* address, Args&&... args) {
        auto pkt = OscBuilder::message(address, std::forward<Args>(args)...);
        sendOSC(pkt.ptr(), pkt.size());
    }

    // Bundle send — ntpTimeSec is NTP time in seconds (double -> uint64 timetag)
    void sendBundle(double ntpTimeSec, std::initializer_list<OscPacket> messages);

    // --- Device management ---
    //
    // Device-name sentinels recognised by switchDevice / setDeviceMode and
    // mirrored on the GUI side:
    //   "__system__"  — follow the macOS system default output (mDeviceMode
    //                   is kept empty internally while this is active).
    //   "__none__"    — (input only) disable audio inputs; clears the
    //                   preferred input sub-device.
    // Any other non-empty value is treated as a literal device name.
    //
    // rescan=true triggers a full CoreAudio re-enumeration (may disrupt a
    // just-opened device on macOS). Pass false to reuse JUCE's cached list.
    std::vector<DeviceInfo>  listDevices(bool rescan = true) const;
    CurrentDeviceInfo        currentDevice() const;     // resolves aggregate to real names
    std::string              realOutputDeviceName() const { return mRealOutputDeviceName; }
    std::string              realInputDeviceName() const  { return mRealInputDeviceName; }
    SwapResult               switchDevice(const std::string& deviceName,
                                          double sampleRate = 0,
                                          int bufferSize = 0,
                                          bool forceCold = false,
                                          const std::string& inputDeviceName = "");

    // Re-open the current device (tear down and recreate without changing
    // selection). Use when an external config change — e.g. a MOTU Pro
    // Audio Control "Computer" channel-count bump — needs to flow through
    // without a full supersonic restart. Preserves aggregate / system-
    // default / manual mode semantics.
    SwapResult               reopenCurrentDevice();

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
    // The bool argument is true when an input device was requested in the
    // attempted setup, allowing the hook to simulate an input-specific
    // failure that should be retried output-only.
    std::function<std::string(bool inputRequested)> testSwapFailure;

    // Injectable hook for testing: if set and returns non-empty string,
    // rebuild_world() is skipped and the error triggers recovery to safe defaults.
    std::function<std::string()> testRebuildFailure;

    // Injectable hook for testing: if set, switchDriver() in headless mode
    // simulates a successful driver switch where the new driver's default
    // device reports this sample rate. Allows testing rate-mismatch cold swaps.
    std::function<double()> testDriverSwitchRate;

    // Injectable hook for testing: if set and returns a non-empty string,
    // init() throws std::runtime_error with that message just before
    // setting mRunning=true. Used to exercise the partial-init shutdown
    // cleanup path.
    std::function<std::string()> testInitFailure;

    // Test-only: when true, init() closes the audio device immediately
    // after the JUCE init block, before deciding which audio source to start.
    // This reproduces the "device manager exists but no current device"
    // state real users hit when JUCE/ALSA returns "no channels" against
    // PipeWire's default sink (issue #3526). Used to verify the headless
    // fallback path actually starts and that process_audio runs so OSC
    // commands aren't queued forever in the IN ring buffer.
    bool testForceNoCurrentDeviceAfterInit = false;

    // --- State cache ---
    StateCache& stateCache() { return mStateCache; }
    const StateCache& stateCache() const { return mStateCache; }

    // --- Clock offset ---
    void   setClockOffset(double offsetSeconds);
    double getClockOffset() const;

    // --- Audio callback access (for preTick hook, pause/resume) ---
    JuceAudioCallback& audioCallback() { return mAudioCallback; }

    // CFRunLoop suppression (macOS). Main.cpp's run-loop pump calls
    // isRunLoopSuppressed() each tick and sleeps instead of pumping
    // while true. setRunLoopSuppressed(false) is called by Main once
    // the boot-time aggregate has settled.
    bool isRunLoopSuppressed() const { return mSuppressRunLoop.load(); }
    void setRunLoopSuppressed(bool v) { mSuppressRunLoop.store(v); }

    // --- Device mode (system/auto vs manual device name) ---
    std::string setDeviceMode(const std::string& mode);
    void forceDeviceMode(const std::string& mode) { mDeviceMode = mode; }
    std::string deviceMode() const { return mDeviceMode; }

    // Preferred-device accessors. These track the user's long-lived
    // intent for auto-re-attach on hot-plug, independent of what device
    // is currently active (the device may have been removed and we
    // fell back to the system default — but we still want to come back
    // when it reappears). deviceMode() is "current selection"; these
    // are "want to use whenever available".
    std::string preferredOutputDevice() const { return mPreferredOutputDevice; }
    std::string preferredInputDevice() const  { return mPreferredInputDevice; }

    void printDeviceList();

    // --- Purge stale messages ---
    void purge();

private:
    void changeListenerCallback(juce::ChangeBroadcaster* source) override;
    void interceptForCache(const uint8_t* data, uint32_t size);
    bool interceptBufferFreed(const uint8_t* data, uint32_t size);

    // Clamp bufferSize up to kMinAggregateBufferSize when the current
    // aggregate has kernel drift compensation running (SRC IOProc
    // starves at tight buffers — audible warble). Also mirrors the
    // clamped value into mCurrentConfig.bufferSize. No-op on single
    // devices and same-clock aggregates (where drift-comp is skipped).
    // Called from all three aggregate-setup sites (init boot
    // path, init post-setup negotiate step, switchDevice
    // aggregate branch) so the floor is uniformly applied.
    void clampAggregateBufferIfNeeded(int& bufferSize);
    juce::String reinitialiseWithDefaultsPreservingConfig();

    // ── Audio source state machine ──────────────────────────────────────────
    //
    // process_audio() runs from exactly one of two drivers:
    //   RealCallback: JUCE's AudioDeviceManager fires our JuceAudioCallback
    //                 when a device is open.
    //   Headless:     a high-priority timer thread (HeadlessDriver) fakes
    //                 the same contract when no device is available. Used
    //                 for explicit cfg.headless==true, for boot-time
    //                 device-init failures (issue #3526: ALSA/PipeWire
    //                 "no channels"), and for cold-swap rollback when no
    //                 usable device is left.
    //
    // mActiveSource is the source of truth. Every device-swap path goes
    // through stopAudioSource() then startAudioSource() so the "exactly
    // one source active while running" invariant holds. Without it, a
    // partially-failed init can leave mDeviceManager non-null with no
    // current device, neither driver firing, and process_audio silently
    // never called.
    enum class AudioSource { None, RealCallback, Headless };

    AudioSource mActiveSource = AudioSource::None;

    AudioSource desiredAudioSource() const;

    // Precondition: mActiveSource == None. Picks RealCallback or Headless
    // based on desiredAudioSource(), then blocks until process_audio has
    // ticked at least once (or 5s with a warning). This blocking wait is
    // the boot/swap barrier so callers can sendOSC() immediately after.
    void startAudioSource();

    // Idempotent. Does NOT remove the change listener (shutdown-only) so
    // hot-plug events survive swaps.
    void stopAudioSource();

    void waitForFirstAudioTick(uint32_t before);

    // switchDevice sub-stages. Each has a single responsibility and is
    // safe to call independently (they read / write engine state, so
    // they're member methods rather than pure functions). Extracted to
    // keep switchDevice itself readable.
    //
    // Refuses "add mic while wireless output is active" upfront — the
    // aggregate logic later would drop it anyway, but refusing early
    // avoids triggering a cold swap that can race ScopeOut2 during
    // rebuild. Returns non-empty error string on refusal; empty = OK.
    std::string refuseWirelessMicAddition(const std::string& deviceName,
                                          const std::string& inputDeviceName);

    // Refuses a swap whose output or input name doesn't resolve to any
    // visible device (and isn't a known sentinel like "__system__" /
    // "__none__"). Without this, switchDevice mutates state (mCurrentConfig,
    // opts[], destroy_world) before the doomed setAudioDeviceSetup —
    // leaving the engine half-broken when JUCE returns "No such device".
    // Returns non-empty error string on refusal; empty = OK.
    std::string refuseUnknownDeviceName(const std::string& deviceName,
                                        const std::string& inputDeviceName);

    // Probes a named device (input or output) via JUCE and returns the
    // number of channels it advertises, or -1 if the name doesn't
    // match / device can't be opened. Handles the "__none__" sentinel
    // by returning -1 without probing.
    int probeDeviceChannelCount(const std::string& name, bool isInput);

    // Returns the sample rates advertised by a named device, or an
    // empty vector if the name doesn't match or the device can't be
    // opened. Core primitive: the rate-matching helpers below compose
    // it so there's one place that owns "walk device types → scan →
    // createDevice → getAvailableSampleRates".
    std::vector<double> probeDeviceSampleRates(const std::string& name,
                                               bool isInput);

    // Probes a named device for available sample rates and, if the
    // engine's currentRate isn't supported, sets sampleRate to the
    // nearest available rate. No-op if caller specified a rate, the
    // device can't be opened, or the name is empty. This is what makes
    // a switch "cold" when the target device can't run at the current
    // rate.
    void probeAndAdjustForTargetRate(const std::string& name, bool isInput,
                                     double& sampleRate, double currentRate);

    // Records the user's preferred output/input device for future
    // hot-plug re-attach, and caches the successfully-used sample rate
    // for this device name so a later switch back can restore it.
    // Called from switchDevice's success tail. deviceName empty means
    // rate/buffer-only change — no preference update.
    void recordSwapPreferences(const std::string& deviceName,
                               const std::string& inputDeviceName,
                               double sampleRate);

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
    // Rate held while on a non-wireless device. Remembered so a detour
    // through wireless (AirPlay/Bluetooth) — which forces 44.1 or its own
    // negotiated rate — doesn't leave the engine sticky at that rate once
    // the user switches back to a hardware device that can do the original.
    int                      mPreWirelessRate = 0;

    // User's preferred output device name across hot-plug cycles. Set from
    // -H / sound_card_name at boot even when that device isn't present at
    // boot time (so we fall back to system default now but auto-re-attach
    // when the device reappears), and also from explicit user switches.
    // Empty = no preference (follow macOS default, don't auto-switch on
    // hot-plug). mDeviceMode tracks "current selection regardless of
    // whether device is present"; mPreferredOutputDevice tracks "want to
    // use this device whenever it's available".
    std::string              mPreferredOutputDevice;
    // Same for the input sub-device in an aggregate.
    std::string              mPreferredInputDevice;
    std::string              mLastInputDeviceName;    // saved on disable, restored on re-enable
    std::string              mRealOutputDeviceName;   // actual output device behind aggregate
    std::string              mRealInputDeviceName;    // actual input device behind aggregate
    std::mutex               mSwapMutex;
    std::chrono::steady_clock::time_point mLastSelfTriggeredChange{}; // suppress async change notifications from our own setAudioDeviceSetup

    // listDrivers() cache — avoids re-scanning every AudioIODeviceType
    // on every /supersonic/info push. Re-scanning is expensive (each
    // scan touches JACK / ASIO / etc. whether or not they're usable)
    // and, on Linux without a JACK server, produces stderr spam from
    // libjack's connect() failures. Cache TTL is short so a user who
    // starts jackd mid-session sees JACK reappear within a few seconds.
    mutable std::mutex                           mListDriversMutex;
    mutable std::vector<std::string>             mCachedDrivers;
    mutable std::chrono::steady_clock::time_point mCachedDriversAt{};
    // Cache for listDevices(false). Building this list calls JUCE's
    // type->createDevice() + initialise() per device — on Windows that's a
    // full WASAPI IAudioClient activation each time, ~50–100 ms per device.
    // With ~150 device/type combinations on a typical machine the call takes
    // ~10 s, which during boot starves the OSC thread and causes spider's
    // /supersonic/notify handshake to time out. Cache invalidated by device-
    // change events (audioDeviceListChanged) and by listDevices(true).
    mutable std::mutex                           mListDevicesMutex;
    mutable std::vector<DeviceInfo>              mCachedDevices;
    mutable std::chrono::steady_clock::time_point mCachedDevicesAt{};
    // Pause CFRunLoop pumping in Main.cpp during aggregate destroy/create
    // — queued audioDeviceListChanged messages would trigger a second
    // cold swap and crash ScopeOut2 during the rebuild. Accessed from
    // both the engine (writer during aggregate transitions) and Main's
    // CFRunLoop pump (reader at every tick), so an atomic is needed.
    std::atomic<bool>        mSuppressRunLoop{false};
    std::string              mDeviceMode;   // empty = system/auto, non-empty = manual device name
    bool                     mWorldRebuilt{false};
    std::map<std::string, int> mDeviceRateMemory; // per-device remembered sample rate

    // Shared memory — owned by the engine, survives across cold swaps.
    std::unique_ptr<server_shared_memory_creator> mShmemCreator;

    // Recording
    juce::TimeSliceThread    mRecordThread{"SuperSonic-RecordIO"};
    std::string              mRecordPath;
};

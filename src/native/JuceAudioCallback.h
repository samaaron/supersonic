/*
 * JuceAudioCallback.h — JUCE audio driver bridge
 */
#pragma once

#include <juce_audio_devices/juce_audio_devices.h>
#include "WallClock.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <vector>

class SampleLoader;
class SuperClock;

extern "C" {
    bool process_audio(double current_time, uint32_t active_output_channels, uint32_t active_input_channels);
    void init_memory(double sample_rate);
    void set_time_offset(double offset);
    uintptr_t get_audio_output_bus();
    uintptr_t get_audio_input_bus();
    int get_audio_buffer_samples();
    uintptr_t get_audio_bus_pool();
    int get_audio_bus_count();
    int get_audio_first_private_bus_idx();
    void touch_audio_bus(uint32_t busIdx);
    void touch_audio_bus_for_next_block(uint32_t busIdx);
}

// Shared per-scsynth-block render, used by both HeadlessDriver and the engine's
// manual pump (SupersonicEngine::pumpAudioBlock): drain Link Audio inputs into
// the bus pool, run process_audio, then publish the main + aux Link sinks. The
// caller owns installPendingBuffers(), the NTP/host-time derivation, the
// samplePos advance, and the processCount tick — only the block body lives here
// so the two drivers can't drift apart.
void renderAudioBlock(SuperClock& clock,
                      uint32_t blockSize,
                      uint32_t numOutputChannels,
                      uint32_t numInputChannels,
                      uint32_t sampleRate,
                      double   ntp,
                      uint64_t hostMicros);

class JuceAudioCallback : public juce::AudioIODeviceCallback {
public:
    JuceAudioCallback();
    ~JuceAudioCallback() override = default;

    void initialiseWorld(uint8_t* ringBufferStorage,
                         int sampleRate,
                         int numOutputChannels,
                         int numInputChannels,
                         int numBuffers,
                         int maxNodes,
                         int maxGraphDefs,
                         int maxWireBufs,
                         int numAudioBusChannels,
                         int numControlBusChannels,
                         int realTimeMemorySize,
                         int numRGens,
                         int sharedMemoryID = 0,
                         int bufLen        = 0);  // 0 = use kDefaultBlockSize

    // scsynth's audio block size — read at audio-thread frequency.
    int bufferLength() const { return mBufLen; }

    // Wire the SampleLoader so installPendingBuffers() runs on the audio thread
    void setSampleLoader(SampleLoader* loader) { mSampleLoader = loader; }

    // Wire the engine-owned SuperClock. SuperClock owns audio-thread NTP
    // derivation (the IIR previously inlined here, now in SuperClockNative).
    // Must be called before any audio callback or initialiseWorld.
    void setSuperClock(SuperClock* sc) { mSuperClock = sc; }

    // C++20 atomic wait — equivalent of JS Atomics.wait()/notify()
    std::atomic<uint32_t> processCount{0};

    // Nominal rate of the currently-open device (set in
    // audioDeviceAboutToStart; 0 before the first device). Atomic because the
    // watchdog's rate-skew check reads it off the control thread.
    int nominalSampleRate() const {
        return mNominalRate.load(std::memory_order_relaxed);
    }

    void audioDeviceAboutToStart(juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;
    void audioDeviceIOCallbackWithContext(
        const float* const* inputChannelData,
        int numInputChannels,
        float* const* outputChannelData,
        int numOutputChannels,
        int numSamples,
        const juce::AudioIODeviceCallbackContext& context) override;

    // --- Pause/resume for device swap ---
    void pause();
    void resume();
    bool isPaused() const;

    // --- Gap detector state (for testing) ---
    // Returns true if the inter-callback gap detector has a baseline timestamp,
    // meaning the next callback will measure the gap since that timestamp.
    bool gapDetectorArmed() const { return mLastCbTime.time_since_epoch().count() != 0; }
    // Arm the gap detector by recording the current time as baseline.
    void armGapDetector() { mLastCbTime = std::chrono::high_resolution_clock::now(); }

    // Read wall clock as NTP seconds (sub-millisecond precision).
    // Public + static so HeadlessDriver can reuse the same implementation.
    static double wallClockNTP() { return ::wallClockNTP(); }

    // --- Pre-tick hook (for tau integration) ---
    // Called before process_audio() each 128-sample block.
    // samplePosition: cumulative samples processed (for beat calculation)
    // wallMs: wall clock time in milliseconds
    std::function<void(double samplePosition, double wallMs)> preTick;

    // --- Wake hook ---
    // Called when a sleep/wake cycle is detected (callback gap > 2 seconds).
    // Used to purge stale messages from the ring buffer and scheduler.
    std::function<void()> onWake;

    // --- Recording ---
    // Atomic void* — audio thread casts to juce::AudioFormatWriter::ThreadedWriter*.
    // Non-audio thread stores/clears. Lock-free FIFO push in write().
    std::atomic<void*> mRecordWriter{nullptr};

private:
    // scsynth's audio block size — the number of samples the graph
    // processes per tick. Matches the hardware callback size when set at
    // init so the process loop is 1:1 (no accumulator / prefetch dance).
    // Capped at sonicpi::kMaxBlockSize because static_audio_bus in
    // audio_processor.cpp is sized at that max. Initialised in
    // initialiseWorld from the caller's chosen value (typically HW buffer
    // size) and honoured thereafter.
    int mBufLen = 128;

    SampleLoader* mSampleLoader = nullptr;
    SuperClock*   mSuperClock   = nullptr;
    uint8_t*   mRingBufferStorage  = nullptr;
    int        mSampleRate         = 48000;
    int        mNumOutputChannels  = 2;
    int        mNumInputChannels   = 2;
    // Input bus width the World was built with (immutable). mNumInputChannels
    // tracks the live device and can exceed it; the input-feed loop clamps to this.
    int        mWorldInputBusChannels = 0;
    // Output bus width the World was built with (immutable). mNumOutputChannels
    // tracks the live device and can exceed it after a hot swap to a wider
    // device; the render loop clamps to this so channels the World never
    // renders emit silence instead of stale bus contents.
    int        mWorldOutputBusChannels = 0;
    double     mSamplePosition     = 0.0;   // cumulative samples (increments by mBufLen)
    int        mOutputLatencySamples = 0;   // device DSP→DAC latency, captured at start

    // Prefetch buffer: channel-major, mBufLen samples per channel. Only
    // populated when the HW callback wants fewer samples than a scsynth
    // block (rare — happens if HW buffer shrinks after World init).
    std::vector<float> mPrefetchBuf;
    int   mPrefetchCount = 0;

    // Input accumulator: needed only when HW callback size < mBufLen.
    // In the typical case (HW buffer == block size) mInputAccumCount is
    // always == mBufLen after one callback and the accumulator is a
    // no-op copy. Layout: channel-major, capacity sized to hold at
    // least HW + block.
    std::vector<float> mInputAccum;
    int   mInputAccumCount = 0;
    int   mAccumPerChanCap = 0;

    std::atomic<bool> mPaused{false};
    std::atomic<int>  mNominalRate{0};   // see nominalSampleRate()

    // One-shot guard for promoting the audio thread to realtime. Reset in
    // audioDeviceAboutToStart (control thread) so each device (re)start
    // re-promotes the possibly-new audio thread; acted on in the first
    // audioDeviceIOCallbackWithContext, which runs on the audio thread itself.
    std::atomic<bool> mRealtimeElevated{false};

    // One-shot per device start (same reset pattern as mRealtimeElevated):
    // logs the moment the device's IO thread first reaches our callback.
    // Boot-diagnostic: a process death between "audio callback attached"
    // and this line means the driver/HAL never delivered a callback.
    std::atomic<bool> mFirstCallbackLogged{false};

    // Audio thread timing stats (accessed only from audio thread, no atomics needed)
    uint32_t mCallbackCount = 0;
    uint32_t mOverrunCount  = 0;
    double   mTotalUs       = 0.0;
    double   mMaxUs         = 0.0;
    double   mLastOverrunLogSec = 0.0;  // rate-limits [overrun] log lines

    // DSP load published to native-stats: a smoothed average (EMA) and a decaying
    // peak of per-callback load (callback time / time budget), in percent.
    double   mLoadAvgPct    = 0.0;
    double   mLoadPeakPct   = 0.0;

    // Inter-callback gap detector baseline (used by sleep/wake recovery)
    std::chrono::high_resolution_clock::time_point mLastCbTime{};

};

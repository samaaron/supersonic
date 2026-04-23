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

extern "C" {
    bool process_audio(double current_time, uint32_t active_output_channels, uint32_t active_input_channels);
    void init_memory(double sample_rate);
    void set_time_offset(double offset);
    uintptr_t get_audio_output_bus();
    uintptr_t get_audio_input_bus();
    int get_audio_buffer_samples();
}

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

    // C++20 atomic wait — equivalent of JS Atomics.wait()/notify()
    std::atomic<uint32_t> processCount{0};

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
    uint8_t*   mRingBufferStorage  = nullptr;
    int        mSampleRate         = 48000;
    int        mNumOutputChannels  = 2;
    int        mNumInputChannels   = 2;
    double     mSamplePosition     = 0.0;   // cumulative samples (increments by mBufLen)

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

    // Sample-position-based NTP clock.
    // We capture a high-resolution wall-clock baseline at device start, then
    // derive NTP purely from sample counting: ntp = mBaseNTP + samples/rate.
    // This eliminates jitter from OS scheduling and millisecond-precision clocks.
    // A slow drift correction keeps long-term sync with wall clock.
    double mBaseNTP = 0.0;   // NTP time corresponding to mSamplePosition == 0

    // Audio thread timing stats (accessed only from audio thread, no atomics needed)
    uint32_t mCallbackCount = 0;
    uint32_t mOverrunCount  = 0;
    double   mTotalUs       = 0.0;
    double   mMaxUs         = 0.0;

    // Inter-callback gap detector baseline (used by sleep/wake recovery)
    std::chrono::high_resolution_clock::time_point mLastCbTime{};

};

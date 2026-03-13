/*
 * JuceAudioCallback.h — JUCE audio driver bridge
 */
#pragma once

#include <juce_audio_devices/juce_audio_devices.h>
#include <atomic>
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
                         int sharedMemoryID = 0);

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

    // --- Pre-tick hook (for tau integration) ---
    // Called before process_audio() each 128-sample block.
    // samplePosition: cumulative samples processed (for beat calculation)
    // wallMs: wall clock time in milliseconds
    std::function<void(double samplePosition, double wallMs)> preTick;

    // --- Recording ---
    // Atomic void* — audio thread casts to juce::AudioFormatWriter::ThreadedWriter*.
    // Non-audio thread stores/clears. Lock-free FIFO push in write().
    std::atomic<void*> mRecordWriter{nullptr};

private:
    // scsynth's internal block size is hardcoded to 128 in audio_processor.cpp
    // (QUANTUM_SIZE = 128, static_audio_bus[128*128]).
    // We accumulate 128-sample blocks into a prefetch buffer to serve whatever
    // size JUCE requests without ever dropping samples.
    static constexpr int kBufLen = 128;

    SampleLoader* mSampleLoader = nullptr;
    uint8_t*   mRingBufferStorage  = nullptr;
    int        mSampleRate         = 48000;
    int        mNumOutputChannels  = 2;
    int        mNumInputChannels   = 2;
    double     mSamplePosition     = 0.0;   // cumulative samples (increments by kBufLen)

    // Prefetch buffer: dynamically sized at init (numOutputChannels * kBufLen).
    // Layout: channel-major, kBufLen samples per channel.
    std::vector<float> mPrefetchBuf;
    int   mPrefetchCount = 0;

    std::atomic<bool> mPaused{false};

    // Audio thread timing stats (accessed only from audio thread, no atomics needed)
    int    mCallbackCount = 0;
    int    mOverrunCount  = 0;
    double mTotalUs       = 0.0;
    double mMaxUs         = 0.0;

};

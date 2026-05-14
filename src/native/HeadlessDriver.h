/*
 * HeadlessDriver.h — Timer-driven audio processing without a real audio device.
 *
 * When no audio hardware is available (headless mode), this thread calls
 * process_audio() at the correct block rate (matches whatever block size
 * the engine configured, typically 128 samples), using platform-specific
 * high-resolution timers for accurate timing:
 *
 *   Linux:   clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME)
 *   macOS:   mach_wait_until()
 *   Windows: WaitableTimer with CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
 *
 * Worker threads (ReplyReader, DebugReader) are woken after each block,
 * so OSC replies flow exactly as they would with a real audio device.
 */
#pragma once

#include <juce_core/juce_core.h>
#include <cstdint>

class JuceAudioCallback;
class SampleLoader;

class HeadlessDriver : public juce::Thread {
public:
    HeadlessDriver();

    void configure(JuceAudioCallback* callback,
                   SampleLoader* sampleLoader,
                   int sampleRate,
                   int blockSize,
                   int numOutputChannels,
                   int numInputChannels);

    void run() override;

private:
    // Shared loop body: install buffers, derive NTP, process audio, wake workers.
    // Called once per block from the platform-specific run() loop.
    void processBlock(double& baseNTP, double& samplePos);

    // Ticks the engine at this block size. Set explicitly from
    // SupersonicEngine::Config so the tick rate is deterministic and
    // independent of whatever buffer size a transient real device may
    // have left on the audio callback (e.g. failed-init fallback path).
    int mBlockSize = 128;

    JuceAudioCallback* mCallback         = nullptr;
    SampleLoader*      mSampleLoader     = nullptr;
    int                mSampleRate        = 48000;
    int                mNumOutputChannels = 2;
    int                mNumInputChannels  = 0;
};

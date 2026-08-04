/*
 * RecordWriter.h — threaded session-recorder file writer (WAV/FLAC).
 *
 * Lock-free FIFO fed from the audio thread, drained on a juce::TimeSliceThread
 * into libsndfile. Replaces juce_audio_formats' AudioFormatWriter::ThreadedWriter
 * — that module is GPL-dual and deliberately not vendored in SuperSmoothy.
 *
 * Threading contract (same as the ThreadedWriter it replaces): write() is
 * audio-thread only; construction/destruction happen on a control thread with
 * the audio callback paused or the writer pointer already detached.
 */
#pragma once

#include <juce_core/juce_core.h>
#include <sndfile.h>
#include <string>
#include <vector>

class RecordWriter : private juce::TimeSliceClient {
public:
    // bufferFrames sizes the FIFO (e.g. 10 s of audio). Unsupported
    // format/bitDepth combinations or an unopenable path leave openedOk()
    // false. Supported: wav 16/24/32(float), flac 16/24.
    RecordWriter(const std::string& path, const std::string& format,
                 int bitDepth, double sampleRate, int numChannels,
                 juce::TimeSliceThread& ioThread, int bufferFrames);
    ~RecordWriter() override;

    bool openedOk() const { return mFile != nullptr; }

    // Audio thread. Interleaves into the FIFO; returns false (dropping the
    // block) when the FIFO is full — never blocks.
    bool write(const float* const* channels, int numSamples);

private:
    int useTimeSlice() override;
    void drain(bool flushAll);

    SNDFILE* mFile = nullptr;
    juce::TimeSliceThread& mThread;
    int mChannels = 0;
    juce::AbstractFifo mFifo;   // counts frames
    std::vector<float> mRing;   // interleaved frame ring, mFifo indexes it

    JUCE_DECLARE_NON_COPYABLE(RecordWriter)
};

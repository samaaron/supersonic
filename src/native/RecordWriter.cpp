#include "RecordWriter.h"

#include <algorithm>

RecordWriter::RecordWriter(const std::string& path, const std::string& format,
                           int bitDepth, double sampleRate, int numChannels,
                           juce::TimeSliceThread& ioThread, int bufferFrames)
    : mThread(ioThread), mChannels(numChannels),
      mFifo(std::max(1024, bufferFrames)) {
    int fmt = 0;
    if (format == "flac") {
        if (bitDepth == 16)      fmt = SF_FORMAT_FLAC | SF_FORMAT_PCM_16;
        else if (bitDepth == 24) fmt = SF_FORMAT_FLAC | SF_FORMAT_PCM_24;
    } else {
        if (bitDepth == 16)      fmt = SF_FORMAT_WAV | SF_FORMAT_PCM_16;
        else if (bitDepth == 24) fmt = SF_FORMAT_WAV | SF_FORMAT_PCM_24;
        else if (bitDepth == 32) fmt = SF_FORMAT_WAV | SF_FORMAT_FLOAT;
    }
    if (fmt == 0 || numChannels <= 0 || sampleRate <= 0)
        return;

    SF_INFO info{};
    info.samplerate = static_cast<int>(sampleRate);
    info.channels   = numChannels;
    info.format     = fmt;
    mFile = sf_open(path.c_str(), SFM_WRITE, &info);
    if (!mFile)
        return;

    mRing.resize(static_cast<size_t>(mFifo.getTotalSize())
                 * static_cast<size_t>(numChannels));
    mThread.addTimeSliceClient(this);
}

RecordWriter::~RecordWriter() {
    if (!mFile)
        return;
    // removeTimeSliceClient blocks until any in-flight slice completes, so
    // drain/sf_close below can't race useTimeSlice. The audio thread has
    // already been detached from this writer by the caller.
    mThread.removeTimeSliceClient(this);
    drain(true);
    sf_close(mFile);
}

bool RecordWriter::write(const float* const* channels, int numSamples) {
    if (mFile == nullptr || numSamples <= 0)
        return false;

    int s1, n1, s2, n2;
    mFifo.prepareToWrite(numSamples, s1, n1, s2, n2);
    if (n1 + n2 < numSamples) {
        mFifo.finishedWrite(0);
        return false;
    }

    const auto interleave = [&](int startFrame, int numFrames, int srcOffset) {
        for (int f = 0; f < numFrames; ++f) {
            float* dst = mRing.data()
                + static_cast<size_t>(startFrame + f) * mChannels;
            for (int c = 0; c < mChannels; ++c)
                dst[c] = channels[c] != nullptr ? channels[c][srcOffset + f]
                                                : 0.0f;
        }
    };
    interleave(s1, n1, 0);
    interleave(s2, n2, n1);
    mFifo.finishedWrite(n1 + n2);
    return true;
}

void RecordWriter::drain(bool flushAll) {
    for (;;) {
        int s1, n1, s2, n2;
        mFifo.prepareToRead(mFifo.getNumReady(), s1, n1, s2, n2);
        if (n1 + n2 == 0)
            return;
        if (n1 > 0)
            sf_writef_float(mFile,
                mRing.data() + static_cast<size_t>(s1) * mChannels, n1);
        if (n2 > 0)
            sf_writef_float(mFile,
                mRing.data() + static_cast<size_t>(s2) * mChannels, n2);
        mFifo.finishedRead(n1 + n2);
        if (!flushAll)
            return;
    }
}

int RecordWriter::useTimeSlice() {
    drain(false);
    return 10;
}

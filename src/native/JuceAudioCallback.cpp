/*
 * JuceAudioCallback.cpp
 */
#include "JuceAudioCallback.h"
#include "SampleLoader.h"
#include "src/shared_memory.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <cstring>
#include <chrono>

static constexpr double NTP_EPOCH_OFFSET = 2208988800.0;

double JuceAudioCallback::wallClockNTP() {
    auto now = std::chrono::system_clock::now();
    double secsSinceEpoch = std::chrono::duration<double>(
        now.time_since_epoch()).count();
    return secsSinceEpoch + NTP_EPOCH_OFFSET;
}

JuceAudioCallback::JuceAudioCallback() = default;

void JuceAudioCallback::initialiseWorld(uint8_t* ringBufferStorage,
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
                                         int sharedMemoryID)
{
    mRingBufferStorage = ringBufferStorage;
    mSampleRate        = sampleRate;
    mNumOutputChannels = numOutputChannels;
    mNumInputChannels  = numInputChannels;

    // Allocate prefetch buffer for the configured output channel count
    mPrefetchBuf.assign(static_cast<size_t>(numOutputChannels) * kBufLen, 0.0f);

    // Write WorldOptions at fixed offset +65536 (mirrors JS layout)
    uint32_t* opts = reinterpret_cast<uint32_t*>(ringBufferStorage + 65536);
    opts[0]  = static_cast<uint32_t>(numBuffers);
    opts[1]  = static_cast<uint32_t>(maxNodes);
    opts[2]  = static_cast<uint32_t>(maxGraphDefs);
    opts[3]  = static_cast<uint32_t>(maxWireBufs);
    opts[4]  = static_cast<uint32_t>(numAudioBusChannels);
    opts[5]  = static_cast<uint32_t>(numInputChannels);  // mNumInputBusChannels
    opts[6]  = static_cast<uint32_t>(numOutputChannels); // mNumOutputBusChannels
    opts[7]  = static_cast<uint32_t>(numControlBusChannels);
    opts[8]  = 128;    // bufLength — MUST be 128: audio_processor.cpp QUANTUM_SIZE is hardcoded
    opts[9]  = static_cast<uint32_t>(realTimeMemorySize);
    opts[10] = static_cast<uint32_t>(numRGens);
    opts[11] = 0;      // realTime flag — always false (NRT/SAB mode)
    opts[12] = 0;      // memoryLocking
    opts[13] = 1;      // loadGraphDefs
    opts[14] = static_cast<uint32_t>(sampleRate);
    opts[15] = 0;      // verbosity
    opts[16] = 0;      // mode: 0 = direct memory access (SAB path in web, direct pointers in native)
    opts[17] = static_cast<uint32_t>(sharedMemoryID);  // mSharedMemoryID (UDP port for boost shm)

    init_memory(static_cast<double>(sampleRate));
}

void JuceAudioCallback::audioDeviceAboutToStart(juce::AudioIODevice* device) {
    mSampleRate     = static_cast<int>(device->getCurrentSampleRate());
    mSamplePosition = 0.0;
    mPrefetchCount  = 0;
    mBaseNTP        = wallClockNTP();
    std::fill(mPrefetchBuf.begin(), mPrefetchBuf.end(), 0.0f);

    // Native timing: set ntp_start and drift to 0. NTP is derived from sample
    // position with slow drift correction (see run loop), so these offsets are unused.
    if (mRingBufferStorage) {
        double* ntpStartPtr = reinterpret_cast<double*>(
            mRingBufferStorage + NTP_START_TIME_START);
        *ntpStartPtr = 0.0;

        auto* driftPtr = reinterpret_cast<std::atomic<int32_t>*>(
            mRingBufferStorage + DRIFT_OFFSET_START);
        driftPtr->store(0, std::memory_order_relaxed);
    }

}

void JuceAudioCallback::audioDeviceStopped() {
    mSamplePosition = 0.0;
    mPrefetchCount  = 0;
}

// --- Pause/resume ---

void JuceAudioCallback::pause() {
    mPaused.store(true, std::memory_order_release);
}

void JuceAudioCallback::resume() {
    mSamplePosition = 0.0;
    mPrefetchCount  = 0;
    mBaseNTP        = wallClockNTP();
    mCallbackCount  = 0;       // re-arm warmup for new device
    mLastCbTime     = {};      // clear gap detector baseline
    mOverrunCount   = 0;
    mTotalUs        = 0.0;
    mMaxUs          = 0.0;
    mPaused.store(false, std::memory_order_release);
}

bool JuceAudioCallback::isPaused() const {
    return mPaused.load(std::memory_order_acquire);
}

void JuceAudioCallback::audioDeviceIOCallbackWithContext(
    const float* const* inputChannelData,
    int numInputChannels,
    float* const* outputChannelData,
    int numOutputChannels,
    int numSamples,
    const juce::AudioIODeviceCallbackContext& /*context*/)
{
    int nIn  = juce::jmin(numInputChannels,  mNumInputChannels);
    int nOut = juce::jmin(numOutputChannels, mNumOutputChannels);

    // Zero any extra output channels that scsynth won't fill
    for (int ch = nOut; ch < numOutputChannels; ++ch)
        if (outputChannelData[ch])
            std::memset(outputChannelData[ch], 0,
                        static_cast<size_t>(numSamples) * sizeof(float));

    // ── Warmup: output silence for the first few callbacks to absorb page faults
    if (mCallbackCount < 4) {
        for (int ch = 0; ch < nOut; ++ch)
            if (outputChannelData[ch])
                std::memset(outputChannelData[ch], 0,
                            static_cast<size_t>(numSamples) * sizeof(float));
        mCallbackCount++;
        processCount.fetch_add(1, std::memory_order_release);
        processCount.notify_all();
        return;
    }

    // ── If paused, output silence but keep worker threads alive ───────────
    if (mPaused.load(std::memory_order_acquire)) {
        for (int ch = 0; ch < nOut; ++ch)
            if (outputChannelData[ch])
                std::memset(outputChannelData[ch], 0,
                            static_cast<size_t>(numSamples) * sizeof(float));
        processCount.fetch_add(1, std::memory_order_release);
        processCount.notify_all();
        return;
    }

    // ── Timing measurement ────────────────────────────────────────────────────
    auto cbStart = std::chrono::high_resolution_clock::now();

    // Sleep/wake recovery: if gap > 2 seconds, re-anchor NTP timing
    // and purge stale messages so the engine doesn't try to catch up
    if (mLastCbTime.time_since_epoch().count() != 0) {
        double gapUs = std::chrono::duration<double, std::micro>(cbStart - mLastCbTime).count();
        if (gapUs > 2'000'000.0) {
            fprintf(stderr, "  [wake] gap=%.0fs — re-anchoring NTP, purging stale messages\n",
                    gapUs / 1e6);
            fflush(stderr);
            mBaseNTP = wallClockNTP() - mSamplePosition / mSampleRate;
            if (onWake) onWake();
        }
    }
    mLastCbTime = cbStart;

    // ── 0. Install any buffers decoded by the SampleLoader I/O thread ─────────
    // Mirrors the WASM architecture: buffer installation + /done reply happen
    // on the audio thread, keeping the OUT ring buffer single-producer.
    if (mSampleLoader)
        mSampleLoader->installPendingBuffers();

    int outputFilled  = 0;
    int inputConsumed = 0;
    float* prefBase   = mPrefetchBuf.data();

    // ── 1. Drain leftover samples from the previous callback ─────────────────
    if (mPrefetchCount > 0) {
        int toDrain = std::min(mPrefetchCount, numSamples);
        for (int ch = 0; ch < nOut; ++ch)
            if (outputChannelData[ch])
                std::memcpy(outputChannelData[ch],
                            prefBase + ch * kBufLen,
                            static_cast<size_t>(toDrain) * sizeof(float));
        // Shift remaining prefetch samples down
        if (toDrain < mPrefetchCount) {
            int remaining = mPrefetchCount - toDrain;
            for (int ch = 0; ch < nOut; ++ch)
                std::memmove(prefBase + ch * kBufLen,
                             prefBase + ch * kBufLen + toDrain,
                             static_cast<size_t>(remaining) * sizeof(float));
        }
        mPrefetchCount -= toDrain;
        outputFilled    = toDrain;
    }

    // ── 2. Generate 128-sample scsynth blocks until the JUCE buffer is full ──
    // Derive NTP from sample position for jitter-free timing.  The sample
    // counter advances by exactly kBufLen per block, so NTP progresses
    // perfectly smoothly — immune to OS scheduling jitter and clock
    // quantisation (juce::Time::currentTimeMillis() is only 1ms precision).
    //
    // A slow drift correction (~1% per callback) keeps long-term sync with
    // the wall clock, compensating for sample-rate/wall-clock drift (ppm-level).
    double wallNow = wallClockNTP();
    double sampleNTP = mBaseNTP + mSamplePosition / mSampleRate;
    double drift = wallNow - sampleNTP;
    mBaseNTP += drift * 0.01;  // low-pass filter: converge ~1% per callback

    double wallNTP = mBaseNTP + mSamplePosition / mSampleRate;

    while (outputFilled < numSamples) {
        // Copy JUCE input into scsynth's input bus (channel-major, kBufLen per ch).
        // If we overshoot the available input (due to prefetch alignment), zero-pad.
        auto* inputBus = reinterpret_cast<float*>(get_audio_input_bus());
        if (inputBus) {
            for (int ch = 0; ch < nIn; ++ch) {
                int avail = std::max(0, std::min(kBufLen, numSamples - inputConsumed));
                if (avail > 0 && inputChannelData[ch])
                    std::memcpy(inputBus + ch * kBufLen,
                                inputChannelData[ch] + inputConsumed,
                                static_cast<size_t>(avail) * sizeof(float));
                if (avail < kBufLen)
                    std::memset(inputBus + ch * kBufLen + avail, 0,
                                static_cast<size_t>(kBufLen - avail) * sizeof(float));
            }
        }
        inputConsumed += kBufLen;

        // Pre-tick hook (for tau integration)
        if (preTick)
            preTick(mSamplePosition, wallNTP * 1000.0 - NTP_EPOCH_OFFSET * 1000.0);

        // Native timing: pass wall clock NTP time directly.
        // process_audio computes: current_ntp = current_time + ntp_start + drift + global
        // With ntp_start=0 and drift=0, this becomes: wallNTP + global_offset
        // Advance NTP by one block duration for each sub-block
        process_audio(wallNTP,
                      static_cast<uint32_t>(mNumOutputChannels),
                      static_cast<uint32_t>(mNumInputChannels));
        wallNTP += static_cast<double>(kBufLen) / mSampleRate;
        mSamplePosition += kBufLen;

        auto* outputBus = reinterpret_cast<float*>(get_audio_output_bus());
        if (outputBus) {
            int needed  = numSamples - outputFilled;
            int toCopy  = std::min(needed, kBufLen);

            for (int ch = 0; ch < nOut; ++ch)
                if (outputChannelData[ch])
                    std::memcpy(outputChannelData[ch] + outputFilled,
                                outputBus + ch * kBufLen,
                                static_cast<size_t>(toCopy) * sizeof(float));
            outputFilled += toCopy;

            // Save any leftover samples for the next JUCE callback
            int leftover = kBufLen - toCopy;
            if (leftover > 0) {
                for (int ch = 0; ch < nOut; ++ch)
                    std::memcpy(prefBase + ch * kBufLen,
                                outputBus + ch * kBufLen + toCopy,
                                static_cast<size_t>(leftover) * sizeof(float));
                mPrefetchCount = leftover;
            }
        }
    }

    // ── 3. Recording tap — lock-free FIFO push to background writer ──────────
    auto* recWriter = static_cast<juce::AudioFormatWriter::ThreadedWriter*>(
        mRecordWriter.load(std::memory_order_acquire));
    if (recWriter)
        recWriter->write(outputChannelData, numSamples);

    // ── 4. Timing stats (no I/O on audio thread — store atomically for external query) ──
    auto cbEnd = std::chrono::high_resolution_clock::now();
    double cbUs = std::chrono::duration<double, std::micro>(cbEnd - cbStart).count();
    double budgetUs = (static_cast<double>(numSamples) / mSampleRate) * 1e6;

    mCallbackCount++;
    mTotalUs += cbUs;
    if (cbUs > mMaxUs) mMaxUs = cbUs;
    if (cbUs > budgetUs) mOverrunCount++;

    // ── 5. Notify worker threads (one tick per JUCE callback) ─────────────────
    processCount.fetch_add(1, std::memory_order_release);
    processCount.notify_all();
}

/*
 * JuceAudioCallback.cpp
 */
#include "JuceAudioCallback.h"
#include "WallClock.h"
#include "SampleLoader.h"
#include "shared_memory.h"
#include "audio_config.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <algorithm>
#include <cstring>

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
                                         int sharedMemoryID,
                                         int bufLen)
{
    mRingBufferStorage = ringBufferStorage;
    mSampleRate        = sampleRate;
    mNumOutputChannels = numOutputChannels;
    mNumInputChannels  = numInputChannels;

    // Choose the scsynth block size. bufLen == 0 means "use platform
    // default" (always 128 on web due to AudioWorklet; starting value on
    // native — audioDeviceAboutToStart will optionally resize). Clamp
    // into [32, kMaxBlockSize] because static_audio_bus is sized at the
    // max, and too-small blocks waste control-rate density.
    int chosen = (bufLen > 0) ? bufLen : sonicpi::kDefaultBlockSize;
    chosen = std::clamp(chosen, 32, sonicpi::kMaxBlockSize);
    mBufLen = chosen;

    // Allocate prefetch buffer for the configured output channel count
    mPrefetchBuf.assign(static_cast<size_t>(numOutputChannels) * mBufLen, 0.0f);

    // Initial accumulator: 2*mBufLen per channel. audioDeviceAboutToStart
    // will grow it if the HW buffer size exceeds this.
    int inChans = std::max(1, numInputChannels);
    mAccumPerChanCap = mBufLen * 2;
    mInputAccum.assign(static_cast<size_t>(inChans) * mAccumPerChanCap, 0.0f);
    mInputAccumCount = 0;

    // Write WorldOptions at WORLD_OPTIONS_START (safe offset outside ring buffers)
    uint32_t* opts = reinterpret_cast<uint32_t*>(ringBufferStorage + WORLD_OPTIONS_START);
    using namespace sonicpi::WorldOpts;
    opts[kNumBuffers]            = static_cast<uint32_t>(numBuffers);
    opts[kMaxNodes]              = static_cast<uint32_t>(maxNodes);
    opts[kMaxGraphDefs]          = static_cast<uint32_t>(maxGraphDefs);
    opts[kMaxWireBufs]           = static_cast<uint32_t>(maxWireBufs);
    opts[kNumAudioBusChannels]   = static_cast<uint32_t>(numAudioBusChannels);
    opts[kNumInputBusChannels]   = static_cast<uint32_t>(numInputChannels);
    opts[kNumOutputBusChannels]  = static_cast<uint32_t>(numOutputChannels);
    opts[kNumControlBusChannels] = static_cast<uint32_t>(numControlBusChannels);
    opts[kBufLength]             = static_cast<uint32_t>(mBufLen);
    opts[kRealTimeMemorySize]    = static_cast<uint32_t>(realTimeMemorySize);
    opts[kNumRGens]              = static_cast<uint32_t>(numRGens);
    opts[kRealTime]              = 0;  // always false (NRT/SAB mode)
    opts[kMemoryLocking]         = 0;
    opts[kLoadGraphDefs]         = 1;
    opts[kSampleRate]            = static_cast<uint32_t>(sampleRate);
    opts[kVerbosity]             = 0;
    opts[kMode]                  = 0;  // direct memory access (SAB path in web, direct pointers in native)
    opts[17] = static_cast<uint32_t>(sharedMemoryID);  // mSharedMemoryID — index differs between web (18) and native (17); keep raw

    init_memory(static_cast<double>(sampleRate));
}

void JuceAudioCallback::audioDeviceAboutToStart(juce::AudioIODevice* device) {
    mSampleRate     = static_cast<int>(device->getCurrentSampleRate());
    mSamplePosition = 0.0;
    mPrefetchCount  = 0;
    mInputAccumCount = 0;
    mBaseNTP        = wallClockNTP();
    std::fill(mPrefetchBuf.begin(), mPrefetchBuf.end(), 0.0f);

    // Sync our internal channel counts from what the device actually opened.
    // mNumInput/OutputChannels are what we tell scsynth (via process_audio's
    // active channels args) and what we clamp JUCE's channel arrays to.
    // Without this, initialiseWorld()'s boot-time values stick forever even
    // after cold swaps that change the channel count (e.g. re-enabling inputs
    // after a mic-permission grant). Result: hw mic delivers samples but
    // scsynth never sees them — worldIn=0 in [audio-input] telemetry.
    int activeIn  = device->getActiveInputChannels().countNumberOfSetBits();
    int activeOutCount = device->getActiveOutputChannels().countNumberOfSetBits();
    if (activeIn > 0)       mNumInputChannels  = activeIn;
    if (activeOutCount > 0) mNumOutputChannels = activeOutCount;

    // Resize mPrefetchBuf if the new device exposes more output channels
    // than we allocated at initialiseWorld. mPrefetchBuf is indexed per
    // channel as prefBase + ch * mBufLen — writing to a ch beyond the
    // original allocation corrupts memory after the HW-buffer < mBufLen
    // prefetch path runs. Example: boot on a 2-ch device (buffer sized
    // for 2), cold-swap to 4-ch Loopback, prefetch writes at ch=2 / ch=3
    // fall off the end of the buffer.
    {
        size_t needed = static_cast<size_t>(mNumOutputChannels) * mBufLen;
        if (mPrefetchBuf.size() < needed)
            mPrefetchBuf.assign(needed, 0.0f);
    }

    // Size the input accumulator to hold at least one full HW callback's
    // worth of samples plus a scsynth block. HW buffers > 2*mBufLen (e.g.
    // 512 or 1024) would otherwise overflow the default 256-sample
    // accumulator and corrupt memory in the overflow branch.
    //
    // Two independent growth dimensions: per-channel capacity AND
    // channel count. The accumulator is channel-major — the callback
    // addresses it as `accumBase + ch * mAccumPerChanCap` — so the
    // vector must hold mAccumPerChanCap * chans floats. A cold-swap
    // from a 2-ch device to an 8-ch device at the same hardware
    // buffer size leaves mAccumPerChanCap unchanged but doubles the
    // required total size; without a separate channel-count check,
    // the next callback's input memcpy would walk past the vector
    // end at ch >= old chans.
    int hwBufSize = device->getCurrentBufferSizeSamples();
    int perChanNeeded = std::max(hwBufSize + mBufLen, 2 * mBufLen);
    int chans = std::max(1, std::max(activeIn, mNumInputChannels));
    if (perChanNeeded > mAccumPerChanCap)
        mAccumPerChanCap = perChanNeeded;
    size_t neededSize = static_cast<size_t>(mAccumPerChanCap) * chans;
    if (mInputAccum.size() < neededSize) {
        fprintf(stderr, "[juce-callback] resizing input accum: %zu -> %zu floats "
                "(perChanCap=%d chans=%d hwBuf=%d activeIn=%d mNumIn=%d)\n",
                mInputAccum.size(), neededSize, mAccumPerChanCap, chans,
                hwBufSize, activeIn, mNumInputChannels);
        fflush(stderr);
        mInputAccum.assign(neededSize, 0.0f);
    }

    // Log the full output channel layout so we can verify which physical
    // channels the aggregate is exposing and which are actually active.
    // Ordering matters: for an aggregate combining e.g. MBP Speakers +
    // MOTU, channels 0-1 might be MBP and 2-3 might be MOTU (or vice
    // versa, depending on sub-device list order). Scsynth writes to
    // whatever channels JUCE marks "active" — usually the first two.
    // Channel-layout dump on every device open — kept always-on because
    // it's the one-shot output we need to diagnose "my MOTU output 5 has
    // the wrong thing" / aggregate-ordering issues in user bug reports.
    // One per device start, so the volume is bounded.
    {
        auto outNames = device->getOutputChannelNames();
        auto activeOut = device->getActiveOutputChannels();
        fprintf(stderr, "[juce-callback] output channels (%d total):\n", outNames.size());
        for (int i = 0; i < outNames.size(); ++i) {
            fprintf(stderr, "[juce-callback]   [%d] %s%s\n", i,
                    outNames[i].toRawUTF8(), activeOut[i] ? " (active)" : "");
        }
        fflush(stderr);
    }
    fprintf(stderr, "[juce-callback] aboutToStart: device='%s' type='%s' sr=%d bs=%d activeOut=%d activeIn=%d outLat=%d inLat=%d\n",
            device->getName().toRawUTF8(),
            device->getTypeName().toRawUTF8(),
            mSampleRate,
            hwBufSize,
            activeOutCount, activeIn,
            device->getOutputLatencyInSamples(),
            device->getInputLatencyInSamples());
    fflush(stderr);

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
    fprintf(stderr, "[juce-callback] audioDeviceStopped (callbackCount=%u)\n", mCallbackCount);
    fflush(stderr);
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
    mInputAccumCount = 0;      // discard stale mic samples from before pause
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
    float* prefBase   = mPrefetchBuf.data();
    float* accumBase  = mInputAccum.data();
    const int accumPerChanCap = mAccumPerChanCap;

    // Accumulate available hardware input samples. Decouples HW buffer size
    // from scsynth's 128-sample block: we feed scsynth a full mBufLen only
    // when we have one, so no zero-padding inside a block.
    if (nIn > 0) {
        // Clamp incoming samples to our capacity. If the HW buffer is larger
        // than the accumulator (shouldn't happen — aboutToStart sized us to
        // fit), keep the newest samples only.
        int inSamples = std::min(numSamples, accumPerChanCap);
        int roomLeft  = accumPerChanCap - mInputAccumCount;
        if (inSamples > roomLeft) {
            // Drop oldest samples from accumulator to make room.
            int drop = inSamples - roomLeft;
            int keep = std::max(0, mInputAccumCount - drop);
            if (keep > 0) {
                for (int ch = 0; ch < nIn; ++ch)
                    std::memmove(accumBase + ch * accumPerChanCap,
                                 accumBase + ch * accumPerChanCap + drop,
                                 static_cast<size_t>(keep) * sizeof(float));
            }
            mInputAccumCount = keep;
        }
        // Copy new samples (or the tail if HW buffer > capacity)
        int srcOffset = numSamples - inSamples;
        for (int ch = 0; ch < nIn; ++ch) {
            if (inputChannelData[ch])
                std::memcpy(accumBase + ch * accumPerChanCap + mInputAccumCount,
                            inputChannelData[ch] + srcOffset,
                            static_cast<size_t>(inSamples) * sizeof(float));
            else
                std::memset(accumBase + ch * accumPerChanCap + mInputAccumCount,
                            0, static_cast<size_t>(inSamples) * sizeof(float));
        }
        mInputAccumCount += inSamples;
    }

    // ── 1. Drain leftover samples from the previous callback ─────────────────
    if (mPrefetchCount > 0) {
        int toDrain = std::min(mPrefetchCount, numSamples);
        for (int ch = 0; ch < nOut; ++ch)
            if (outputChannelData[ch])
                std::memcpy(outputChannelData[ch],
                            prefBase + ch * mBufLen,
                            static_cast<size_t>(toDrain) * sizeof(float));
        // Shift remaining prefetch samples down
        if (toDrain < mPrefetchCount) {
            int remaining = mPrefetchCount - toDrain;
            for (int ch = 0; ch < nOut; ++ch)
                std::memmove(prefBase + ch * mBufLen,
                             prefBase + ch * mBufLen + toDrain,
                             static_cast<size_t>(remaining) * sizeof(float));
        }
        mPrefetchCount -= toDrain;
        outputFilled    = toDrain;
    }

    // ── 2. Generate 128-sample scsynth blocks until the JUCE buffer is full ──
    // Derive NTP from sample position for jitter-free timing.  The sample
    // counter advances by exactly mBufLen per block, so NTP progresses
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
        // Feed scsynth one full mBufLen block of input from the accumulator.
        // If the accumulator doesn't have a full block yet (common at startup
        // when HW buffer < mBufLen), fall back to zero-padding the rest —
        // but this is now a rare edge case, not the common path.
        auto* inputBus = reinterpret_cast<float*>(get_audio_input_bus());
        if (inputBus && nIn > 0) {
            int usable = std::min(mInputAccumCount, mBufLen);
            for (int ch = 0; ch < nIn; ++ch) {
                if (usable > 0)
                    std::memcpy(inputBus + ch * mBufLen,
                                accumBase + ch * accumPerChanCap,
                                static_cast<size_t>(usable) * sizeof(float));
                if (usable < mBufLen)
                    std::memset(inputBus + ch * mBufLen + usable, 0,
                                static_cast<size_t>(mBufLen - usable) * sizeof(float));
            }
            // Shift accumulator down to discard the block we just consumed.
            if (usable > 0) {
                int remaining = mInputAccumCount - usable;
                if (remaining > 0) {
                    for (int ch = 0; ch < nIn; ++ch)
                        std::memmove(accumBase + ch * accumPerChanCap,
                                     accumBase + ch * accumPerChanCap + usable,
                                     static_cast<size_t>(remaining) * sizeof(float));
                }
                mInputAccumCount = remaining;
            }
        }

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
        wallNTP += static_cast<double>(mBufLen) / mSampleRate;
        mSamplePosition += mBufLen;

        auto* outputBus = reinterpret_cast<float*>(get_audio_output_bus());
        if (outputBus) {
            int needed  = numSamples - outputFilled;
            int toCopy  = std::min(needed, mBufLen);

            for (int ch = 0; ch < nOut; ++ch)
                if (outputChannelData[ch])
                    std::memcpy(outputChannelData[ch] + outputFilled,
                                outputBus + ch * mBufLen,
                                static_cast<size_t>(toCopy) * sizeof(float));
            outputFilled += toCopy;

            // Save any leftover samples for the next JUCE callback
            int leftover = mBufLen - toCopy;
            if (leftover > 0) {
                for (int ch = 0; ch < nOut; ++ch)
                    std::memcpy(prefBase + ch * mBufLen,
                                outputBus + ch * mBufLen + toCopy,
                                static_cast<size_t>(leftover) * sizeof(float));
                mPrefetchCount = leftover;
            }
        } else {
            // Engine not ready yet — init_memory hasn't run so the output
            // bus is null. Emit silence for the rest of this callback
            // rather than spinning in the while loop. This can happen
            // briefly at boot between aboutToStart and initialiseWorld().
            for (int ch = 0; ch < nOut; ++ch)
                if (outputChannelData[ch])
                    std::memset(outputChannelData[ch] + outputFilled, 0,
                                static_cast<size_t>(numSamples - outputFilled) * sizeof(float));
            outputFilled = numSamples;
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

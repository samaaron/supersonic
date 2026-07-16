/*
 * JuceAudioCallback.cpp
 */
#include "JuceAudioCallback.h"
#include "RealtimeThread.h"
#include "WallClock.h"
#include "SampleLoader.h"
#include "SuperClock.h"
#include "shared_memory.h"
#include "audio_config.h"
#include "supersonic_config.h"   // ss_log
#include "lanes/lanes.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <algorithm>
#include <cstring>

// The engine's segment-resident metrics block (defined in audio_processor.cpp,
// C linkage). Used to mirror Link clock + stream-health into the dashboard.
extern "C" PerformanceMetrics* metrics;
extern "C" void World_PublishAudioLoad(uint32_t cpuAvgCenti, uint32_t cpuPeakCenti,
                                       uint32_t callbackOverruns);

// Shared per-scsynth-block render (see JuceAudioCallback.h). One copy of the
// drain → process_audio → publish sequence for both HeadlessDriver and the
// engine's manual pump.
void renderAudioBlock(SuperClock& clock,
                      uint32_t blockSize,
                      uint32_t numOutputChannels,
                      uint32_t numInputChannels,
                      uint32_t sampleRate,
                      double   ntp,
                      uint64_t hostMicros) {
    // Drain Link Audio inputs into the private bus pool before process_audio so
    // In.ar reads see them this block (no-op without an active subscription).
    if (auto* busPool = reinterpret_cast<float*>(get_audio_bus_pool())) {
        clock.drainLinkAudioInputsToBuses(
            busPool, blockSize,
            static_cast<uint32_t>(get_audio_bus_count()), sampleRate, hostMicros);
    }

    ss_tick(ntp, numOutputChannels, numInputChannels);

    // Publish the main sink (stereo when nOut >= 2, mono fallback for 1) plus any
    // user aux sinks. No-op when Link Audio is off / no subscriber.
    if (const float* outputBus = ss_audio_out()) {
        if (numOutputChannels >= 2) {
            clock.publishAudioBlock(outputBus, outputBus + blockSize,
                                    static_cast<size_t>(blockSize), sampleRate, hostMicros);
        } else if (numOutputChannels == 1) {
            clock.publishAudioBlock(outputBus, nullptr,
                                    static_cast<size_t>(blockSize), sampleRate, hostMicros);
        }
    }
    if (auto* busPool = reinterpret_cast<float*>(get_audio_bus_pool())) {
        clock.publishAuxSinks(
            busPool, blockSize,
            static_cast<uint32_t>(get_audio_bus_count()), sampleRate, hostMicros);
    }
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
                                         int sharedMemoryID,
                                         int bufLen)
{
    mRingBufferStorage = ringBufferStorage;
    mSampleRate        = sampleRate;
    mNumOutputChannels = numOutputChannels;
    mNumInputChannels  = numInputChannels;
    mWorldInputBusChannels = numInputChannels;  // immutable World input-bus width

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

    // Configure + boot the World through the lanes ABI's typed init. ss_init
    // writes the positional options block (at WORLD_OPTIONS_START in the arena —
    // ringBufferStorage here, or the external segment) and brings the engine up,
    // so this site sets named fields rather than magic indices. NRT/self-driven
    // invariants (real-time off, memory-locking off, mode 0) are implied by ss_init.
    SsWorldOptions opts = {};
    opts.num_buffers              = static_cast<uint32_t>(numBuffers);
    opts.max_nodes                = static_cast<uint32_t>(maxNodes);
    opts.max_graph_defs           = static_cast<uint32_t>(maxGraphDefs);
    opts.max_wire_bufs            = static_cast<uint32_t>(maxWireBufs);
    opts.num_audio_bus_channels   = static_cast<uint32_t>(numAudioBusChannels);
    opts.num_input_bus_channels   = static_cast<uint32_t>(numInputChannels);
    opts.num_output_bus_channels  = static_cast<uint32_t>(numOutputChannels);
    opts.num_control_bus_channels = static_cast<uint32_t>(numControlBusChannels);
    opts.buf_length               = static_cast<uint32_t>(mBufLen);
    opts.real_time_memory_size    = static_cast<uint32_t>(realTimeMemorySize);
    opts.num_rgens                = static_cast<uint32_t>(numRGens);
    opts.load_graph_defs          = 1;  // native loads synthdefs from the host filesystem
    opts.verbosity                = 0;
    opts.shared_memory_id         = static_cast<uint32_t>(sharedMemoryID);
    ss_init(&opts, static_cast<double>(sampleRate));
}

void JuceAudioCallback::audioDeviceAboutToStart(juce::AudioIODevice* device) {
    mSampleRate     = static_cast<int>(device->getCurrentSampleRate());
    mSamplePosition = 0.0;
    mPrefetchCount  = 0;
    mInputAccumCount = 0;
    mSuperClock->resetAudioThreadTime(mSamplePosition, mSampleRate);
    std::fill(mPrefetchBuf.begin(), mPrefetchBuf.end(), 0.0f);

    // A device (re)start can hand the callback to a brand-new audio thread, so
    // re-arm realtime promotion; the first IO callback below performs it on the
    // audio thread itself (this function runs on the control thread).
    mRealtimeElevated.store(false, std::memory_order_relaxed);

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
        fprintf(stderr, "[juce] resizing input accum: %zu -> %zu floats "
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
        fprintf(stderr, "[juce] output channels (%d total):\n", outNames.size());
        for (int i = 0; i < outNames.size(); ++i) {
            fprintf(stderr, "[juce]   [%d] %s%s\n", i,
                    outNames[i].toRawUTF8(), activeOut[i] ? " (active)" : "");
        }
        fflush(stderr);
    }
    fprintf(stderr, "[juce] aboutToStart: device='%s' type='%s' sr=%d bs=%d activeOut=%d activeIn=%d outLat=%d inLat=%d\n",
            device->getName().toRawUTF8(),
            device->getTypeName().toRawUTF8(),
            mSampleRate,
            hwBufSize,
            activeOutCount, activeIn,
            device->getOutputLatencyInSamples(),
            device->getInputLatencyInSamples());
    fflush(stderr);

    mOutputLatencySamples = device->getOutputLatencyInSamples();

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
    fprintf(stderr, "[juce] audioDeviceStopped (callbackCount=%u)\n", mCallbackCount);
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
    mSuperClock->resetAudioThreadTime(mSamplePosition, mSampleRate);
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
    const juce::AudioIODeviceCallbackContext& context)
{
    // Promote the audio thread to realtime once per device start. Done here
    // rather than in audioDeviceAboutToStart (control thread) because this
    // callback runs on the audio thread. A denied request (no rtprio
    // permission) leaves the thread unchanged; see RealtimeThread.h.
    if (!mRealtimeElevated.exchange(true, std::memory_order_relaxed)) {
        const auto rt = supersonic::elevateCurrentThreadToRealtime();
        ss_log("[juce] audio thread realtime: status=%d policy=%d prio=%d err=%d",
               static_cast<int>(rt.status), rt.policy, rt.priority, rt.error);
    }

    // Denormal flush-to-zero is a per-thread flag, and scsynth only arms it on
    // the boot thread (sc_SetDenormalFlags in World_New). Arm it on this audio
    // thread too, else denormal reverb tails take the slow path and spike CPU.
    const juce::ScopedNoDenormals scopedNoDenormals;

    // ── If paused, output silence and touch nothing else ──────────────────────
    // Placed before the warmup counter, mSamplePosition and mLastCbTime reads
    // below: during a device swap/recovery the control thread runs resume(),
    // which resets exactly those fields, and a freshly-opened device can already
    // be delivering callbacks on its own IO thread before resume() clears
    // mPaused. Gating here keeps that callback off every resume-mutated field
    // (the acquire pairs with resume()'s release, publishing the reset), so
    // there's no data race — and no need to make those hot-path fields atomic.
    if (mPaused.load(std::memory_order_acquire)) {
        for (int ch = 0; ch < numOutputChannels; ++ch)
            if (outputChannelData[ch])
                std::memset(outputChannelData[ch], 0,
                            static_cast<size_t>(numSamples) * sizeof(float));
        processCount.fetch_add(1, std::memory_order_release);
        processCount.notify_all();
        return;
    }

    int nIn  = juce::jmin(numInputChannels,  mNumInputChannels);
    int nOut = juce::jmin(numOutputChannels, mNumOutputChannels);

    // hostTimeNs is the framework's "this buffer plays at T" timestamp
    // (CoreAudio supplies it on macOS). When absent — JUCE's Windows backends,
    // the headless driver, some ALSA configs — fall back to SuperClock's
    // jitter-free host time rather than reading the jittery link.clock() here.
    uint64_t linkAudioBlockHostMicros =
        context.hostTimeNs != nullptr
            ? (*context.hostTimeNs) / 1000ULL
            : static_cast<uint64_t>(
                  mSuperClock->linkAudioHostMicros(mSamplePosition, mSampleRate));
    const uint64_t scsynthBlockMicros = mSampleRate > 0
        ? static_cast<uint64_t>((static_cast<double>(mBufLen) * 1e6) / mSampleRate)
        : 0ULL;

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
            mSuperClock->resetAudioThreadTime(mSamplePosition, mSampleRate);
            if (onWake) onWake();
        } else if (gapUs > 150'000.0) {
            // Sub-wake stall: long enough to snap the timeline and push
            // scheduled client threads past their sched-ahead window (Spider
            // raises TimingError on the affected live loops), too short for
            // the wake path above. Log its size — without this, such a stall
            // surfaces only as unexplained LATEs downstream.
            fprintf(stderr, "  [gap] audio callback stalled %.0fms\n", gapUs / 1000.0);
            fflush(stderr);
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
    double wallNTP = mSuperClock->updateAudioThreadNTP(mSamplePosition, mSampleRate);

    // Sample clock: one anchor per hardware callback (the line is linear
    // across its sub-blocks); per-block cursor advances happen in the loop.
    // Negative latency reports from flaky drivers clamp to 0 — a raw cast
    // would push the anchor ~a day ahead.
    mSuperClock->publishSampleClock(
        mSamplePosition, static_cast<double>(mSampleRate), wallNTP,
        static_cast<uint32_t>(std::max(0, mOutputLatencySamples)));

    // Mirror Link clock + stream-health into the dashboard metrics from one
    // lock-free session capture. `metrics` is the engine's segment-resident
    // metrics block.
    mSuperClock->publishLinkMetrics(metrics, 4.0);

    // Mirror the cross-platform SuperClock readout (tempo/beat/phase/playing,
    // slots 65-68) from the same Link-driven clock instance. Reads the SAB
    // mirror — RT-safe and live even on no-Link builds.
    mSuperClock->publishClockMetrics(metrics, wallNTP, 4.0);


    while (outputFilled < numSamples) {
        // Feed scsynth one full mBufLen block of input from the accumulator.
        // If the accumulator doesn't have a full block yet (common at startup
        // when HW buffer < mBufLen), fall back to zero-padding the rest —
        // but this is now a rare edge case, not the common path.
        float* inputBus = ss_audio_in();
        if (inputBus && nIn > 0) {
            int usable = std::min(mInputAccumCount, mBufLen);
            // Clamp to the World's input bus width: mNumInputChannels tracks the
            // live device and can exceed it after a device swap, which would copy
            // past the input region into the private bus pool. Mirrors process_audio().
            int inCh = std::min(nIn, mWorldInputBusChannels);
            for (int ch = 0; ch < inCh; ++ch) {
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
            preTick(mSamplePosition, wallNTP * 1000.0 - supersonic::kNtpEpochOffset * 1000.0);

        // Drain pending Link Audio input into the listen bus before
        // scsynth's In.ar reads it. No-op without an active subscription.
        if (auto* busPool = reinterpret_cast<float*>(get_audio_bus_pool())) {
            mSuperClock->drainLinkAudioInputsToBuses(
                busPool,
                static_cast<uint32_t>(mBufLen),
                static_cast<uint32_t>(get_audio_bus_count()),
                static_cast<uint32_t>(mSampleRate),
                linkAudioBlockHostMicros);
        }

        // Native timing: pass wall-clock NTP directly; the tick uses it as-is
        // (only the WASM build converts its argument, from AudioContext time).
        // Advance NTP by one block duration for each sub-block.
        ss_tick(wallNTP,
                static_cast<uint32_t>(mNumOutputChannels),
                static_cast<uint32_t>(mNumInputChannels));
        wallNTP += static_cast<double>(mBufLen) / mSampleRate;
        mSamplePosition += mBufLen;
        // Keep scope-stream writes anchored to the block being rendered.
        mSuperClock->advanceEngineFrames(mSamplePosition);

        const float* outputBus = ss_audio_out();
        if (outputBus) {
            // Publish this scsynth block to Link Audio. Main sink:
            // stereo when nOut >= 2, mono fallback for nOut == 1.
            // hostMicros is the audio-framework's playback timestamp
            // for THIS sub-block; advanced after each publish so
            // consecutive sub-blocks within one JUCE callback get
            // correctly-spaced timestamps. No-op when LinkAudio off /
            // no subscriber.
            if (nOut >= 2) {
                mSuperClock->publishAudioBlock(
                    outputBus, outputBus + mBufLen,
                    static_cast<size_t>(mBufLen),
                    static_cast<uint32_t>(mSampleRate),
                    linkAudioBlockHostMicros);
            } else if (nOut == 1) {
                mSuperClock->publishAudioBlock(
                    outputBus, nullptr,
                    static_cast<size_t>(mBufLen),
                    static_cast<uint32_t>(mSampleRate),
                    linkAudioBlockHostMicros);
            }
            // Any user-added aux sinks tapping arbitrary bus ranges.
            // No-op when none registered (lock-free fast path).
            if (auto* busPool = reinterpret_cast<float*>(get_audio_bus_pool())) {
                mSuperClock->publishAuxSinks(
                    busPool,
                    static_cast<uint32_t>(mBufLen),
                    static_cast<uint32_t>(get_audio_bus_count()),
                    static_cast<uint32_t>(mSampleRate),
                    linkAudioBlockHostMicros);
            }
            linkAudioBlockHostMicros += scsynthBlockMicros;

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
    if (cbUs > budgetUs) {
        mOverrunCount++;
        // An overrun (render missed its budget) is audible as a stutter but
        // produces no callback gap and no clock drift, so it needs its own
        // log line to be diagnosable. Rate-limited to one line per second.
        const double nowSec =
            std::chrono::duration<double>(cbEnd.time_since_epoch()).count();
        if (nowSec - mLastOverrunLogSec >= 1.0) {
            mLastOverrunLogSec = nowSec;
            ss_log("[overrun] render took %.1fms of %.1fms budget "
                   "(total overruns: %u)",
                   cbUs / 1000.0, budgetUs / 1000.0, mOverrunCount);
        }
    }

    // DSP load = how much of the callback's time budget the render consumed.
    // Smooth the average (EMA) and let the peak decay, so both track recent
    // behaviour rather than pinning to a one-off lifetime spike. Published to
    // native-stats as percent * 100 for the GUI dashboard.
    if (budgetUs > 0.0) {
        const double instPct = (cbUs / budgetUs) * 100.0;
        mLoadAvgPct  += (instPct - mLoadAvgPct) * 0.1;            // ~10-callback EMA
        mLoadPeakPct = std::max(instPct, mLoadPeakPct * 0.95);   // decaying peak
        World_PublishAudioLoad(static_cast<uint32_t>(mLoadAvgPct * 100.0 + 0.5),
                               static_cast<uint32_t>(mLoadPeakPct * 100.0 + 0.5),
                               mOverrunCount);
    }

    // ── 5. Notify worker threads (one tick per JUCE callback) ─────────────────
    processCount.fetch_add(1, std::memory_order_release);
    processCount.notify_all();
}

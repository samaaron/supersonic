/*
 * SPLimiterCore.h: look-ahead brickwall limiter — the C++ face of it.
 *
 * The DSP is Rust (rust/supersonic-limiter/src/lib.rs), where the
 * design, the ceiling proof and the reasoning about each stage now
 * live. This header is the shape the ugens and the tests already
 * expect, and does nothing but forward.
 *
 * Keeping the interface identical is the point: test_splimiter.cpp is
 * the specification of what this limiter guarantees — that the ceiling
 * holds on adversarial input, that latency is exactly the look-ahead,
 * that the two channels are scaled identically, that the output does
 * not depend on block size — and every one of those cases now exercises
 * the Rust implementation without being rewritten to do so.
 *
 * The core is placed inside the caller's allocation rather than owning
 * its own. The ugen constructs on the audio thread and takes its
 * scratch from the server's real-time heap, so nothing here may reach
 * for a general allocator.
 */
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>

extern "C" {
int    sp_limiter_min_lookahead(void);
int    sp_limiter_max_lookahead(void);
int    sp_limiter_max_channels(void);
int    sp_limiter_clamp_lookahead(int samples);
int    sp_limiter_clamp_channels(int channels);
int    sp_limiter_lookahead_samples(float seconds, double sampleRate);
int    sp_limiter_box_length(int d);
int    sp_limiter_kernel_length(int d);
int    sp_limiter_deque_capacity(int d);
size_t sp_limiter_memory_bytes(int lookahead, int channels);
void*  sp_limiter_configure(void* mem, int lookahead, int channels, double sampleRate);
void   sp_limiter_reset(void* handle);
int    sp_limiter_latency(const void* handle);
int    sp_limiter_channels(const void* handle);
float  sp_limiter_current_gain(const void* handle);
void   sp_limiter_process_mono(void* handle, const float* in, float* out, int numSamples,
                               float level, float release, float* gainOut);
void   sp_limiter_process_stereo(void* handle, const float* inL, const float* inR, float* outL,
                                 float* outR, int numSamples, float level, float release,
                                 float* gainOut);
}

namespace sonicpi::dsp {

class SPLimiterCore {
public:
    // Smallest and largest look-ahead the core will run at, in samples.
    static constexpr int kMinLookahead = 4;
    static constexpr int kMaxLookahead = 1 << 16;
    static constexpr int kMaxChannels = 2;

    static int clampLookahead(int samples) { return sp_limiter_clamp_lookahead(samples); }
    static int clampChannels(int channels) { return sp_limiter_clamp_channels(channels); }

    // Look-ahead in samples for a duration in seconds, clamped.
    static int lookaheadSamples(float seconds, double sampleRate) {
        return sp_limiter_lookahead_samples(seconds, sampleRate);
    }

    static int boxLength(int d) { return sp_limiter_box_length(d); }

    // Kernel length actually in use. Exposed so tests can assert the
    // K <= D+1 invariant the ceiling guarantee rests on.
    static int kernelLength(int d) { return sp_limiter_kernel_length(d); }

    static int dequeCapacity(int d) { return sp_limiter_deque_capacity(d); }

    // Total scratch the core needs, in bytes — the core's own state
    // included, since it is placed in the same block. Callers allocate
    // this once and hand it to configure().
    static std::size_t memoryBytes(int lookaheadSamples, int channels = 1) {
        return sp_limiter_memory_bytes(lookaheadSamples, channels);
    }

    // Point the core at its scratch memory and set look-ahead and
    // channel count. `mem` must be at least memoryBytes(lookahead,
    // channels) and suitably aligned (any malloc or RTAlloc result is).
    // Clears all state, so this is also the reset path.
    void configure(int lookaheadSamples, int channels, double sampleRate, void* mem) {
        mHandle = sp_limiter_configure(mem, lookaheadSamples, channels, sampleRate);
    }

    // Zero the delay lines and park the gain at unity.
    void reset() { sp_limiter_reset(mHandle); }

    // Latency in samples: the delay from an input sample to the output
    // sample carrying it.
    int latencySamples() const { return sp_limiter_latency(mHandle); }
    int channels() const { return sp_limiter_channels(mHandle); }

    // Current smoothed gain, for metering.
    float currentGain() const { return sp_limiter_current_gain(mHandle); }

    // Mono. `in` and `out` may alias. `gainOut` is optional (pass
    // nullptr) and receives the applied gain per sample, so a meter can
    // read measured gain reduction rather than inferring it.
    void process(const float* in, float* out, int numSamples, float level, float release,
                 float* gainOut = nullptr) {
        sp_limiter_process_mono(mHandle, in, out, numSamples, level, release, gainOut);
    }

    // Stereo with a linked detector: one gain, derived from whichever
    // channel needs it most, applied to both.
    void process(const float* inL, const float* inR, float* outL, float* outR, int numSamples,
                 float level, float release, float* gainOut = nullptr) {
        sp_limiter_process_stereo(mHandle, inL, inR, outL, outR, numSamples, level, release,
                                  gainOut);
    }

private:
    void* mHandle = nullptr;
};

}  // namespace sonicpi::dsp

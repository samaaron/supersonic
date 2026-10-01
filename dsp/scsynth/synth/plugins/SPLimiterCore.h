/*
 * SPLimiterCore.h: Look-ahead brickwall limiter DSP
 *
 * The processing core behind the SPLimiter / SPLimiter2 UGens, extracted
 * as a plain struct with no SuperCollider dependencies so the guarantees
 * below can be unit-tested directly (see test/native/test_splimiter.cpp).
 *
 * The design target is minimum latency for a given transparency, because
 * this sits on a master bus where the delay is added to live input
 * monitoring. Latency is exactly `lookahead` samples, not twice it.
 *
 *
 * ── How it works ────────────────────────────────────────────────────
 *
 * Writing D for the look-ahead in samples, x for the input and L for the
 * ceiling, the output is
 *
 *     y[n] = g[n] * x[n-D]
 *
 * so the gain g[n] is chosen with D samples of the signal's future in
 * hand. The required instantaneous gain is
 *
 *     r[n] = min(1, L/|x[n]|)
 *
 * which is spiky and discontinuous; multiplying by it directly *is*
 * nonlinear distortion. So g is built from r in three stages:
 *
 *   1. m[n] = min over k in [n-D, n] of r[k]      (sliding minimum)
 *   2. h[n] = m[n] with the rising limb slowed    (program-dependent release)
 *   3. g[n] = h smoothed by a length-K kernel     (cascaded box filters)
 *
 * This is the same structure as Hämäläinen's peak limiter (DAFx-02),
 * which feeds an order-statistics filter into the gain smoother so the
 * smoothed gain provably cannot exceed the required one; here the
 * statistic is a minimum in the gain domain rather than a maximum in the
 * level domain.
 *
 *
 * ── Why the ceiling is guaranteed ───────────────────────────────────
 *
 * Stage 3 is a normalised non-negative kernel, so
 *
 *     g[n] = sum over j in [0, K-1] of w[j] * h[n-j],   sum of w[j] = 1
 *
 * Stage 2 only ever holds the envelope *below* its input, so h <= m
 * pointwise. And m[n-j] is a minimum over [n-j-D, n-j], a window which
 * contains index n-D for every j in [0, D]. So as long as K <= D+1,
 * every term of the sum is bounded by r[n-D], and therefore
 *
 *     g[n] <= r[n-D]   =>   |y[n]| = g[n]*|x[n-D]| <= L
 *
 * with no overshoot and no clipping, for any input whatsoever. The
 * K <= D+1 constraint is what boxLength() enforces when it sizes the
 * box filters. The bound holds for a *constant* L; lowering the
 * ceiling takes D samples to apply to audio already in the delay line.
 *
 * In stereo the detector runs on max(|l|, |r|), so the single gain is
 * bounded by the required gain of whichever channel needs it most, and
 * the guarantee holds for both. That also means L and R are always
 * scaled identically, which is what keeps the stereo image still when
 * the limiter works, an unlinked pair moves the image on any peak that
 * is not perfectly symmetric.
 *
 *
 * ── Why the pieces are the shape they are ───────────────────────────
 *
 * The sliding minimum is a monotonic deque (Lemire's streaming min-max
 * filter), so it costs O(1) amortised per sample rather than O(D). It
 * also removes only the gain that is genuinely required: a per-block
 * peak-and-ramp scheme instead drags the whole mix down for a full
 * block because one sample in it was loud.
 *
 * The smoothing kernel is three cascaded box filters, which is a
 * quadratic B-spline, continuous in its first derivative, unlike a
 * linear ramp, whose slope discontinuities at both ends are themselves
 * a distortion source. Cascaded boxes give that shape for O(1) per
 * sample via running sums, where a direct FIR would be O(K). Cascaded
 * smoothers for this purpose are also what Sanfilippo (IFC-22) arrives
 * at, using exponentials rather than boxes; boxes are used here because
 * their finite support is what closes the K <= D+1 proof above.
 *
 * The release is program-dependent, keyed on how long reduction has
 * PERSISTED rather than on how deep it currently is. An isolated
 * transient recovers fast, which is inaudible because the transient
 * that caused it masks the recovery. Sustained reduction recovers
 * slowly, because a gain that keeps snapping back between hits
 * modulates the low end audibly, measurable as intermodulation. Keying
 * this on depth instead gets it backwards: heavy programme material is
 * both deep and sustained, and wants the slow limb.
 *
 * Attack needs no such treatment. It is fixed at the look-ahead length,
 * and a gain dip in the couple of milliseconds before a transient is
 * masked by the transient itself.
 */
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace sonicpi::dsp {

class SPLimiterCore {
public:
    // Smallest and largest look-ahead the core will run at, in samples.
    // The floor keeps the box filters non-degenerate; the ceiling bounds
    // what a synthdef can ask the server to allocate.
    static constexpr int kMinLookahead = 4;
    static constexpr int kMaxLookahead = 1 << 16;

    static constexpr int kMaxChannels = 2;

    // Ratio of the slow (sustained reduction) release time to the fast
    // (isolated transient) one.
    static constexpr float kSlowReleaseRatio = 8.0f;

    // Time constant, in seconds, over which reduction is judged to have
    // persisted. Long enough not to be swayed by a single transient,
    // short enough to reach the slow limb within a bar or two.
    static constexpr float kSustainTimeConstant = 0.15f;

    // Average gain drop below unity at which the release is fully slow.
    // 0.25 is a little over 2.5 dB of *sustained* reduction.
    static constexpr float kFullSustainDepth = 0.25f;

    // Clamp a requested look-ahead to the supported range.
    static int clampLookahead(int samples) {
        if (samples < kMinLookahead) return kMinLookahead;
        if (samples > kMaxLookahead) return kMaxLookahead;
        return samples;
    }

    // Look-ahead in samples for a duration in seconds, clamped. Rounds
    // to nearest rather than up: a float seconds value is not exact, so
    // rounding up turns a requested 1.5 ms at 48 kHz into 73 samples
    // rather than 72 purely on the representation error.
    static int lookaheadSamples(float seconds, double sampleRate) {
        if (!(seconds > 0.0f) || !(sampleRate > 0.0)) return kMinLookahead;
        const double n = std::floor(static_cast<double>(seconds) * sampleRate + 0.5);
        if (n > static_cast<double>(kMaxLookahead)) return kMaxLookahead;
        return clampLookahead(static_cast<int>(n));
    }

    // Box-filter length for a look-ahead of d samples. Three cascaded
    // boxes of length B give a kernel of length 3B-2, and the ceiling
    // proof needs that to be <= d+1, which this satisfies for every
    // d >= 0, since 3*floor((d+3)/3) <= d+3.
    static int boxLength(int d) {
        const int b = (d + 3) / 3;
        return b < 1 ? 1 : b;
    }

    // Kernel length actually in use. Exposed so tests can assert the
    // K <= D+1 invariant the ceiling guarantee rests on.
    static int kernelLength(int d) { return boxLength(d) * 3 - 2; }

    // Deque capacity for a look-ahead of d samples. The sliding minimum
    // spans d+1 indices, and a push happens before the now-stale head is
    // evicted, so the deque holds up to d+2 entries, plus one slot to
    // keep "full" distinguishable from "empty".
    static int dequeCapacity(int d) { return d + 3; }

    static int clampChannels(int channels) {
        if (channels < 1) return 1;
        if (channels > kMaxChannels) return kMaxChannels;
        return channels;
    }

    // Total scratch the core needs, in bytes. Callers allocate this once
    // and hand it to configure().
    static std::size_t memoryBytes(int lookaheadSamples, int channels = 1) {
        const int d = clampLookahead(lookaheadSamples);
        const int cap = dequeCapacity(d);
        const std::size_t floats =
            static_cast<std::size_t>(d) * static_cast<std::size_t>(clampChannels(channels)) +
            static_cast<std::size_t>(cap) +
            static_cast<std::size_t>(boxLength(d)) * 3;
        const std::size_t ints = static_cast<std::size_t>(cap);
        return floats * sizeof(float) + ints * sizeof(std::uint32_t);
    }

    // Point the core at its scratch memory and set look-ahead and
    // channel count. `mem` must be at least memoryBytes(lookahead,
    // channels) and suitably aligned for float/uint32 (any malloc or
    // RTAlloc result is). Clears all state, so this is also the reset
    // path.
    void configure(int lookaheadSamples, int channels, double sampleRate, void* mem) {
        mLookahead = clampLookahead(lookaheadSamples);
        mChannels = clampChannels(channels);
        mDequeCap = dequeCapacity(mLookahead);
        mBoxLen = boxLength(mLookahead);
        mSampleRate = sampleRate > 0.0 ? sampleRate : 44100.0;

        float* f = static_cast<float*>(mem);
        for (int c = 0; c < mChannels; ++c) {
            mDelay[c] = f;
            f += mLookahead;
        }
        mDequeVals = f;  f += mDequeCap;
        mBox[0] = f;     f += mBoxLen;
        mBox[1] = f;     f += mBoxLen;
        mBox[2] = f;     f += mBoxLen;
        mDequeIdx = reinterpret_cast<std::uint32_t*>(f);

        reset();
    }

    // Zero the delay lines and park the gain at unity.
    void reset() {
        for (int c = 0; c < mChannels; ++c) {
            for (int i = 0; i < mLookahead; ++i) mDelay[c][i] = 0.0f;
        }
        for (int i = 0; i < mDequeCap; ++i) {
            mDequeVals[i] = 0.0f;
            mDequeIdx[i] = 0;
        }
        for (int b = 0; b < 3; ++b) {
            for (int i = 0; i < mBoxLen; ++i) mBox[b][i] = 1.0f;
            mBoxSum[b] = static_cast<double>(mBoxLen);
            mBoxPos[b] = 0;
        }
        mDelayPos = 0;
        mHead = 0;
        mTail = 0;
        mCount = 0;
        mSampleIndex = 0;
        mEnv = 1.0f;
        mGain = 1.0f;
        mSustain = 0.0f;
    }

    // Latency in samples: the delay from an input sample to the output
    // sample carrying it.
    int latencySamples() const { return mLookahead; }
    int channels() const { return mChannels; }

    // Current smoothed gain, for metering.
    float currentGain() const { return mGain; }

    // Mono. `in` and `out` may alias. `gainOut` is optional (pass
    // nullptr) and receives the applied gain per sample, so a meter can
    // read measured gain reduction rather than inferring it.
    void process(const float* in, float* out, int numSamples, float level, float release,
                 float* gainOut = nullptr) {
        Coefs k = coefs(level, release);
        for (int i = 0; i < numSamples; ++i) {
            const float x = in[i];
            const float g = nextGain(std::fabs(x), k);
            const float delayed = mDelay[0][mDelayPos];
            mDelay[0][mDelayPos] = x;
            advance();
            out[i] = g * delayed;
            if (gainOut) gainOut[i] = g;
        }
    }

    // Stereo with a linked detector: one gain, derived from whichever
    // channel needs it most, applied to both.
    void process(const float* inL, const float* inR, float* outL, float* outR, int numSamples,
                 float level, float release, float* gainOut = nullptr) {
        Coefs k = coefs(level, release);
        for (int i = 0; i < numSamples; ++i) {
            const float l = inL[i];
            const float r = inR[i];
            const float al = std::fabs(l);
            const float ar = std::fabs(r);
            const float g = nextGain(al > ar ? al : ar, k);
            const float dl = mDelay[0][mDelayPos];
            const float dr = mDelay[1][mDelayPos];
            mDelay[0][mDelayPos] = l;
            mDelay[1][mDelayPos] = r;
            advance();
            outL[i] = g * dl;
            outR[i] = g * dr;
            if (gainOut) gainOut[i] = g;
        }
    }

private:
    struct Coefs {
        float level;
        float fast;
        float slow;
        float sustainAlpha;
    };

    Coefs coefs(float level, float release) const {
        if (!(level > 0.0f)) level = 1e-6f;
        if (!(release > 0.0f)) release = 1e-4f;
        Coefs k;
        k.level = level;
        k.fast = onePoleCoef(release);
        k.slow = onePoleCoef(release * kSlowReleaseRatio);
        // How fast the sustain estimate itself moves. Complement of a
        // one-pole coefficient, so it is a per-sample blend weight.
        k.sustainAlpha = 1.0f - onePoleCoef(kSustainTimeConstant);
        return k;
    }

    // One sample of the gain path: sliding minimum, program-dependent
    // release, then the smoothing cascade.
    float nextGain(float mag, const Coefs& k) {
        const float required = mag > k.level ? k.level / mag : 1.0f;

        // Sliding minimum over the look-ahead window, via a deque held
        // monotonically non-decreasing from head to tail.
        const std::uint32_t window = static_cast<std::uint32_t>(mLookahead) + 1u;
        while (mCount > 0 && mDequeVals[prev(mTail)] >= required) {
            mTail = prev(mTail);
            --mCount;
        }
        mDequeVals[mTail] = required;
        mDequeIdx[mTail] = mSampleIndex;
        mTail = next(mTail);
        ++mCount;
        // Unsigned subtraction, so this stays correct across the wrap of
        // mSampleIndex on a long-running session.
        while (mCount > 0 && (mSampleIndex - mDequeIdx[mHead]) >= window) {
            mHead = next(mHead);
            --mCount;
        }
        const float minGain = mDequeVals[mHead];

        // How much reduction is being asked for, averaged over
        // kSustainTimeConstant. This is what decides the release rate:
        // brief reduction leaves it near zero, sustained reduction
        // drives it up.
        mSustain += (1.0f - minGain - mSustain) * k.sustainAlpha;

        // Program-dependent release. Falls instantly, rises at a rate
        // that slows the longer reduction has persisted. Whichever limb
        // is taken, mEnv stays <= minGain, which is what the ceiling
        // proof requires.
        if (minGain <= mEnv) {
            mEnv = minGain;
        } else {
            float sustained = mSustain * (1.0f / kFullSustainDepth);
            if (sustained > 1.0f) sustained = 1.0f;
            if (sustained < 0.0f) sustained = 0.0f;
            const float coef = k.fast + (k.slow - k.fast) * sustained;
            mEnv = minGain + (mEnv - minGain) * coef;
        }

        // Three cascaded box filters: a quadratic B-spline kernel of
        // length 3*mBoxLen-2, computed in O(1) from running sums. The
        // sums are double so that repeated add/subtract over a long
        // session cannot drift the gain above its input.
        float v = mEnv;
        for (int b = 0; b < 3; ++b) {
            mBoxSum[b] += static_cast<double>(v) - static_cast<double>(mBox[b][mBoxPos[b]]);
            mBox[b][mBoxPos[b]] = v;
            if (++mBoxPos[b] >= mBoxLen) mBoxPos[b] = 0;
            v = static_cast<float>(mBoxSum[b] / mBoxLen);
        }
        mGain = v;
        ++mSampleIndex;
        return mGain;
    }

    void advance() {
        if (++mDelayPos >= mLookahead) mDelayPos = 0;
    }

    // One-pole coefficient for a time constant in seconds. Larger means
    // slower.
    float onePoleCoef(float seconds) const {
        const double n = static_cast<double>(seconds) * mSampleRate;
        if (n < 1.0) return 0.0f;
        return static_cast<float>(std::exp(-1.0 / n));
    }

    int next(int i) const { return i + 1 >= mDequeCap ? 0 : i + 1; }
    int prev(int i) const { return i == 0 ? mDequeCap - 1 : i - 1; }

    float* mDelay[kMaxChannels] = {nullptr, nullptr};
    float* mDequeVals = nullptr;
    std::uint32_t* mDequeIdx = nullptr;
    float* mBox[3] = {nullptr, nullptr, nullptr};

    double mBoxSum[3] = {0.0, 0.0, 0.0};
    int mBoxPos[3] = {0, 0, 0};
    int mBoxLen = 1;

    double mSampleRate = 44100.0;
    int mLookahead = 1;
    int mChannels = 1;
    int mDequeCap = 4;
    int mDelayPos = 0;

    int mHead = 0;
    int mTail = 0;
    int mCount = 0;
    std::uint32_t mSampleIndex = 0;

    float mEnv = 1.0f;
    float mGain = 1.0f;
    float mSustain = 0.0f;
};

}  // namespace sonicpi::dsp

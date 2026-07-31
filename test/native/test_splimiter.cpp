/*
 * test_splimiter.cpp: Guarantees of the look-ahead limiter core
 *
 * SPLimiterCore claims three things that the master bus depends on:
 *
 *   1. The output never exceeds the ceiling, for any input at all.
 *      Not "rarely", not "within a dB". That claim is a proof (see the
 *      header), so the tests here are adversarial: the nastiest signals
 *      that could break it, at every block size.
 *   2. Latency is exactly the look-ahead, in samples.
 *   3. A signal already under the ceiling passes through untouched.
 *
 * If any of these regress, the mixer's `clip2` safety net starts doing
 * audible work and the whole point of the design is lost.
 */
#include <catch2/catch_test_macros.hpp>

#include "SPLimiterCore.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

using sonicpi::dsp::SPLimiterCore;

namespace {

constexpr double kSR = 48000.0;

// MSVC only defines kPi behind _USE_MATH_DEFINES.
constexpr double kPi = 3.14159265358979323846;

// Tolerated overshoot. The proof is exact in real arithmetic; float
// rounding in the gain path costs a handful of ULP, which at 1e-5
// relative is 100 dB below the ceiling.
constexpr float kTol = 1e-5f;

// A configured core plus the scratch it owns. Guard words either side of
// the scratch catch a core that writes outside what memoryBytes() said
// it needed.
class Fixture {
public:
    explicit Fixture(int lookahead, int channels = 1, double sampleRate = kSR) {
        const std::size_t bytes = SPLimiterCore::memoryBytes(lookahead, channels);
        mWords = (bytes + sizeof(std::uint32_t) - 1) / sizeof(std::uint32_t);
        mBuf.assign(mWords + 2 * kGuard, kGuardWord);
        mCore.configure(lookahead, channels, sampleRate, mBuf.data() + kGuard);
    }

    SPLimiterCore& core() { return mCore; }

    bool guardsIntact() const {
        for (std::size_t i = 0; i < kGuard; ++i) {
            if (mBuf[i] != kGuardWord) return false;
            if (mBuf[mBuf.size() - 1 - i] != kGuardWord) return false;
        }
        return true;
    }

private:
    static constexpr std::size_t kGuard = 8;
    static constexpr std::uint32_t kGuardWord = 0xDEADBEEFu;

    SPLimiterCore mCore;
    std::size_t mWords = 0;
    std::vector<std::uint32_t> mBuf;
};

// Deterministic noise, no <random> so the sequence is identical on
// every platform and a failure is always reproducible.
class Rng {
public:
    explicit Rng(std::uint32_t seed) : mState(seed) {}
    // Uniform in [-1, 1).
    float next() {
        mState = mState * 1664525u + 1013904223u;
        return static_cast<float>(static_cast<std::int32_t>(mState)) / 2147483648.0f;
    }

private:
    std::uint32_t mState;
};

// Run a signal through in fixed-size blocks and return the output.
std::vector<float> run(SPLimiterCore& core, const std::vector<float>& in, float level,
                       float release, int blockSize) {
    std::vector<float> out(in.size(), 0.0f);
    std::size_t pos = 0;
    while (pos < in.size()) {
        const int n = static_cast<int>(std::min<std::size_t>(blockSize, in.size() - pos));
        core.process(in.data() + pos, out.data() + pos, n, level, release);
        pos += static_cast<std::size_t>(n);
    }
    return out;
}

// The assertion that matters. Returns the worst offending sample index,
// or -1 if the ceiling held throughout.
int firstOvershoot(const std::vector<float>& out, float level) {
    const float limit = level * (1.0f + kTol);
    for (std::size_t i = 0; i < out.size(); ++i) {
        if (!(std::fabs(out[i]) <= limit)) return static_cast<int>(i);
    }
    return -1;
}

}  // namespace

// =============================================================================
// The K <= D+1 invariant the ceiling proof rests on
// =============================================================================

TEST_CASE("SPLimiter kernel never outruns the look-ahead window", "[splimiter]") {
    for (int d = SPLimiterCore::kMinLookahead; d <= 4096; ++d) {
        const int k = SPLimiterCore::kernelLength(d);
        INFO("lookahead " << d << " gives kernel " << k);
        REQUIRE(k >= 1);
        REQUIRE(k <= d + 1);
    }
}

TEST_CASE("SPLimiter look-ahead is clamped to the supported range", "[splimiter]") {
    REQUIRE(SPLimiterCore::clampLookahead(0) == SPLimiterCore::kMinLookahead);
    REQUIRE(SPLimiterCore::clampLookahead(-100) == SPLimiterCore::kMinLookahead);
    REQUIRE(SPLimiterCore::clampLookahead(1 << 20) == SPLimiterCore::kMaxLookahead);

    // 1.5 ms at 48k is 72 samples.
    REQUIRE(SPLimiterCore::lookaheadSamples(0.0015f, 48000.0) == 72);
    // Nonsense durations fall back to the floor rather than allocating 0.
    REQUIRE(SPLimiterCore::lookaheadSamples(0.0f, 48000.0) == SPLimiterCore::kMinLookahead);
    REQUIRE(SPLimiterCore::lookaheadSamples(-1.0f, 48000.0) == SPLimiterCore::kMinLookahead);
    REQUIRE(SPLimiterCore::lookaheadSamples(1000.0f, 48000.0) == SPLimiterCore::kMaxLookahead);
}

// =============================================================================
// The ceiling, against signals designed to break it
// =============================================================================

TEST_CASE("SPLimiter holds the ceiling on adversarial signals", "[splimiter]") {
    const int lookahead = 72;
    const float level = 0.99f;
    const int n = 48000;

    struct Case {
        const char* name;
        std::vector<float> sig;
    };
    std::vector<Case> cases;

    // Loud white noise: peaks arriving with no warning, constantly.
    {
        std::vector<float> s(n);
        Rng rng(12345);
        for (int i = 0; i < n; ++i) s[i] = rng.next() * 4.0f;
        cases.push_back({"white noise at +12 dB", std::move(s)});
    }

    // Isolated unit impulses on silence, the worst case for any scheme
    // that smooths its gain, because there is nothing to mask the dip
    // and the gain must be all the way down for exactly one sample.
    {
        std::vector<float> s(n, 0.0f);
        for (int i = 100; i < n; i += 977) s[i] = 50.0f;
        cases.push_back({"sparse +34 dB impulses on silence", std::move(s)});
    }

    // Impulses closer together than the look-ahead window, so the
    // sliding minimum never gets to release between them.
    {
        std::vector<float> s(n, 0.0f);
        for (int i = 100; i < n; i += 7) s[i] = 20.0f;
        cases.push_back({"impulses spaced inside the window", std::move(s)});
    }

    // Silence to full scale with no ramp, which is the hardest edge for
    // the release to be caught out on.
    {
        std::vector<float> s(n, 0.0f);
        for (int i = n / 2; i < n; ++i) s[i] = 8.0f;
        cases.push_back({"instant step to +18 dB", std::move(s)});
    }

    // Low frequencies are where a limiter is most tempted to overshoot,
    // because a whole cycle is far longer than the look-ahead.
    for (double f : {20.0, 30.0, 50.0, 100.0}) {
        std::vector<float> s(n);
        for (int i = 0; i < n; ++i) {
            s[i] = 4.0f * static_cast<float>(std::sin(2.0 * kPi * f * i / kSR));
        }
        cases.push_back({"loud low sine", std::move(s)});
    }

    // A sweep, so no single period length is special.
    {
        std::vector<float> s(n);
        double phase = 0.0;
        for (int i = 0; i < n; ++i) {
            const double f = 20.0 + 4000.0 * (static_cast<double>(i) / n);
            phase += 2.0 * kPi * f / kSR;
            s[i] = 3.0f * static_cast<float>(std::sin(phase));
        }
        cases.push_back({"20 Hz to 4 kHz sweep at +9.5 dB", std::move(s)});
    }

    // Alternating full-scale samples: the fastest signal representable.
    {
        std::vector<float> s(n);
        for (int i = 0; i < n; ++i) s[i] = (i & 1) ? 6.0f : -6.0f;
        cases.push_back({"Nyquist square at +15.5 dB", std::move(s)});
    }

    // Enormous values from a runaway synth. Sanitize upstream catches
    // NaN and Inf, but finite-and-huge reaches the limiter intact.
    {
        std::vector<float> s(n, 0.0f);
        Rng rng(999);
        for (int i = 0; i < n; ++i) s[i] = rng.next() * 1.0e6f;
        cases.push_back({"runaway synth at 1e6", std::move(s)});
    }

    for (const auto& c : cases) {
        // Every block size, including ones that do not divide the
        // signal length, because the deque and the box filters carry
        // state across calls.
        for (int block : {1, 2, 3, 7, 64, 72, 73, 128, 512, 4096}) {
            Fixture fx(lookahead);
            const auto out = run(fx.core(), c.sig, level, 0.05f, block);
            const int bad = firstOvershoot(out, level);
            INFO(c.name << ", block " << block << ", first overshoot at " << bad);
            if (bad >= 0) {
                INFO("value " << out[static_cast<std::size_t>(bad)] << " vs ceiling " << level);
            }
            REQUIRE(bad == -1);
            REQUIRE(fx.guardsIntact());
        }
    }
}

TEST_CASE("SPLimiter holds the ceiling at every look-ahead", "[splimiter]") {
    const int n = 8000;
    std::vector<float> sig(n);
    Rng rng(4242);
    for (int i = 0; i < n; ++i) sig[i] = rng.next() * 10.0f;

    for (int d : {SPLimiterCore::kMinLookahead, 5, 6, 7, 8, 15, 16, 17, 64, 72, 100, 480, 2048}) {
        for (float level : {0.1f, 0.5f, 0.99f, 1.0f, 2.0f}) {
            Fixture fx(d);
            const auto out = run(fx.core(), sig, level, 0.05f, 128);
            INFO("lookahead " << d << ", level " << level);
            REQUIRE(firstOvershoot(out, level) == -1);
            REQUIRE(fx.guardsIntact());
        }
    }
}

// =============================================================================
// Latency and transparency
// =============================================================================

TEST_CASE("SPLimiter latency is exactly the look-ahead", "[splimiter]") {
    for (int d : {SPLimiterCore::kMinLookahead, 8, 33, 72, 480}) {
        Fixture fx(d);
        REQUIRE(fx.core().latencySamples() == d);

        // A marker well under the ceiling, so the gain stays at unity
        // and the only thing between input and output is the delay.
        std::vector<float> sig(4 * d + 64, 0.0f);
        sig[0] = 0.5f;
        const auto out = run(fx.core(), sig, 0.99f, 0.05f, 16);

        INFO("lookahead " << d);
        for (int i = 0; i < d; ++i) REQUIRE(out[static_cast<std::size_t>(i)] == 0.0f);
        REQUIRE(out[static_cast<std::size_t>(d)] == 0.5f);
    }
}

TEST_CASE("SPLimiter is transparent below the ceiling", "[splimiter]") {
    const int d = 72;
    const int n = 4096;
    Fixture fx(d);

    std::vector<float> sig(n);
    for (int i = 0; i < n; ++i) {
        sig[i] = 0.9f * static_cast<float>(std::sin(2.0 * kPi * 220.0 * i / kSR));
    }

    const auto out = run(fx.core(), sig, 0.99f, 0.05f, 64);

    // Bit-exact, not approximately: a gain of exactly 1.0 multiplied in
    // should not perturb a single sample.
    for (int i = d; i < n; ++i) {
        INFO("sample " << i);
        REQUIRE(out[static_cast<std::size_t>(i)] == sig[static_cast<std::size_t>(i - d)]);
    }
    REQUIRE(fx.core().currentGain() == 1.0f);
}

TEST_CASE("SPLimiter output does not depend on block size", "[splimiter]") {
    const int d = 72;
    const int n = 20000;
    std::vector<float> sig(n);
    Rng rng(777);
    for (int i = 0; i < n; ++i) sig[i] = rng.next() * 3.0f;

    Fixture ref(d);
    const auto expected = run(ref.core(), sig, 0.99f, 0.05f, n);

    for (int block : {1, 5, 32, 64, 100, 512}) {
        Fixture fx(d);
        const auto got = run(fx.core(), sig, 0.99f, 0.05f, block);
        INFO("block " << block);
        REQUIRE(got == expected);
    }

    // And with genuinely ragged blocks, as a hardware callback delivers
    // after a device change.
    {
        Fixture fx(d);
        std::vector<float> got(n, 0.0f);
        Rng chunks(31337);
        std::size_t pos = 0;
        while (pos < static_cast<std::size_t>(n)) {
            int c = 1 + static_cast<int>(std::fabs(chunks.next()) * 300.0f);
            c = static_cast<int>(std::min<std::size_t>(static_cast<std::size_t>(c), n - pos));
            fx.core().process(sig.data() + pos, got.data() + pos, c, 0.99f, 0.05f);
            pos += static_cast<std::size_t>(c);
        }
        REQUIRE(got == expected);
    }
}

TEST_CASE("SPLimiter processes in place", "[splimiter]") {
    const int d = 72;
    const int n = 8000;
    std::vector<float> sig(n);
    Rng rng(2024);
    for (int i = 0; i < n; ++i) sig[i] = rng.next() * 5.0f;

    Fixture ref(d);
    const auto expected = run(ref.core(), sig, 0.99f, 0.05f, 64);

    Fixture fx(d);
    std::vector<float> buf = sig;
    for (int pos = 0; pos < n; pos += 64) {
        const int c = std::min(64, n - pos);
        fx.core().process(buf.data() + pos, buf.data() + pos, c, 0.99f, 0.05f);
    }
    REQUIRE(buf == expected);
}

// =============================================================================
// Gain behaviour
// =============================================================================

TEST_CASE("SPLimiter recovers to unity after a transient", "[splimiter]") {
    const int d = 72;
    Fixture fx(d);

    // One loud hit, then four seconds of quiet. The tail of the
    // program-dependent release runs at ~0.4 s, so a couple of seconds
    // is not enough to call it settled.
    std::vector<float> sig(static_cast<std::size_t>(4 * kSR), 0.0f);
    for (int i = 1000; i < 1100; ++i) sig[static_cast<std::size_t>(i)] = 10.0f;

    const auto out = run(fx.core(), sig, 0.99f, 0.05f, 64);
    REQUIRE(firstOvershoot(out, 0.99f) == -1);

    // The hit must come out sitting just under the ceiling: limited,
    // not crushed. Anything well below 0.99 here would mean the gain
    // is over-reacting to a peak it had time to measure exactly.
    float peak = 0.0f;
    for (int i = 1000; i < 1200; ++i) {
        peak = std::max(peak, std::fabs(out[static_cast<std::size_t>(i)]));
    }
    INFO("limited peak " << peak);
    REQUIRE(peak > 0.9f);
    REQUIRE(peak <= 0.99f * (1.0f + kTol));

    // And the gain must then have released all the way back.
    REQUIRE(fx.core().currentGain() > 0.999f);
}

// The release is keyed on how long reduction has PERSISTED, not on how
// deep it is. An isolated transient recovers fast, masked by the
// transient that caused it, while sustained reduction recovers slowly,
// because a gain that snaps back between hits modulates the low end.
//
// Keying it on depth instead gets this backwards, and it shows up as
// intermodulation on driven programme material rather than as anything
// the ceiling tests would catch. Hence this test.
TEST_CASE("SPLimiter release slows when reduction persists", "[splimiter]") {
    const int d = 72;
    const float release = 0.05f;
    const float level = 0.99f;

    // Samples taken for the gain to climb back to `target` after the
    // signal goes quiet, given a preamble of `hits` loud impulses spaced
    // `spacing` samples apart. One hit is a transient; hundreds of them
    // over a second is sustained limiting.
    auto recovery = [&](int hits, int spacing, float target) {
        Fixture fx(d);
        const int lead = 500;
        std::vector<float> sig(static_cast<std::size_t>(6 * kSR), 0.0f);
        for (int h = 0; h < hits; ++h) {
            const std::size_t at = static_cast<std::size_t>(lead + h * spacing);
            if (at < sig.size()) sig[at] = 20.0f;
        }
        const int quietFrom = lead + hits * spacing + d;
        std::vector<float> out(sig.size(), 0.0f);
        int firstAt = -1;
        for (std::size_t i = 0; i < sig.size(); ++i) {
            fx.core().process(sig.data() + i, out.data() + i, 1, level, release);
            if (firstAt < 0 && static_cast<int>(i) > quietFrom &&
                fx.core().currentGain() >= target) {
                firstAt = static_cast<int>(i) - quietFrom;
            }
        }
        REQUIRE(firstOvershoot(out, level) == -1);
        return firstAt;
    };

    // Identical hit depth in both cases, so only persistence differs.
    const int transient = recovery(1, 1000, 0.9f);
    const int sustained = recovery(400, 120, 0.9f);

    INFO("transient recovered in " << transient << ", sustained in " << sustained);
    REQUIRE(transient > 0);
    REQUIRE(sustained > 0);
    // Comfortably slower, not marginally: the two limbs are 8x apart.
    REQUIRE(sustained > transient * 3);
}

TEST_CASE("SPLimiter tolerates degenerate parameters", "[splimiter]") {
    const int d = 72;
    const int n = 4000;
    std::vector<float> sig(n);
    Rng rng(55);
    for (int i = 0; i < n; ++i) sig[i] = rng.next() * 4.0f;

    // A zero or negative ceiling must not divide by zero or emit NaN.
    for (float level : {0.0f, -1.0f}) {
        Fixture fx(d);
        const auto out = run(fx.core(), sig, level, 0.05f, 64);
        for (float v : out) {
            REQUIRE(std::isfinite(v));
            REQUIRE(std::fabs(v) <= 1e-5f);
        }
    }

    // Likewise a zero or negative release.
    for (float rel : {0.0f, -0.5f}) {
        Fixture fx(d);
        const auto out = run(fx.core(), sig, 0.99f, rel, 64);
        for (float v : out) REQUIRE(std::isfinite(v));
        REQUIRE(firstOvershoot(out, 0.99f) == -1);
    }
}

TEST_CASE("SPLimiter reset returns it to a clean state", "[splimiter]") {
    const int d = 72;
    const int n = 4000;
    std::vector<float> sig(n);
    Rng rng(88);
    for (int i = 0; i < n; ++i) sig[i] = rng.next() * 4.0f;

    Fixture fx(d);
    const auto first = run(fx.core(), sig, 0.99f, 0.05f, 64);
    fx.core().reset();
    REQUIRE(fx.core().currentGain() == 1.0f);
    const auto second = run(fx.core(), sig, 0.99f, 0.05f, 64);
    REQUIRE(second == first);
}

// =============================================================================
// Stereo linking
//
// The point of the linked detector is that L and R are always scaled by
// the identical gain, so the stereo image cannot move when the limiter
// works. Two independent mono instances shift the image on any peak that
// is not symmetric, which is measurable on real programme material.
// =============================================================================

namespace {

// Asymmetric stereo programme: the two channels share nothing, and each
// takes turns being the louder one.
void asymmetricStereo(int n, std::vector<float>& l, std::vector<float>& r) {
    l.assign(n, 0.0f);
    r.assign(n, 0.0f);
    Rng rng(606);
    for (int i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / kSR;
        l[static_cast<std::size_t>(i)] =
            3.0f * static_cast<float>(std::sin(2.0 * kPi * 70.0 * t)) + rng.next() * 0.4f;
        r[static_cast<std::size_t>(i)] =
            0.6f * static_cast<float>(std::sin(2.0 * kPi * 190.0 * t)) + rng.next() * 0.4f;
        // Hard one-sided hits, alternating channel.
        if (i % 3000 == 500) l[static_cast<std::size_t>(i)] = 30.0f;
        if (i % 3000 == 1900) r[static_cast<std::size_t>(i)] = 24.0f;
    }
}

}  // namespace

TEST_CASE("SPLimiter2 applies one identical gain to both channels", "[splimiter]") {
    const int d = 72;
    const int n = 48000;
    const float level = 0.99f;

    std::vector<float> l, r;
    asymmetricStereo(n, l, r);

    for (int block : {1, 7, 64, 512}) {
        Fixture fx(d, 2);
        std::vector<float> ol(n, 0.0f), orr(n, 0.0f), gain(n, 0.0f);
        for (int pos = 0; pos < n; pos += block) {
            const int c = std::min(block, n - pos);
            fx.core().process(l.data() + pos, r.data() + pos, ol.data() + pos, orr.data() + pos, c,
                              level, 0.05f, gain.data() + pos);
        }

        INFO("block " << block);
        REQUIRE(firstOvershoot(ol, level) == -1);
        REQUIRE(firstOvershoot(orr, level) == -1);
        REQUIRE(fx.guardsIntact());

        // The reported gain must be exactly the gain actually applied,
        // on both channels, this is what a meter reads.
        for (int i = d; i < n; ++i) {
            const auto u = static_cast<std::size_t>(i);
            const auto v = static_cast<std::size_t>(i - d);
            REQUIRE(ol[u] == gain[u] * l[v]);
            REQUIRE(orr[u] == gain[u] * r[v]);
        }
    }
}

TEST_CASE("SPLimiter2 leaves the stereo image exactly where it was", "[splimiter]") {
    const int d = 72;
    const int n = 48000;
    const float level = 0.99f;

    std::vector<float> l, r;
    asymmetricStereo(n, l, r);

    Fixture linked(d, 2);
    std::vector<float> ol(n, 0.0f), orr(n, 0.0f);
    linked.core().process(l.data(), r.data(), ol.data(), orr.data(), n, level, 0.05f);

    // Worst-case divergence between the gain applied to L and to R,
    // measured only where there is signal to divide by.
    auto worstLinkErrorDb = [&](const std::vector<float>& outL, const std::vector<float>& outR) {
        double worst = 0.0;
        for (int i = d; i < n; ++i) {
            const auto u = static_cast<std::size_t>(i);
            const auto v = static_cast<std::size_t>(i - d);
            if (std::fabs(l[v]) < 0.05f || std::fabs(r[v]) < 0.05f) continue;
            const double gl = outL[u] / l[v];
            const double gr = outR[u] / r[v];
            if (gl <= 0.0 || gr <= 0.0) continue;
            worst = std::max(worst, std::fabs(20.0 * std::log10(gl / gr)));
        }
        return worst;
    };

    const double linkedErr = worstLinkErrorDb(ol, orr);
    INFO("linked worst-case link error " << linkedErr << " dB");
    // Float division either side of an identical multiply, so this is
    // rounding only, not a tolerance on a design compromise.
    REQUIRE(linkedErr < 1e-3);

    // Two independent mono instances on the same material, for contrast:
    // the whole reason the linked version exists.
    Fixture ml(d), mr(d);
    std::vector<float> il(n, 0.0f), ir(n, 0.0f);
    ml.core().process(l.data(), il.data(), n, level, 0.05f);
    mr.core().process(r.data(), ir.data(), n, level, 0.05f);
    const double unlinkedErr = worstLinkErrorDb(il, ir);
    INFO("unlinked worst-case link error " << unlinkedErr << " dB");
    REQUIRE(unlinkedErr > 1.0);
}

TEST_CASE("SPLimiter2 is transparent below the ceiling", "[splimiter]") {
    const int d = 72;
    const int n = 4096;
    Fixture fx(d, 2);

    std::vector<float> l(n), r(n);
    for (int i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / kSR;
        l[static_cast<std::size_t>(i)] = 0.9f * static_cast<float>(std::sin(2.0 * kPi * 220.0 * t));
        r[static_cast<std::size_t>(i)] = 0.4f * static_cast<float>(std::sin(2.0 * kPi * 330.0 * t));
    }

    std::vector<float> ol(n, 0.0f), orr(n, 0.0f), gain(n, 0.0f);
    for (int pos = 0; pos < n; pos += 64) {
        const int c = std::min(64, n - pos);
        fx.core().process(l.data() + pos, r.data() + pos, ol.data() + pos, orr.data() + pos, c,
                          0.99f, 0.05f, gain.data() + pos);
    }

    for (int i = d; i < n; ++i) {
        const auto u = static_cast<std::size_t>(i);
        const auto v = static_cast<std::size_t>(i - d);
        REQUIRE(gain[u] == 1.0f);
        REQUIRE(ol[u] == l[v]);
        REQUIRE(orr[u] == r[v]);
    }
}

// =============================================================================
// Long-run behaviour
//
// The sliding-minimum window compares sample indices, which wrap after
// 2^32 samples, a day at 48 kHz, well inside a Sonic Pi installation's
// uptime. Hidden by default because it has to actually reach the wrap.
// =============================================================================

TEST_CASE("SPLimiter survives the sample-index wrap", "[.][splimiter-wrap]") {
    const int d = 72;
    Fixture fx(d);

    // Enough to cross 2^32 twice over, in blocks, alternating between a
    // loud sample and quiet so the deque is exercised rather than idle.
    const int block = 512;
    std::vector<float> in(block), out(block);
    const std::uint64_t total = (1ull << 32) + (1ull << 20);
    std::uint64_t done = 0;
    std::uint32_t phase = 0;
    while (done < total) {
        for (int i = 0; i < block; ++i) {
            phase = phase * 1664525u + 1013904223u;
            in[static_cast<std::size_t>(i)] = (phase & 0x3F) == 0 ? 5.0f : 0.2f;
        }
        fx.core().process(in.data(), out.data(), block, 0.99f, 0.05f);
        REQUIRE(firstOvershoot(out, 0.99f) == -1);
        done += static_cast<std::uint64_t>(block);
    }
    REQUIRE(fx.guardsIntact());
}

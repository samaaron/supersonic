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
#include <limits>
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

// The correctly-rounded float product of two floats. Two 24-bit mantissas
// multiply exactly inside a double's 53, so rounding that once to float is
// by definition what float arithmetic owes — computing it this way keeps an
// exact comparison from depending on whether the compiler happened to keep
// an intermediate in a wider register (32-bit x86 evaluates float
// expressions in 80-bit x87 registers unless told otherwise).
float exactProduct(float a, float b) {
    return static_cast<float>(static_cast<double>(a) * static_cast<double>(b));
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
            REQUIRE(ol[u] == exactProduct(gain[u], l[v]));
            REQUIRE(orr[u] == exactProduct(gain[u], r[v]));
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
// Never louder than it was given
//
// A limiter that can boost is a fader with a bug in it. The gain path is
// built from r = min(1, L/|x|), a sliding minimum, an envelope that only
// sits below its input, and a normalised kernel over values that are all
// <= 1, so unity is a hard upper bound at every stage.
// =============================================================================

TEST_CASE("SPLimiter never boosts", "[splimiter]") {
    const int d = 72;
    const int n = 24000;

    // Quiet, loud, and straddling the ceiling: a boost bug could hide in
    // any one of them.
    for (float drive : {0.01f, 0.5f, 0.99f, 1.01f, 8.0f}) {
        std::vector<float> sig(n);
        Rng rng(606);
        for (int i = 0; i < n; ++i) sig[i] = rng.next() * drive;

        Fixture fx(d);
        std::vector<float> out(n, 0.0f), gain(n, 0.0f);
        for (int pos = 0; pos < n; pos += 64) {
            const int c = std::min(64, n - pos);
            fx.core().process(sig.data() + pos, out.data() + pos, c, 0.99f, 0.05f,
                              gain.data() + pos);
        }

        INFO("drive " << drive);
        for (int i = d; i < n; ++i) {
            const auto u = static_cast<std::size_t>(i);
            const auto v = static_cast<std::size_t>(i - d);
            REQUIRE(gain[u] <= 1.0f);
            REQUIRE(gain[u] >= 0.0f);
            // The sample it carries can only have got quieter.
            REQUIRE(std::fabs(out[u]) <= std::fabs(sig[v]));
        }
    }
}

// =============================================================================
// Gain continuity
//
// A step in the gain is a click, and on a master bus a click is audible
// over everything. The smoother is three cascaded boxes of length B, and
// each box is a running mean, so one sample can move its output by at
// most (range of its input)/B. The input to the first box is in [0, 1],
// so 1/B bounds the whole cascade. Asserting against the derived bound
// rather than a measured constant keeps this meaningful if the kernel is
// ever retuned.
// =============================================================================

TEST_CASE("SPLimiter gain moves too smoothly to click", "[splimiter]") {
    const int n = 24000;

    // Sparse full-scale impulses on silence: the largest gain excursion
    // the smoother can be asked for, since the gain must go from unity to
    // deep reduction and back with nothing in between.
    std::vector<float> sig(n, 0.0f);
    for (int i = 100; i < n; i += 977) sig[static_cast<std::size_t>(i)] = 50.0f;

    double previous = 1.0;
    for (int d : {SPLimiterCore::kMinLookahead, 8, 16, 32, 72, 144, 480}) {
        Fixture fx(d);
        std::vector<float> out(n, 0.0f), gain(n, 0.0f);
        for (int pos = 0; pos < n; pos += 64) {
            const int c = std::min(64, n - pos);
            fx.core().process(sig.data() + pos, out.data() + pos, c, 0.99f, 0.05f,
                              gain.data() + pos);
        }

        double worst = 0.0;
        for (std::size_t i = 1; i < gain.size(); ++i) {
            worst = std::max(worst, std::fabs(static_cast<double>(gain[i]) - gain[i - 1]));
        }
        const double bound = 1.0 / SPLimiterCore::boxLength(d);

        INFO("lookahead " << d << ", worst step " << worst << ", bound " << bound);
        REQUIRE(worst <= bound);

        // And a longer look-ahead is always the smoother one, which is
        // what makes the parameter mean something.
        REQUIRE(worst < previous);
        previous = worst;
    }
}

// =============================================================================
// Non-finite input
//
// A runaway synth reaches the master bus as finite-but-enormous, or as
// Inf/NaN if sanitize upstream ever lets one through. Garbage in may be
// garbage out for the samples themselves, but it must not poison the
// limiter: the gain state has to survive so the *next* sound is limited
// correctly. A master limiter that latches on one bad sample takes the
// whole session down with it.
// =============================================================================

TEST_CASE("SPLimiter contains non-finite input and recovers", "[splimiter]") {
    const int d = 72;
    const int n = 24000;
    const int burstFrom = 1000;
    const int burstLen = 10;
    const float level = 0.99f;

    struct Case { const char* name; float value; };
    const Case cases[] = {
        {"+Inf", std::numeric_limits<float>::infinity()},
        {"-Inf", -std::numeric_limits<float>::infinity()},
        {"NaN", std::numeric_limits<float>::quiet_NaN()},
        {"1e30", 1e30f},
        {"denormal", 1e-40f},
    };

    for (const auto& c : cases) {
        Fixture fx(d);
        // Ordinary quiet programme either side of the burst, so the tail
        // shows whether the limiter came back.
        std::vector<float> sig(n, 0.0f);
        for (int i = 0; i < n; ++i) {
            sig[static_cast<std::size_t>(i)] =
                0.3f * static_cast<float>(std::sin(2.0 * kPi * 220.0 * i / kSR));
        }
        for (int i = burstFrom; i < burstFrom + burstLen; ++i) {
            sig[static_cast<std::size_t>(i)] = c.value;
        }

        std::vector<float> out(n, 0.0f);
        for (int pos = 0; pos < n; pos += 64) {
            const int cc = std::min(64, n - pos);
            fx.core().process(sig.data() + pos, out.data() + pos, cc, level, 0.05f);
        }

        INFO(c.name);

        // Contamination is confined to the samples that actually carried
        // it through the delay line — it does not spread.
        int nonFinite = 0;
        for (int i = 0; i < n; ++i) {
            if (!std::isfinite(out[static_cast<std::size_t>(i)])) ++nonFinite;
        }
        REQUIRE(nonFinite <= burstLen);

        // Well past the burst, the limiter is doing its job again: finite,
        // under the ceiling, and still passing audio rather than sitting
        // at zero gain forever.
        double tailPeak = 0.0;
        for (int i = n / 2; i < n; ++i) {
            const float v = out[static_cast<std::size_t>(i)];
            REQUIRE(std::isfinite(v));
            tailPeak = std::max(tailPeak, static_cast<double>(std::fabs(v)));
        }
        REQUIRE(tailPeak <= level * (1.0 + kTol));
        REQUIRE(tailPeak > 0.1);
        REQUIRE(std::isfinite(fx.core().currentGain()));
        REQUIRE(fx.core().currentGain() > 0.5f);
        REQUIRE(fx.guardsIntact());
    }
}

// =============================================================================
// Sample rate
//
// Sonic Pi runs at whatever the device gives it. The look-ahead is
// specified in seconds and converted per rate, so the guarantee has to
// survive that conversion rather than holding only at 48k.
// =============================================================================

TEST_CASE("SPLimiter holds the ceiling at every sample rate", "[splimiter]") {
    const float level = 0.99f;
    for (double sr : {44100.0, 48000.0, 88200.0, 96000.0, 192000.0}) {
        const int d = SPLimiterCore::lookaheadSamples(0.0015f, sr);
        Fixture fx(d, 1, sr);
        REQUIRE(fx.core().latencySamples() == d);

        const int n = static_cast<int>(sr);
        std::vector<float> sig(n);
        Rng rng(19);
        for (int i = 0; i < n; ++i) sig[i] = rng.next() * 6.0f;

        const auto out = run(fx.core(), sig, level, 0.05f, 64);
        INFO("sample rate " << sr << ", lookahead " << d);
        REQUIRE(firstOvershoot(out, level) == -1);
        REQUIRE(fx.guardsIntact());
    }
}

// =============================================================================
// Mono and stereo agree
//
// SPLimiter2 fed the same signal on both channels must be the mono
// SPLimiter, exactly. If the two paths ever drift apart, a mix that
// happens to be mono changes when it is routed through the stereo UGen.
// =============================================================================

TEST_CASE("SPLimiter2 on identical channels matches the mono path", "[splimiter]") {
    const int d = 72;
    const int n = 24000;
    std::vector<float> sig(n);
    Rng rng(5150);
    for (int i = 0; i < n; ++i) sig[i] = rng.next() * 5.0f;

    Fixture mono(d, 1);
    const auto om = run(mono.core(), sig, 0.99f, 0.05f, 64);

    Fixture stereo(d, 2);
    std::vector<float> ol(n, 0.0f), orr(n, 0.0f);
    for (int pos = 0; pos < n; pos += 64) {
        const int c = std::min(64, n - pos);
        stereo.core().process(sig.data() + pos, sig.data() + pos, ol.data() + pos,
                              orr.data() + pos, c, 0.99f, 0.05f);
    }

    // Bit-exact, not close: the detector sees max(|l|,|r|) = |x| and the
    // arithmetic is otherwise identical, so there is nothing to round
    // differently.
    REQUIRE(ol == om);
    REQUIRE(orr == om);
}

// =============================================================================
// Distortion
//
// Multiplying by a gain that moves within a cycle *is* harmonic
// distortion, and it is the measurement that separates a transparent
// limiter from an audibly grubby one. The sliding minimum runs over
// D+1 samples, and |x| of a sine repeats every half period, so once
// D >= sr/(2f) the window always spans a whole half period, the minimum
// is constant, and the gain stops rippling: distortion goes to zero, not
// merely low. Below that corner the ripple is bounded and falls fast
// with frequency.
// =============================================================================

namespace {

// Magnitude of one bin, over an exact integer number of periods so no
// leakage lands in the harmonic bins.
double binMag(const std::vector<float>& x, int from, int len, double freq, double sr) {
    double re = 0.0, im = 0.0;
    for (int i = 0; i < len; ++i) {
        const double ph = 2.0 * kPi * freq * i / sr;
        re += x[static_cast<std::size_t>(from + i)] * std::cos(ph);
        im -= x[static_cast<std::size_t>(from + i)] * std::sin(ph);
    }
    return 2.0 * std::sqrt(re * re + im * im) / len;
}

// Total harmonic distortion of the steady-state output for a sine of
// `freq` driven `drive` times over the ceiling.
double measureThd(int d, double freq, float drive, float level) {
    const int period = static_cast<int>(kSR / freq);
    const int cycles = std::max(8, static_cast<int>(kSR / period));
    const int len = period * cycles;
    const int settle = static_cast<int>(kSR);  // a second is far past the release
    const int n = len + 2 * settle;

    std::vector<float> sig(n);
    for (int i = 0; i < n; ++i) {
        sig[static_cast<std::size_t>(i)] =
            drive * static_cast<float>(std::sin(2.0 * kPi * freq * i / kSR));
    }

    Fixture fx(d);
    const auto out = run(fx.core(), sig, level, 0.05f, 64);

    const double fund = binMag(out, settle, len, freq, kSR);
    double harmonics = 0.0;
    for (int h = 2; h <= 20; ++h) {
        const double hf = freq * h;
        if (hf >= kSR / 2.0) break;
        const double m = binMag(out, settle, len, hf, kSR);
        harmonics += m * m;
    }
    return std::sqrt(harmonics) / fund;
}

}  // namespace

TEST_CASE("SPLimiter is distortion-free above the look-ahead corner", "[splimiter]") {
    const int d = 72;
    const float level = 0.99f;
    // Frequencies chosen to divide 48 kHz exactly, so every harmonic bin
    // is an exact integer number of periods.
    const double corner = kSR / (2.0 * d);  // 333 Hz at d = 72

    for (double f : {500.0, 1000.0, 4000.0}) {
        REQUIRE(f > corner);
        const double thd = measureThd(d, f, 2.0f, level);
        INFO("freq " << f << " THD " << (thd * 100.0) << " %");
        // Numerically zero: the gain is genuinely constant here, so this
        // is float noise and nothing else.
        REQUIRE(thd < 1e-6);
    }
}

TEST_CASE("SPLimiter keeps low-frequency distortion bounded", "[splimiter]") {
    const int d = 72;
    const float level = 0.99f;

    // Below the corner the gain does ripple. These bounds sit just above
    // what the current design measures, so a change that made the limiter
    // grubbier would fail rather than pass quietly.
    struct Bound { double freq; double maxThdPercent; };
    const Bound bounds[] = {
        {25.0, 1.0},   // measures 0.74 %
        {30.0, 0.8},   // measures 0.60 %
        {50.0, 0.4},   // measures 0.29 %
        {100.0, 0.1},  // measures 0.066 %
        {200.0, 0.01}, // measures 0.0021 %
    };

    double previous = 100.0;
    for (const auto& b : bounds) {
        const double thd = measureThd(d, b.freq, 2.0f, level) * 100.0;
        INFO("freq " << b.freq << " THD " << thd << " % (bound " << b.maxThdPercent << " %)");
        REQUIRE(thd < b.maxThdPercent);
        // Monotonic: distortion falls as the window covers more of the
        // half period.
        REQUIRE(thd < previous);
        previous = thd;
    }
}

TEST_CASE("SPLimiter distortion falls as look-ahead grows", "[splimiter]") {
    const float level = 0.99f;
    const double f = 50.0;

    // 50 Hz has a half period of 480 samples, so the corner is crossed at
    // exactly that look-ahead and distortion should vanish there.
    double previous = 100.0;
    for (int d : {16, 32, 72, 144}) {
        const double thd = measureThd(d, f, 2.0f, level);
        INFO("lookahead " << d << " THD " << (thd * 100.0) << " %");
        REQUIRE(thd < previous);
        previous = thd;
    }
    REQUIRE(measureThd(480, f, 2.0f, level) < 1e-6);
}

// =============================================================================
// Inter-sample peaks
//
// The ceiling guarantee is on sample values. A D/A converter, a sample
// rate converter and every lossy encoder reconstruct the waveform
// *between* the samples, and that reconstruction can be higher than any
// sample in it. This is what "true peak" means in BS.1770, and it is the
// one place where a correct sample-peak limiter still hands a clipped
// signal to the outside world.
//
// The interpolator below is checked against signals whose true peak is
// known analytically before it is used to judge the limiter, because a
// measurement instrument asserted against nothing is worth nothing.
// =============================================================================

namespace {

// Windowed-sinc fractional interpolator. The window is centred on the
// interpolation point rather than on the tap index, and each phase is
// normalised to unit DC gain; getting either wrong shows up immediately
// in the self-check below.
class Oversampler {
public:
    Oversampler(int factor, int halfTaps) : mOs(factor), mHalf(halfTaps) {
        mTaps.resize(static_cast<std::size_t>(mOs) * 2 * mHalf);
        for (int p = 0; p < mOs; ++p) {
            const double frac = static_cast<double>(p) / mOs;
            double sum = 0.0;
            for (int k = -mHalf; k < mHalf; ++k) {
                const double t = k - frac;
                const double s =
                    (std::fabs(t) < 1e-12) ? 1.0 : std::sin(kPi * t) / (kPi * t);
                const double u = (t + mHalf) / (2.0 * mHalf);
                const double w = (u < 0.0 || u > 1.0)
                                     ? 0.0
                                     : 0.35875 - 0.48829 * std::cos(2 * kPi * u) +
                                           0.14128 * std::cos(4 * kPi * u) -
                                           0.01168 * std::cos(6 * kPi * u);
                mTaps[idx(p, k)] = s * w;
                sum += s * w;
            }
            for (int k = -mHalf; k < mHalf; ++k) mTaps[idx(p, k)] /= sum;
        }
    }

    double peak(const std::vector<float>& x) const {
        double pk = 0.0;
        const int n = static_cast<int>(x.size());
        for (int i = mHalf; i < n - mHalf; ++i) {
            for (int p = 0; p < mOs; ++p) {
                double acc = 0.0;
                for (int k = -mHalf; k < mHalf; ++k) {
                    acc += x[static_cast<std::size_t>(i + k)] * mTaps[idx(p, k)];
                }
                pk = std::max(pk, std::fabs(acc));
            }
        }
        return pk;
    }

private:
    std::size_t idx(int p, int k) const {
        return static_cast<std::size_t>(p) * 2 * mHalf + static_cast<std::size_t>(k + mHalf);
    }

    int mOs;
    int mHalf;
    std::vector<double> mTaps;
};

const Oversampler& oversampler() {
    static const Oversampler os(8, 32);  // BS.1770 asks for >= 4x
    return os;
}

}  // namespace

TEST_CASE("inter-sample peak meter reads known signals correctly", "[splimiter]") {
    const int n = 8192;
    const auto& os = oversampler();

    // DC: nothing happens between the samples.
    {
        std::vector<float> x(n, 0.7f);
        REQUIRE(std::fabs(os.peak(x) - 0.7) < 1e-4);
    }
    // A sine well below Nyquist peaks at its amplitude wherever it is sampled.
    {
        std::vector<float> x(n);
        for (int i = 0; i < n; ++i) {
            x[static_cast<std::size_t>(i)] =
                0.8f * static_cast<float>(std::sin(2.0 * kPi * 1000.0 * i / kSR));
        }
        REQUIRE(std::fabs(os.peak(x) - 0.8) < 1e-4);
    }
    // The canonical +3 dB case: a quarter-rate sine sampled at 45 degrees,
    // so every sample sits at 0.8/sqrt(2) and the true peak is still 0.8.
    {
        std::vector<float> x(n);
        for (int i = 0; i < n; ++i) {
            x[static_cast<std::size_t>(i)] = 0.8f * static_cast<float>(std::sin(
                2.0 * kPi * (kSR / 4.0) * i / kSR + kPi / 4.0));
        }
        double samplePeak = 0.0;
        for (float v : x) samplePeak = std::max(samplePeak, static_cast<double>(std::fabs(v)));
        REQUIRE(std::fabs(samplePeak - 0.8 / std::sqrt(2.0)) < 1e-4);
        REQUIRE(std::fabs(os.peak(x) - 0.8) < 1e-4);
    }
    // Alternating samples are a Nyquist-rate cosine: reconstruction peaks
    // at the sample value, not above it.
    {
        std::vector<float> x(n);
        for (int i = 0; i < n; ++i) x[static_cast<std::size_t>(i)] = (i & 1) ? 0.99f : -0.99f;
        REQUIRE(std::fabs(os.peak(x) - 0.99) < 1e-4);
    }
}

TEST_CASE("SPLimiter keeps inter-sample peaks tight on band-limited audio", "[splimiter]") {
    const int d = 72;
    const int n = 16384;
    const float level = 0.99f;
    const auto& os = oversampler();

    // Programme material is band-limited well below Nyquist, which is the
    // case that has to stay clean: a converter fed this must not clip.
    struct Case { const char* name; std::vector<float> sig; };
    std::vector<Case> cases;
    {
        std::vector<float> s(n);
        double phase = 0.0;
        for (int i = 0; i < n; ++i) {
            const double f = 20.0 + 8000.0 * (static_cast<double>(i) / n);
            phase += 2.0 * kPi * f / kSR;
            s[static_cast<std::size_t>(i)] = 3.0f * static_cast<float>(std::sin(phase));
        }
        cases.push_back({"20 Hz to 8 kHz sweep at +9.5 dB", std::move(s)});
    }
    {
        std::vector<float> s(n);
        for (int i = 0; i < n; ++i) {
            const double t = static_cast<double>(i) / kSR;
            s[static_cast<std::size_t>(i)] =
                static_cast<float>(2.5 * std::sin(2.0 * kPi * 110.0 * t) +
                                   1.5 * std::sin(2.0 * kPi * 440.0 * t) +
                                   0.8 * std::sin(2.0 * kPi * 1320.0 * t));
        }
        cases.push_back({"harmonic stack at +12 dB", std::move(s)});
    }

    for (const auto& c : cases) {
        Fixture fx(d);
        const auto out = run(fx.core(), c.sig, level, 0.05f, 64);
        REQUIRE(firstOvershoot(out, level) == -1);

        const double tp = os.peak(out);
        const double overDb = 20.0 * std::log10(tp / level);
        INFO(c.name << ": true peak " << tp << " (" << overDb << " dB over the ceiling)");
        // Measures under 0.03 dB for both. Half a dB is still far below
        // anything audible and leaves room for the meter's own error.
        REQUIRE(overDb < 0.5);
    }
}

// Full-band content is a different matter, and this test exists to record
// that honestly rather than to claim a guarantee the design does not make.
// White noise has independent consecutive samples, so its reconstruction
// swings hard between them: the *input* here already has inter-sample
// peaks ~6 dB above its sample peak. A sample-peak limiter preserves that
// ratio, so the output does too. Closing this needs an oversampled
// detector, which costs latency and CPU; the sample-peak ceiling is exact
// either way.
TEST_CASE("SPLimiter inter-sample behaviour on full-band content is bounded",
          "[splimiter]") {
    const int d = 72;
    const int n = 16384;
    const float level = 0.99f;
    const auto& os = oversampler();

    std::vector<float> sig(n);
    Rng rng(12345);
    for (int i = 0; i < n; ++i) sig[i] = rng.next() * 4.0f;

    Fixture fx(d);
    const auto out = run(fx.core(), sig, level, 0.05f, 64);

    // The guarantee the design actually makes still holds exactly.
    REQUIRE(firstOvershoot(out, level) == -1);

    const double inTp = os.peak(sig);
    double inSample = 0.0;
    for (float v : sig) inSample = std::max(inSample, static_cast<double>(std::fabs(v)));
    const double outTp = os.peak(out);

    // The input's own inter-sample excess, which the limiter inherits
    // rather than causes.
    const double inExcessDb = 20.0 * std::log10(inTp / inSample);
    const double outExcessDb = 20.0 * std::log10(outTp / level);
    INFO("input excess " << inExcessDb << " dB, output excess " << outExcessDb << " dB");
    REQUIRE(inExcessDb > 3.0);
    // Measures ~5.7 dB. The bound catches a regression that made the
    // limiter itself a source of inter-sample peaks.
    REQUIRE(outExcessDb < 7.0);
}

// =============================================================================
// A moving ceiling
//
// The proof is for a constant L. Audio already inside the delay line was
// cleared against the ceiling in force when it entered, so lowering the
// ceiling cannot retroactively apply to it: for up to D samples the
// output can exceed the new ceiling, by at most the ratio of old to new.
// That is a real property of the design, so it is pinned here rather than
// left to be discovered on a master bus.
// =============================================================================

TEST_CASE("SPLimiter applies a lowered ceiling within the look-ahead", "[splimiter]") {
    const int d = 72;
    const int n = 24000;
    const float high = 0.9f;
    const float low = 0.1f;

    std::vector<float> sig(n);
    Rng rng(31);
    for (int i = 0; i < n; ++i) sig[i] = rng.next() * 4.0f;

    const int stepAt = 8000;
    Fixture fx(d);
    std::vector<float> out(n, 0.0f);
    for (int pos = 0; pos < n; pos += 64) {
        const int c = std::min(64, n - pos);
        const float level = pos < stepAt ? high : low;
        fx.core().process(sig.data() + pos, out.data() + pos, c, level, 0.05f);
    }

    // Before the step the old ceiling holds exactly.
    for (int i = 0; i < stepAt; ++i) {
        REQUIRE(std::fabs(out[static_cast<std::size_t>(i)]) <= high * (1.0f + kTol));
    }

    // Across the step the excess is bounded by the ratio of the ceilings,
    // never worse.
    int lastOver = -1;
    double worst = 0.0;
    for (int i = stepAt; i < n; ++i) {
        const double v = std::fabs(out[static_cast<std::size_t>(i)]);
        worst = std::max(worst, v);
        if (v > low * (1.0 + kTol)) lastOver = i;
    }
    INFO("worst " << worst << ", settles by sample " << lastOver << " of " << stepAt);
    REQUIRE(worst <= static_cast<double>(high) * (1.0 + kTol));

    // And it is over within the look-ahead, not lingering.
    REQUIRE(lastOver >= 0);
    REQUIRE(lastOver - stepAt < d);

    // Long after the step the new ceiling is exact.
    for (int i = stepAt + d; i < n; ++i) {
        REQUIRE(std::fabs(out[static_cast<std::size_t>(i)]) <= low * (1.0f + kTol));
    }

    // Raising the ceiling is safe at any moment, since the old gain was
    // only ever more conservative.
    Fixture up(d);
    std::vector<float> upOut(n, 0.0f);
    for (int pos = 0; pos < n; pos += 64) {
        const int c = std::min(64, n - pos);
        const float level = pos < stepAt ? low : high;
        up.core().process(sig.data() + pos, upOut.data() + pos, c, level, 0.05f);
    }
    for (int i = 0; i < n; ++i) {
        const float level = (i < stepAt) ? low : high;
        REQUIRE(std::fabs(upOut[static_cast<std::size_t>(i)]) <= level * (1.0f + kTol));
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

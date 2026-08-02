/*
 * test_time_source.cpp — deterministic scenario tests for the native
 * TimeSource (WallClock + IIR audio clock), driven through the injectable
 * test wall clock so every scenario has an exact expected answer.
 *
 * Scenarios pinned:
 *   - a multi-second audio-callback stall must re-converge at the IIR rate
 *     without the clock ever running backwards (stall scenario);
 *   - sample accounting mismatched with the device rate (possible across
 *     device swaps) produces a persistent drift the 1%-per-callback
 *     proportional corrector can never eliminate — its steady-state error
 *     under a rate ramp is exactly mismatchFraction * dtPerUpdate / 0.01
 *     (rate-mismatch scenario).
 */
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "native/TimeSource.h"

#include <cmath>

using Catch::Approx;

namespace {

constexpr double kRate  = 48000.0;
constexpr double kBlock = 128.0;
constexpr double kDt    = kBlock / kRate;  // seconds of audio per update
constexpr double kAnchorNTP = 3'900'000'000.0;  // arbitrary plausible NTP

struct ClockRig {
    TimeSource ts;
    double wall    = kAnchorNTP;
    double samples = 0.0;

    ClockRig() {
        ts.setTestWallClock(wall);
        ts.resetAudioThreadTime(samples, kRate);
    }

    // One audio callback: samples and wall advance (independently
    // controllable), then the IIR runs. Returns the clock's NTP answer.
    double step(double sampleAdvance = kBlock, double wallAdvance = kDt) {
        samples += sampleAdvance;
        wall    += wallAdvance;
        ts.setTestWallClock(wall);
        return ts.updateAudioThreadNTP(samples, kRate, 0.0);
    }
};

} // namespace

TEST_CASE("TimeSource: lockstep steady state tracks wall clock exactly",
          "[TimeSource]") {
    ClockRig rig;
    for (int i = 0; i < 2000; ++i) {
        const double ntp = rig.step();
        REQUIRE(ntp == Approx(rig.wall).margin(1e-6));
    }
    REQUIRE(rig.ts.now() == Approx(rig.wall).margin(1e-6));
}

TEST_CASE("TimeSource: 1s callback stall converges at the IIR rate, "
          "monotonically", "[TimeSource]") {
    ClockRig rig;
    for (int i = 0; i < 500; ++i) rig.step();

    // The stall: wall advances 1s while the audio thread delivers nothing
    // (no updates). The first callback after resume sees +1s of drift.
    rig.wall += 1.0;
    rig.ts.setTestWallClock(rig.wall);

    double prev = rig.ts.now();
    double firstGapError = 0.0;
    for (int i = 0; i < 1500; ++i) {
        const double ntp = rig.step();
        if (i == 0) firstGapError = rig.wall - ntp;
        // Time must never run backwards while re-converging.
        REQUIRE(ntp >= prev);
        prev = ntp;
    }
    // First post-stall answer is still ~1s behind wall (the IIR corrects
    // 1% per step, it must not snap) …
    REQUIRE(firstGapError == Approx(1.0).margin(0.02));
    // … and after n steps the residual is 0.99^n: 1500 steps => ~0.3ms.
    const double expectedResidual = std::pow(0.99, 1500);
    REQUIRE(rig.wall - rig.ts.now()
            == Approx(expectedResidual).margin(0.5e-3));
}

TEST_CASE("TimeSource: sustained rate mismatch settles at the P-controller "
          "steady-state error", "[TimeSource]") {
    // 10ms device callbacks whose sample accounting runs 10% fast against
    // wall time (e.g. World configured at one rate while the device delivers
    // another). A proportional corrector (gain 0.01/update) under a ramp
    // input has steady-state error
    //   e* = injectionPerUpdate / gain = (0.1 * 10ms) / 0.01 = 100ms.
    ClockRig rig;
    const double devDt      = 0.010;            // 10ms callbacks
    const double devSamples = kRate * devDt;    // nominal samples per callback

    double ntp = 0.0;
    for (int i = 0; i < 6000; ++i)              // 60s simulated — plateau
        ntp = rig.step(devSamples * 1.1, devDt);

    const double steadyStateError = ntp - rig.wall;  // clock AHEAD of wall
    REQUIRE(steadyStateError == Approx(0.100).margin(0.005));

    // The corrector cannot do better than this by design; correct sample
    // accounting across device swaps is what keeps the injection term at
    // zero. If this expectation fails because the error shrank, the clock
    // gained integral action and this pin needs updating.
}

TEST_CASE("TimeSource: re-anchor (resetAudioThreadTime) zeroes accumulated "
          "error immediately", "[TimeSource]") {
    ClockRig rig;
    for (int i = 0; i < 300; ++i) rig.step(kBlock * 1.1, kDt);  // build error
    REQUIRE(std::abs(rig.ts.now() - rig.wall) > 0.005);

    rig.ts.resetAudioThreadTime(rig.samples, kRate);
    const double ntp = rig.step();
    REQUIRE(ntp == Approx(rig.wall).margin(1e-4));
}

TEST_CASE("TimeSource: freewheel ignores the wall clock entirely",
          "[TimeSource]") {
    ClockRig rig;
    rig.ts.setFreewheelClock(true);
    const double anchor = rig.wall;  // base captured by the last reset

    for (int i = 0; i < 200; ++i) {
        // Wall clock goes haywire: jumps a full second per block.
        const double ntp = rig.step(kBlock, 1.0);
        REQUIRE(ntp == Approx(anchor + rig.samples / kRate).margin(1e-9));
    }
}

// ── Drift-log rate gate ──────────────────────────────────────────────────────
// Steady-state jitter on VMs and Windows shared-mode WASAPI hovers right at
// the drift threshold, so a once-per-second rate limit still logs forever
// (thousands of lines per session — sonic-pi #3553 logs, and any Windows
// session outside ASIO). The gate reports a genuine step immediately, then
// summarises a persisting condition at most once a minute, carrying the
// window's peak and hit count so one line tells the minute's story.

TEST_CASE("DriftLogGate: quiet clock never logs", "[DriftLog]") {
    supersonic::DriftLogGate gate;
    supersonic::DriftLogGate::Line line;
    for (int i = 0; i < 1000; ++i)
        REQUIRE_FALSE(gate.offer(0.004, 1000.0 + i * 0.02, line));
}

TEST_CASE("DriftLogGate: first excursion logs immediately", "[DriftLog]") {
    supersonic::DriftLogGate gate;
    supersonic::DriftLogGate::Line line;
    REQUIRE(gate.offer(0.0062, 5000.0, line));
    REQUIRE(line.hits == 1);
    REQUIRE(line.total == 1);
    REQUIRE(line.peakSec == 0.0062);
}

TEST_CASE("DriftLogGate: sustained jitter summarises once a minute",
          "[DriftLog]") {
    supersonic::DriftLogGate gate;
    supersonic::DriftLogGate::Line line;
    int lines = 0;
    // Two full minutes of 50Hz callbacks all hovering over threshold
    // (6001 ticks spans t=1000..1120 inclusive, crossing both minute marks).
    for (int i = 0; i < 6001; ++i) {
        const double now = 1000.0 + i * 0.02;
        const double drift = (i % 2 == 0) ? 0.006 : -0.008;
        if (gate.offer(drift, now, line))
            ++lines;
    }
    REQUIRE(lines == 3);              // immediate + one per following minute
    REQUIRE(line.peakSec == 0.008);   // window peak uses magnitude
    REQUIRE(line.hits > 2000);        // the whole window, not one callback
    REQUIRE(line.total == 6001);
}

TEST_CASE("DriftLogGate: a step long after the last report logs immediately",
          "[DriftLog]") {
    supersonic::DriftLogGate gate;
    supersonic::DriftLogGate::Line line;
    REQUIRE(gate.offer(0.010, 1000.0, line));
    // Quiet hour.
    for (int i = 0; i < 100; ++i)
        REQUIRE_FALSE(gate.offer(0.001, 1100.0 + i, line));
    // Fresh step: reported at once, window state freshly its own.
    REQUIRE(gate.offer(-0.020, 4600.0, line));
    REQUIRE(line.hits == 1);
    REQUIRE(line.peakSec == 0.020);
}

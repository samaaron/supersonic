//--
// This file is part of Sonic Pi: http://sonic-pi.net
// Full project source: https://github.com/samaaron/sonic-pi
// License: https://github.com/samaaron/sonic-pi/blob/main/LICENSE.md
//
// Copyright 2026 by Sam Aaron (http://sam.aaron.name).
// All rights reserved.
//
// Permission is granted for use, copying, modification, and
// distribution of modified versions of this work as long as this
// notice is included.
//++

// Pure unit tests for sonicpi::audio::LivenessMonitor — no engine, no CoreAudio,
// no real clock (times are plain integers in an arbitrary unit).

#include <catch2/catch_test_macros.hpp>
#include "AudioRecovery.h"

using sonicpi::audio::LivenessMonitor;
using sonicpi::audio::LivenessPhase;

// The core invariant: after a stall, a SINGLE resumed tick must NOT read as
// Live. Liveness requires ticks sustained across the confirm window — otherwise
// a device emitting one callback per reopen (the post-wake CoreAudio wedge)
// masquerades as recovered forever.
TEST_CASE("LivenessMonitor: a single twitch tick after a stall is not Live",
          "[AudioRecovery][Liveness]") {
    LivenessMonitor mon(/*stallWindow*/1000, /*confirmWindow*/500);

    // Boot: ticking normally -> Live immediately (no confirm needed at start).
    mon.observe(100, 0);
    REQUIRE(mon.phase(0) == LivenessPhase::Live);

    // Ticks freeze; past the stall window it must read Stalled.
    mon.observe(100, 1200);
    REQUIRE(mon.phase(1200) == LivenessPhase::Stalled);

    // One twitch tick, then frozen again — the reopen-produced single callback.
    mon.observe(101, 1300);
    CHECK(mon.phase(1300) != LivenessPhase::Live);   // Confirming, not Live
    CHECK(mon.phase(1300) == LivenessPhase::Confirming);

    mon.observe(101, 1400);                          // no further advance
    CHECK(mon.phase(1400) != LivenessPhase::Live);
}

// Sustained ticks across the confirm window DO restore Live.
TEST_CASE("LivenessMonitor: sustained ticks after a stall restore Live",
          "[AudioRecovery][Liveness]") {
    LivenessMonitor mon(/*stallWindow*/1000, /*confirmWindow*/500);

    mon.observe(100, 0);
    REQUIRE(mon.phase(0) == LivenessPhase::Live);

    mon.observe(100, 1200);                          // stall
    REQUIRE(mon.phase(1200) == LivenessPhase::Stalled);

    // Ticks resume and keep advancing past the confirm window.
    mon.observe(101, 1300);
    CHECK(mon.phase(1300) == LivenessPhase::Confirming);
    mon.observe(102, 1500);
    mon.observe(103, 1700);                          // 1700 - runStart(1300) = 400 < 500
    CHECK(mon.phase(1700) == LivenessPhase::Confirming);
    mon.observe(104, 1850);                          // 1850 - 1300 = 550 >= 500
    CHECK(mon.phase(1850) == LivenessPhase::Live);
}

// ── RateSkewMonitor ──────────────────────────────────────────────────────────
// Times in ms, rates in frames/ms (48 = 48kHz). Standard monitor: 1000ms
// windows, 300ms max observation gap, 5% tolerance, 2 consecutive bad windows.

using sonicpi::audio::RateSkewMonitor;

namespace {
RateSkewMonitor standardMonitor() {
    return RateSkewMonitor(/*window*/1000, /*maxGap*/300,
                           /*tolerance*/0.05, /*badWindowsRequired*/2);
}
} // namespace

TEST_CASE("RateSkewMonitor: a healthy device never reads skewed",
          "[AudioRecovery][RateSkew]") {
    auto mon = standardMonitor();
    // 48 frames/ms delivered exactly, sampled every 100ms for 10 windows.
    for (int64_t t = 0; t <= 10000; t += 100) {
        mon.observe(static_cast<uint64_t>(48 * t), 48.0, t);
        CHECK_FALSE(mon.skewed());
    }
}

TEST_CASE("RateSkewMonitor: jitter within tolerance never reads skewed",
          "[AudioRecovery][RateSkew]") {
    auto mon = standardMonitor();
    // ±3% alternating error — inside the 5% tolerance.
    uint64_t frames = 0;
    for (int64_t t = 100; t <= 10000; t += 100) {
        frames += (t / 100) % 2 == 0 ? 4944 : 4656;  // 48*100 ± 3%
        mon.observe(frames, 48.0, t);
        CHECK_FALSE(mon.skewed());
    }
}

// The incident shape: post-sleep DirectSound free-running slow (~0.3x). Two
// full windows of sustained skew — no less — produce the verdict.
TEST_CASE("RateSkewMonitor: sustained slow delivery reads skewed after exactly "
          "the required windows", "[AudioRecovery][RateSkew]") {
    auto mon = standardMonitor();
    for (int64_t t = 0; t <= 1900; t += 100) {
        mon.observe(static_cast<uint64_t>(15 * t), 48.0, t);  // 0.31x
        CHECK_FALSE(mon.skewed());   // first window bad, streak 1 — not enough
    }
    mon.observe(15 * 2000, 48.0, 2000);  // second bad window completes
    CHECK(mon.skewed());
    CHECK(mon.lastRatio() < 0.35);
}

// The pre-wake incident shape: timer free-running fast (~4.8x).
TEST_CASE("RateSkewMonitor: sustained fast delivery reads skewed",
          "[AudioRecovery][RateSkew]") {
    auto mon = standardMonitor();
    for (int64_t t = 0; t <= 2000; t += 100)
        mon.observe(static_cast<uint64_t>(230 * t), 48.0, t);  // 4.8x
    CHECK(mon.skewed());
    CHECK(mon.lastRatio() > 4.0);
}

// One transient stall skews one window; the next healthy window must clear the
// streak so a lone "[gap] audio callback stalled" can never cold-swap.
TEST_CASE("RateSkewMonitor: a single bad window resets on the next good one",
          "[AudioRecovery][RateSkew]") {
    auto mon = standardMonitor();
    mon.observe(0, 48.0, 0);                 // anchor: windows align to 1000s
    // Window [0,1000]: a 200ms stall inside it (frames flat) => 20% slow => bad.
    uint64_t frames = 0;
    for (int64_t t = 100; t <= 1000; t += 100) {
        if (t <= 800) frames += 4800;        // stall for the last 200ms
        mon.observe(frames, 48.0, t);
    }
    CHECK_FALSE(mon.skewed());
    // Window [1000,2000]: healthy — the streak must reset, not accumulate.
    for (int64_t t = 1100; t <= 2000; t += 100) {
        frames += 4800;
        mon.observe(frames, 48.0, t);
    }
    CHECK_FALSE(mon.skewed());
    // Window [2000,3000]: a later lone bad window still isn't enough.
    for (int64_t t = 2100; t <= 3000; t += 100) {
        frames += 2400;                      // 0.5x
        mon.observe(frames, 48.0, t);
    }
    CHECK_FALSE(mon.skewed());
}

// A sampling pause (benign skip, swap in flight, machine asleep) exceeds
// maxGap: the window spanning it is discarded rather than read as slowness.
TEST_CASE("RateSkewMonitor: an observation gap discards the window",
          "[AudioRecovery][RateSkew]") {
    auto mon = standardMonitor();
    for (int64_t t = 0; t <= 900; t += 100)
        mon.observe(static_cast<uint64_t>(48 * t), 48.0, t);
    // 5s sleep: frames frozen. Without the gap check this window would read
    // as ~0.15x and start a streak.
    mon.observe(48 * 900, 48.0, 5900);
    CHECK_FALSE(mon.skewed());
    // Healthy afterwards from the re-anchored window.
    for (int64_t t = 6000; t <= 8000; t += 100)
        mon.observe(static_cast<uint64_t>(48 * 900 + 48 * (t - 5900)), 48.0, t);
    CHECK_FALSE(mon.skewed());
}

// A frames rollback (device restart resets the sample counter) is a
// discontinuity: window AND streak restart, so a bad window either side of a
// restart can't combine into a verdict.
TEST_CASE("RateSkewMonitor: a frames rollback clears window and streak",
          "[AudioRecovery][RateSkew]") {
    auto mon = standardMonitor();
    // One bad window (0.5x): streak 1.
    for (int64_t t = 0; t <= 1000; t += 100)
        mon.observe(static_cast<uint64_t>(24 * t), 48.0, t);
    CHECK_FALSE(mon.skewed());
    // Device restart: frames restart near zero.
    mon.observe(100, 48.0, 1100);
    // Another lone bad window after the restart — streak restarted at 0, so
    // this reaches 1, not 2.
    for (int64_t t = 1200; t <= 2100; t += 100)
        mon.observe(static_cast<uint64_t>(100 + 24 * (t - 1100)), 48.0, t);
    CHECK_FALSE(mon.skewed());
}

// A nominal-rate change (cold swap to a new rate) re-anchors: frames delivered
// against the old rate must not be judged against the new one.
TEST_CASE("RateSkewMonitor: a nominal rate change re-anchors",
          "[AudioRecovery][RateSkew]") {
    auto mon = standardMonitor();
    for (int64_t t = 0; t <= 900; t += 100)
        mon.observe(static_cast<uint64_t>(48 * t), 48.0, t);
    // Rate changes mid-window (48k -> 44.1k device): discard, no verdict.
    for (int64_t t = 1000; t <= 3000; t += 100)
        mon.observe(static_cast<uint64_t>(48 * 900 + 44 * (t - 900)), 44.1, t);
    CHECK_FALSE(mon.skewed());
}

// After the verdict, a healthy window clears it (recovery worked), and
// reset() clears it immediately (recovery started).
TEST_CASE("RateSkewMonitor: skewed clears on a good window or reset",
          "[AudioRecovery][RateSkew]") {
    auto mon = standardMonitor();
    for (int64_t t = 0; t <= 2000; t += 100)
        mon.observe(static_cast<uint64_t>(15 * t), 48.0, t);
    REQUIRE(mon.skewed());

    SECTION("good window clears") {
        uint64_t frames = 15 * 2000;
        for (int64_t t = 2100; t <= 3000; t += 100) {
            frames += 4800;
            mon.observe(frames, 48.0, t);
        }
        CHECK_FALSE(mon.skewed());
    }
    SECTION("reset clears") {
        mon.reset();
        CHECK_FALSE(mon.skewed());
    }
}

/*
 * test_superclock.cpp — SuperClock state-machine tests.
 */
#include <catch2/catch_test_macros.hpp>
#include <cmath>

#include "src/SuperClock.h"
#include "src/native/WallClock.h"

TEST_CASE("SuperClock: default state on construction", "[SuperClock]") {
    SuperClock sc;
    CHECK(sc.getBpm() == 120.0);
    CHECK(sc.isPlaying() == false);
    CHECK(sc.isLinkEnabled() == false);
    CHECK(sc.numPeers() == 0);
}

TEST_CASE("SuperClock: setBpm round-trips", "[SuperClock]") {
    SuperClock sc;
    sc.setBpm(140.0, 0.0); CHECK(sc.getBpm() == 140.0);
    sc.setBpm(60.5,  0.0); CHECK(sc.getBpm() == 60.5);
    sc.setBpm(120.0, 0.0); CHECK(sc.getBpm() == 120.0);
}

TEST_CASE("SuperClock: setIsPlaying round-trips", "[SuperClock]") {
    SuperClock sc;
    CHECK(sc.isPlaying() == false);
    sc.setIsPlaying(true,  1234.5);
    CHECK(sc.isPlaying() == true);
    CHECK(sc.getIsPlayingAtNtp() == 1234.5);
    sc.setIsPlaying(false, 0.0);
    CHECK(sc.isPlaying() == false);
}

TEST_CASE("SuperClock: setLinkEnabled is a no-op without a Link backing",
          "[SuperClock]") {
    SuperClock sc;
    sc.setLinkEnabled(true);
    CHECK(sc.isLinkEnabled() == false);
    CHECK(sc.numPeers() == 0);
}

TEST_CASE("SuperClock: beatAtTime at non-integer-ratio BPM", "[SuperClock]") {
    SuperClock sc;
    sc.setBpm(137.0, 0.0);
    // beatAtTime(t) = t * 137/60 — pick non-trivial values.
    CHECK(std::abs(sc.beatAtTime(3.0, 4.0) - (3.0 * 137.0 / 60.0)) < 1e-12);
    CHECK(std::abs(sc.beatAtTime(1.5, 4.0) - (1.5 * 137.0 / 60.0)) < 1e-12);
    CHECK(std::abs(sc.beatAtTime(0.0, 4.0)) < 1e-12);
}

TEST_CASE("SuperClock: timeAtBeat is inverse of beatAtTime", "[SuperClock]") {
    SuperClock sc;
    sc.setBpm(140.0, 0.0);
    for (double b : {0.0, 0.5, 1.0, 4.0, 17.25}) {
        CHECK(std::abs(sc.beatAtTime(sc.timeAtBeat(b, 4.0), 4.0) - b) < 1e-12);
    }
}

TEST_CASE("SuperClock: phaseAtTime is non-negative and < quantum",
          "[SuperClock]") {
    SuperClock sc;
    sc.setBpm(120.0, 0.0);
    for (double t : {0.0, 0.5, 1.0, 2.5, 7.0}) {
        const double phase = sc.phaseAtTime(t, 4.0);
        CHECK(phase >= 0.0);
        CHECK(phase < 4.0);
    }
    // beatAtTime(-0.5) = -1.0 → phase = 3.0
    const double phaseNeg = sc.phaseAtTime(-0.5, 4.0);
    CHECK(phaseNeg >= 0.0);
    CHECK(phaseNeg < 4.0);
    CHECK(std::abs(phaseNeg - 3.0) < 1e-12);
}

TEST_CASE("SuperClock: requestBeatAtTime maps beat to time", "[SuperClock]") {
    SuperClock sc;
    sc.setBpm(120.0, 0.0);

    // beat 4 at time 2.0 with bpm 120 → beat_origin = 0
    sc.requestBeatAtTime(4.0, 2.0, 4.0);
    CHECK(std::abs(sc.beatAtTime(2.0, 4.0) - 4.0) < 1e-12);

    // beat 0 at time 10.0 → beat_origin moves to 10.0
    sc.requestBeatAtTime(0.0, 10.0, 4.0);
    CHECK(std::abs(sc.beatAtTime(10.0, 4.0)) < 1e-12);
    CHECK(std::abs(sc.beatAtTime(10.5, 4.0) - 1.0) < 1e-12);
}

TEST_CASE("SuperClock: forceBeatAtTime == requestBeatAtTime in session-of-one",
          "[SuperClock]") {
    SuperClock scA, scB;
    scA.setBpm(140.0, 0.0);
    scB.setBpm(140.0, 0.0);
    scA.requestBeatAtTime(8.0, 5.0, 4.0);
    scB.forceBeatAtTime(8.0, 5.0, 4.0);
    CHECK(scA.beatAtTime(7.0, 4.0)  == scB.beatAtTime(7.0, 4.0));
    CHECK(scA.beatAtTime(10.0, 4.0) == scB.beatAtTime(10.0, 4.0));
}

// ── Audio-thread time source ─────────────────────────────────────────────

TEST_CASE("SuperClock: now() returns sensible NTP time", "[SuperClock]") {
    SuperClock sc;
    CHECK(sc.now() > 3.9e9);  // NTP epoch is 1900; today is well past 2024
}

TEST_CASE("SuperClock: wallNow tracks wallClockNTP within tight bound",
          "[SuperClock]") {
    SuperClock sc;
    const double direct = wallClockNTP();
    const double via_sc = sc.wallNow();
    CHECK(std::abs(via_sc - direct) < 0.01);
}

TEST_CASE("SuperClock: updateAudioThreadNTP publishes the returned value",
          "[SuperClock]") {
    SuperClock sc;
    sc.resetAudioThreadTime(0.0, 48000.0);
    const double returned = sc.updateAudioThreadNTP(128.0, 48000.0);
    CHECK(sc.now() == returned);
}

TEST_CASE("SuperClock: resetAudioThreadTime publishes a usable NTP immediately",
          "[SuperClock]") {
    SuperClock sc;
    sc.resetAudioThreadTime(0.0, 48000.0);
    CHECK(std::abs(sc.now() - wallClockNTP()) < 0.01);
}

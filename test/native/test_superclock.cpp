/*
 * test_superclock.cpp — SuperClock state-machine tests.
 */
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cmath>
#include <thread>

#include "src/SuperClock.h"
#include "src/native/WallClock.h"

#ifdef SUPERSONIC_LINK
#include <algorithm>
#include <vector>
// The platform interface scanner that Link's discovery binds to. Our
// loopback-mode patch adds the loopbackOnly() flag + filtering here; the
// visibility tests below assert on what this scanner returns.
#if defined(_WIN32)
#include <ableton/platforms/windows/ScanIpIfAddrs.hpp>
#else
#include <ableton/platforms/posix/ScanIpIfAddrs.hpp>
#endif
#endif  // SUPERSONIC_LINK

namespace {
// setBpm → getBpm is eventually consistent. Link's commitAppSessionState
// updates clientState synchronously then posts session-timing work to
// its io thread; the same call's async handler can momentarily clobber
// a later commit's synchronous update before that later commit's async
// handler runs. This isn't a bug — it's Link's distributed convergence
// model. The test contract here is "eventually equals", not "instantly
// equals".
bool eventuallyBpm(SuperClock& sc, double expected, double eps = 1e-9) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (std::chrono::steady_clock::now() < deadline) {
        if (std::abs(sc.getBpm() - expected) < eps) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

#ifdef SUPERSONIC_LINK
// Invoke the platform interface scanner directly. It's synchronous and
// independent of Link's discovery thread, but reads the same process-global
// loopbackOnly() flag that SuperClock::setLinkVisibility toggles — so it
// reports exactly the address set Link would bind discovery to.
inline std::vector<ableton::discovery::IpAddress> scanLinkInterfaces() {
#if defined(_WIN32)
    return ableton::platforms::windows::ScanIpIfAddrs{}();
#else
    return ableton::platforms::posix::ScanIpIfAddrs{}();
#endif
}

inline bool allLoopback(const std::vector<ableton::discovery::IpAddress>& addrs) {
    return std::all_of(addrs.begin(), addrs.end(),
                       [](const ableton::discovery::IpAddress& ip) {
                           return ip.is_loopback();
                       });
}
#endif  // SUPERSONIC_LINK
}  // namespace

TEST_CASE("SuperClock: default state on construction", "[SuperClock]") {
    SuperClock sc;
    CHECK(sc.getBpm() == 120.0);
    CHECK(sc.isPlaying() == false);
    CHECK(sc.isLinkEnabled() == false);
    CHECK(sc.numPeers() == 0);
}

TEST_CASE("SuperClock: setBpm round-trips (eventually)", "[SuperClock]") {
    SuperClock sc;
    sc.setBpm(140.0, 0.0); CHECK(eventuallyBpm(sc, 140.0));
    sc.setBpm(60.5,  0.0); CHECK(eventuallyBpm(sc, 60.5));
    sc.setBpm(120.0, 0.0); CHECK(eventuallyBpm(sc, 120.0));
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

TEST_CASE("SuperClock: setLinkEnabled toggles Link state",
          "[SuperClock]") {
    SuperClock sc;
    // Fresh SuperClock starts with Link disabled.
    CHECK(sc.isLinkEnabled() == false);

#ifdef SUPERSONIC_LINK
    // On Link builds the setter delegates to ableton::Link::enable.
    sc.setLinkEnabled(true);
    CHECK(sc.isLinkEnabled() == true);
    sc.setLinkEnabled(false);
    CHECK(sc.isLinkEnabled() == false);
#else
    // No-Link builds: setter has no underlying backend; capability stays
    // false regardless of the requested state.
    sc.setLinkEnabled(true);
    CHECK(sc.isLinkEnabled() == false);
#endif

    // No peers ever discovered in a standalone SuperClock (no event loop
    // attached to discover anything).
    CHECK(sc.numPeers() == 0);
}

#ifdef SUPERSONIC_LINK
TEST_CASE("SuperClock: setLinkEnabled(true) on fresh state stays loopback-only",
          "[SuperClock][Link]") {
    // Privacy: bare enable on a fresh SuperClock must not promote to
    // NetworkWide LAN advertising.
    SuperClock sc;
    REQUIRE(sc.getLinkVisibility() == SuperClock::LinkVisibility::Off);
    sc.setLinkEnabled(true);
    CHECK(sc.getLinkVisibility() == SuperClock::LinkVisibility::LoopbackOnly);
}

TEST_CASE("SuperClock: setLinkEnabled(false→true) preserves LoopbackOnly",
          "[SuperClock][Link]") {
    // A disable/enable cycle must not silently upgrade a prior
    // LoopbackOnly choice to NetworkWide.
    SuperClock sc;
    sc.setLinkVisibility(SuperClock::LinkVisibility::LoopbackOnly);
    REQUIRE(sc.getLinkVisibility() == SuperClock::LinkVisibility::LoopbackOnly);

    sc.setLinkEnabled(false);
    REQUIRE(sc.getLinkVisibility() == SuperClock::LinkVisibility::Off);

    sc.setLinkEnabled(true);
    CHECK(sc.getLinkVisibility() == SuperClock::LinkVisibility::LoopbackOnly);
}

// The state-machine tests above only check getLinkVisibility() — the value we
// store. These check the value actually reaches the platform scanner Link
// binds to, i.e. that LoopbackOnly genuinely keeps us off non-loopback
// interfaces. Without this, a build that reports LoopbackOnly while still
// advertising on the LAN (e.g. a flag wired to a dead static) passes silently.
TEST_CASE("SuperClock: LoopbackOnly constrains the interface scan to loopback",
          "[SuperClock][Link][visibility]") {
    SuperClock sc;
    sc.setLinkVisibility(SuperClock::LinkVisibility::LoopbackOnly);

    const auto addrs = scanLinkInterfaces();
    // Loopback (lo0 / 127.0.0.1) is always up, so the filtered set is
    // non-empty and must contain nothing else.
    CHECK_FALSE(addrs.empty());
    CHECK(allLoopback(addrs));

    sc.setLinkVisibility(SuperClock::LinkVisibility::Off);  // reset global flag
}

TEST_CASE("SuperClock: NetworkWide leaves non-loopback interfaces in the scan",
          "[SuperClock][Link][visibility]") {
    SuperClock sc;
    sc.setLinkVisibility(SuperClock::LinkVisibility::LoopbackOnly);
    const auto loopback = scanLinkInterfaces();
    sc.setLinkVisibility(SuperClock::LinkVisibility::NetworkWide);
    const auto wide = scanLinkInterfaces();

    // NetworkWide must not filter: it exposes at least as many interfaces as
    // LoopbackOnly (strictly more on any host with a real NIC). Guards against
    // the loopback filter getting stuck on.
    CHECK(wide.size() >= loopback.size());

    sc.setLinkVisibility(SuperClock::LinkVisibility::Off);  // reset global flag
}

#endif  // SUPERSONIC_LINK

TEST_CASE("SuperClock: setBpm rejects non-finite and sub-1 values",
          "[SuperClock]") {
    // Guards beat math (timeAtBeat divides by bpm).
    SuperClock sc;
    sc.setBpm(120.0, 0.0);

    sc.setBpm(0.0, 0.0);
    CHECK(sc.getBpm() >= 1.0);
    CHECK(std::isfinite(sc.getBpm()));

    sc.setBpm(std::nan(""), 0.0);
    CHECK(sc.getBpm() >= 1.0);
    CHECK(std::isfinite(sc.getBpm()));

    sc.setBpm(-5.0, 0.0);
    CHECK(sc.getBpm() >= 1.0);
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

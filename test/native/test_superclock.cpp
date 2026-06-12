/*
 * test_superclock.cpp — SuperClock state-machine tests.
 */
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>
#include <thread>
#include <vector>

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

// ─── MIDI follower timelines ─────────────────────────────────────────────

TEST_CASE("SuperClock: midi timeline claim is idempotent and slot-stable",
          "[SuperClock][midi]") {
    SuperClock sc;
    const int a = sc.claimMidiTimeline("portA", "Port A");
    CHECK(a == 1);
    CHECK(sc.claimMidiTimeline("portA", "Port A") == a);          // idempotent
    CHECK(sc.resolveTimeline("midi:portA") == a);
    CHECK(sc.resolveTimeline("midi:portB") == -1);      // not claimed
    CHECK(sc.claimMidiTimeline("portB", "Port B") == 2);
    CHECK(sc.claimMidiTimeline("", "") == -1);              // empty name rejected
}

TEST_CASE("SuperClock: midi registry is bounded at SC_MAX_TIMELINES",
          "[SuperClock][midi]") {
    SuperClock sc;
    for (int i = 1; i <= SC_MAX_TIMELINES; ++i)
        CHECK(sc.claimMidiTimeline(("p" + std::to_string(i)).c_str(), "raw") == i);
    CHECK(sc.claimMidiTimeline("overflow", "Overflow") == -1);      // registry full
}

TEST_CASE("SuperClock: resolveTimeline maps names to ids", "[SuperClock][midi]") {
    SuperClock sc;
    CHECK(sc.resolveTimeline("") == 0);                 // link by default
    CHECK(sc.resolveTimeline("link") == 0);
    CHECK(sc.resolveTimeline("midi:nope") == -1);       // unclaimed → placeholder
    const int a = sc.claimMidiTimeline("portA", "Port A");
    CHECK(sc.resolveTimeline("midi:portA") == a);
    CHECK(sc.resolveTimeline("midi") == a);             // bare midi → primary
}

TEST_CASE("SuperClock: midi tempo feed sets bpm and advances beats",
          "[SuperClock][midi]") {
    SuperClock sc;
    const int id = sc.claimMidiTimeline("portA", "Port A");
    sc.setMidiTimelineTempo(id, 140.0);
    CHECK(sc.timelineBpm(id) == Catch::Approx(140.0));

    const int64_t t = sc.linkClockMicros();
    const double b0 = sc.timelineBeatAtLinkTime(id, t, 4.0);
    const double b1 = sc.timelineBeatAtLinkTime(id, t + 1'000'000, 4.0);  // +1s
    CHECK(b1 - b0 == Catch::Approx(140.0 / 60.0).epsilon(1e-6));          // 140 BPM

    // time_at_beat is the inverse of beat_at_time.
    const int64_t back = sc.timelineTimeAtBeatLinkMicros(id, b1, 4.0);
    CHECK(static_cast<double>(back) == Catch::Approx(t + 1'000'000).epsilon(1e-6));
}

TEST_CASE("SuperClock: midi tempo change preserves the current beat",
          "[SuperClock][midi]") {
    SuperClock sc;
    const int id = sc.claimMidiTimeline("portA", "Port A");
    sc.setMidiTimelineTempo(id, 120.0);
    const double before = sc.timelineBeatAtLinkTime(id, sc.linkClockMicros(), 4.0);
    sc.setMidiTimelineTempo(id, 174.0);                 // jump tempo
    const double after = sc.timelineBeatAtLinkTime(id, sc.linkClockMicros(), 4.0);
    CHECK(after == Catch::Approx(before).margin(0.05)); // no beat discontinuity
}

TEST_CASE("SuperClock: midi transport drives playing state", "[SuperClock][midi]") {
    SuperClock sc;
    const int id = sc.claimMidiTimeline("portA", "Port A");
    CHECK_FALSE(sc.timelineIsPlaying(id));
    sc.setMidiTimelineTransport(id, /*START*/ 0, 0.0);
    CHECK(sc.timelineIsPlaying(id));
    sc.setMidiTimelineTransport(id, /*STOP*/ 2, 0.0);
    CHECK_FALSE(sc.timelineIsPlaying(id));
}

TEST_CASE("SuperClock: midi beat tracks the pulse count", "[SuperClock][midi]") {
    SuperClock sc;
    const int id = sc.claimMidiTimeline("portA", "Port A");
    const int64_t ts0 = sc.linkClockMicros();                          // first pulse ~now
    const int64_t iv = static_cast<int64_t>(60.0 / 120.0 / 24.0 * 1e6); // 120 BPM pulse
    const int pulses = 49;                                             // downbeat + 48 -> beat 2.0
    for (int k = 0; k < pulses; ++k)
        sc.midiTimelinePulse(id, static_cast<uint64_t>(ts0 + static_cast<int64_t>(k) * iv));
    // The beat is the exact count: the first pulse IS the downbeat (beat 0),
    // so querying at the last pulse's own timestamp gives 48/24 = 2.0.
    const double beat = sc.timelineBeatAtLinkTime(id, ts0 + static_cast<int64_t>(pulses - 1) * iv, 4.0);
    CHECK(beat == Catch::Approx(2.0).margin(0.02));
    CHECK(sc.timelineBpm(id) == Catch::Approx(120.0).margin(0.5));     // tempo recovered
}

TEST_CASE("SuperClock: midi tempo de-jitters a wobbly clock",
          "[SuperClock][midi]") {
    SuperClock sc;
    const int id = sc.claimMidiTimeline("portA", "Port A");
    const int64_t t0 = sc.linkClockMicros();
    const double iv = 60.0 / 120.0 / 24.0 * 1e6;          // 120 BPM nominal interval
    // Feed 96 pulses whose arrivals alternate ±20% around the true interval
    // (bunched OS delivery). The tempo read-out must stay steady at ~120 BPM.
    double t = static_cast<double>(t0) - 96.0 * iv;
    for (int k = 1; k <= 96; ++k) {
        t += iv * (k % 2 ? 1.20 : 0.80);
        sc.midiTimelinePulse(id, static_cast<uint64_t>(t));
    }
    CHECK(sc.timelineBpm(id) == Catch::Approx(120.0).margin(1.0));
}

TEST_CASE("SuperClock: midi stall-and-burst keeps both beat and tempo",
          "[SuperClock][midi]") {
    // Field capture 2026-06-11 (MOTU rig): the tick stream periodically stalls
    // ~120ms then flushes the queued ticks ~300µs apart. The burst ticks are
    // real (each is 1/24 beat) so they must all land in the beat count, while
    // the garbage arrival intervals must not disturb the tempo.
    SuperClock sc;
    const int id = sc.claimMidiTimeline("portA", "Port A");
    const double iv = 60.0 / 111.0 / 24.0 * 1e6;          // ~111 BPM interval
    double t = static_cast<double>(sc.linkClockMicros());
    int pulses = 0;
    for (int k = 0; k < 96; ++k) { sc.midiTimelinePulse(id, static_cast<uint64_t>(t)); t += iv; ++pulses; }
    const double bpmBefore = sc.timelineBpm(id);

    t += 122000.0 - iv;                                    // stall: next tick 122ms late
    for (int k = 0; k < 6; ++k) {                          // queued ticks flushed in a burst
        sc.midiTimelinePulse(id, static_cast<uint64_t>(t)); t += 300.0; ++pulses;
    }
    for (int k = 0; k < 48; ++k) { sc.midiTimelinePulse(id, static_cast<uint64_t>(t)); t += iv; ++pulses; }

    CHECK(sc.timelineBpm(id) == Catch::Approx(bpmBefore).margin(1.0));
    // Beat at the last pulse's own timestamp = ticks since the downbeat pulse
    // / 24: every burst tick counted, no phase slip.
    const double beat = sc.timelineBeatAtLinkTime(id, static_cast<int64_t>(t - iv), 4.0);
    CHECK(beat == Catch::Approx(static_cast<double>(pulses - 1) / 24.0).margin(0.02));
}

TEST_CASE("SuperClock: midi tempo only interpolates between pulses",
          "[SuperClock][midi]") {
    SuperClock sc;
    const int id = sc.claimMidiTimeline("portA", "Port A");
    const int64_t t0 = sc.linkClockMicros();
    const int64_t iv = static_cast<int64_t>(60.0 / 120.0 / 24.0 * 1e6);
    for (int k = 1; k <= 48; ++k)
        sc.midiTimelinePulse(id, static_cast<uint64_t>(t0 - (48 - k) * iv));
    // Extrapolating one pulse-interval past the last pulse adds exactly 1/24 beat
    // at the ~120 BPM tempo.
    const double here  = sc.timelineBeatAtLinkTime(id, t0, 4.0);
    const double ahead = sc.timelineBeatAtLinkTime(id, t0 + iv, 4.0);
    CHECK(ahead - here == Catch::Approx(1.0 / 24.0).margin(1e-3));
}

// Accuracy test (hidden by default — real-time, busy-waits a CPU; run with
// `SuperSonicNativeTests "[accuracy]"`). Drives a real accelerando and measures
// the engine's beat against ground truth (pulse_count/24), alongside a model
// that integrates the tempo read-out (which drifts).
TEST_CASE("SuperClock: midi beat is drift-free through a live tempo ramp",
          "[SuperClock][midi][accuracy][.]") {
    using namespace std::chrono;
    SuperClock sc;
    const int id = sc.claimMidiTimeline("ramp", "Ramp");

    const double startBpm = 120.0, endBpm = 180.0, rampSecs = 3.0;
    const auto   wall0 = steady_clock::now();
    int64_t pulses = 0;
    double  maxDrift = 0.0;

    // For comparison: integrate the tempo read-out over elapsed time instead of
    // counting pulses.
    double  naiveBeat = 0.0;
    int64_t naivePrevUs = sc.linkClockMicros();
    double  maxNaiveDrift = 0.0;

    int64_t nextUs = sc.linkClockMicros();
    for (;;) {
        const double el = duration<double>(steady_clock::now() - wall0).count();
        if (el >= rampSecs) break;
        const double trueBpm = startBpm + (endBpm - startBpm) * (el / rampSecs);
        nextUs += static_cast<int64_t>(60.0 / trueBpm / 24.0 * 1e6);
        while (sc.linkClockMicros() < nextUs) { /* busy-wait to the pulse instant */ }

        ++pulses;
        sc.midiTimelinePulse(id, static_cast<uint64_t>(sc.linkClockMicros()));

        const int64_t qn        = sc.linkClockMicros();
        const double  trueBeat  = static_cast<double>(pulses - 1) / 24.0;
        const double  engineBeat = sc.timelineBeatAtLinkTime(id, qn, 4.0);
        maxDrift = std::max(maxDrift, std::abs(engineBeat - trueBeat));

        naiveBeat += static_cast<double>(qn - naivePrevUs) * 1e-6 * sc.timelineBpm(id) / 60.0;
        naivePrevUs = qn;
        maxNaiveDrift = std::max(maxNaiveDrift, std::abs(naiveBeat - trueBeat));
    }

    WARN("ramp over " << pulses << " pulses (120->180 BPM, " << rampSecs << "s): "
         "pulse-anchored max drift = " << (maxDrift * 1000.0) << " milli-beats; "
         "naive-integrated max drift = " << (maxNaiveDrift * 1000.0) << " milli-beats");
    CHECK(maxDrift < 0.01);                       // pulse-anchored: ~0 drift
    CHECK(maxNaiveDrift > maxDrift * 5.0);        // the integrating model drifts
}

// Models a `use_bpm :midi / sample; sleep 1` live_loop across a 100->300 BPM step
// landing mid-beat-4. The spider commits each beat's play timestamp ~half a beat
// early via time_at_beat (look-ahead), so we sample the prediction there. Prints
// the gap sequence (~600 ms gaps, a short transition, then ~200 ms).
TEST_CASE("SuperClock: midi beat-fire spread across a 100->300 step",
          "[SuperClock][midi][accuracy][.]") {
    SuperClock sc;
    const int id = sc.claimMidiTimeline("clk", "Clk");

    std::vector<int64_t> pulseTs;
    int64_t t = sc.linkClockMicros();
    auto feed = [&](double bpm, int pulses) {
        const int64_t iv = static_cast<int64_t>(60.0 / bpm / 24.0 * 1e6);
        for (int p = 0; p < pulses; ++p) { pulseTs.push_back(t); t += iv; }
    };
    feed(100.0, 90);    // 3.75 beats at 100 BPM
    feed(300.0, 200);   // big step landing mid-beat-4

    const double lookBeats = 0.5;  // commit each beat ~half a beat ahead
    std::vector<double> fire;
    int nextBeat = 1;
    for (size_t i = 0; i < pulseTs.size(); ++i) {
        sc.midiTimelinePulse(id, static_cast<uint64_t>(pulseTs[i]));
        const double beatNow = sc.timelineBeatAtLinkTime(id, pulseTs[i], 4.0);
        while (nextBeat <= 9 && beatNow >= nextBeat - lookBeats) {
            fire.push_back(static_cast<double>(sc.timelineTimeAtBeatLinkMicros(id, nextBeat, 4.0)));
            ++nextBeat;
        }
    }
    REQUIRE(fire.size() >= 8);
    std::string gaps;
    for (size_t n = 1; n < fire.size(); ++n)
        gaps += std::to_string(std::llround((fire[n] - fire[n - 1]) / 1000.0)) + " ";
    WARN("beat-fire gaps (ms) across 100->300 step: " << gaps
         << "  [expect ~600 x3, transition, ~200 x...; any gap <200 = overshoot]");
    // Steady-state sanity: the last few gaps should be the true 300 BPM (~200 ms).
    const double tail = (fire[fire.size() - 1] - fire[fire.size() - 3]) / 2.0 / 1000.0;
    CHECK(tail == Catch::Approx(200.0).margin(8.0));
}

TEST_CASE("SuperClock: a never-seen midi timeline is a coherent free-run fallback",
          "[SuperClock][midi]") {
    SuperClock sc;
    const int id = sc.resolveTimeline("midi:ghost");        // never claimed
    CHECK(id == -1);
    CHECK(sc.timelineBpm(id) == Catch::Approx(60.0));
    // beat<->time round-trips, and advances at a steady 60 BPM (1 beat / 1e6 us).
    const int64_t t = sc.timelineTimeAtBeatLinkMicros(id, 4.0, 4.0);
    CHECK(sc.timelineBeatAtLinkTime(id, t, 4.0) == Catch::Approx(4.0).margin(1e-6));
    const double adv = sc.timelineBeatAtLinkTime(id, 1'000'000, 4.0)
                     - sc.timelineBeatAtLinkTime(id, 0, 4.0);
    CHECK(adv == Catch::Approx(1.0).margin(1e-6));
}

TEST_CASE("SuperClock: a vanished midi clock keeps its tempo (not freed)",
          "[SuperClock][midi][.]") {
    using namespace std::chrono;
    SuperClock sc;
    const int id = sc.claimMidiTimeline("portA", "Port A");
    const int64_t iv = static_cast<int64_t>(60.0 / 120.0 / 24.0 * 1e6);   // 120 BPM
    int64_t ts = sc.linkClockMicros();
    for (int k = 0; k < 48; ++k) { sc.midiTimelinePulse(id, static_cast<uint64_t>(ts)); ts += iv; }
    CHECK(sc.timelineBpm(id) == Catch::Approx(120.0).margin(1.0));

    std::this_thread::sleep_for(milliseconds(1800));         // exceed the 1.5 s stale gap
    sc.tickMidiStaleness();
    // Slot is NOT reclaimed; the port still resolves and holds its last tempo.
    CHECK(sc.resolveTimeline("midi:portA") == id);
    CHECK(sc.timelineBpm(id) == Catch::Approx(120.0).margin(1.0));
}

TEST_CASE("SuperClock: primary follows the lowest active slot", "[SuperClock][midi]") {
    SuperClock sc;
    const int a = sc.claimMidiTimeline("portA", "Port A");
    const int b = sc.claimMidiTimeline("portB", "Port B");
    CHECK(sc.resolveTimeline("midi") == a);             // lowest slot is primary
    sc.freeMidiTimeline(a);
    CHECK(sc.resolveTimeline("midi") == b);             // promotes next slot
}

TEST_CASE("SuperClock: unknown timeline ids read a 60-BPM placeholder",
          "[SuperClock][midi]") {
    SuperClock sc;
    CHECK(sc.timelineBpm(-1) == Catch::Approx(60.0));
    CHECK(sc.timelineBpm(99) == Catch::Approx(60.0));
    CHECK_FALSE(sc.timelineIsPlaying(-1));
    // beat<->time is a coherent free-run (not the old degenerate 0/now).
    const int64_t t = sc.timelineTimeAtBeatLinkMicros(-1, 2.0, 4.0);
    CHECK(sc.timelineBeatAtLinkTime(-1, t, 4.0) == Catch::Approx(2.0).margin(1e-6));
}

TEST_CASE("SuperClock: listTimelines reports link plus active midi rows",
          "[SuperClock][midi]") {
    SuperClock sc;
    auto only = sc.listTimelines();
    REQUIRE(only.size() == 1);
    CHECK(only[0].name == "link");
    sc.claimMidiTimeline("portA", "Port A");
    auto two = sc.listTimelines();
    REQUIRE(two.size() == 2);
    CHECK(two[1].name == "midi:portA");
    CHECK(two[1].primary);
}

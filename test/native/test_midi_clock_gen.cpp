/*
 * test_midi_clock_gen.cpp — the pure 24-PPQN clock generator in isolation:
 * pulses land on 1/24-beat boundaries, align to the next boundary on start, are
 * emitted exactly once, and stop cleanly. No engine, no SuperClock, no IO.
 */
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "scheduler/MidiClockGen.h"

#include <vector>

using Catch::Matchers::WithinAbs;

namespace {
std::vector<double> drain(MidiClockGenerator& g, double horizon) {
    std::vector<double> out;
    g.collect(horizon, [&](double b) { out.push_back(b); });
    return out;
}
} // namespace

TEST_CASE("clock gen emits 24 pulses per beat on 1/24 boundaries", "[midi_clock]") {
    MidiClockGenerator g;
    REQUIRE(drain(g, 1.0).empty());           // stopped → nothing

    g.start(0.0);
    REQUIRE(g.running());

    // Just shy of beat 1 → exactly 24 pulses (0/24 .. 23/24).
    auto p = drain(g, 0.999);
    REQUIRE(p.size() == 24);
    CHECK_THAT(p.front(), WithinAbs(0.0, 1e-9));
    CHECK_THAT(p.back(), WithinAbs(23.0 / 24.0, 1e-9));

    // Continuing does not repeat already-emitted pulses.
    auto p2 = drain(g, 2.0);
    CHECK_THAT(p2.front(), WithinAbs(24.0 / 24.0, 1e-9));
    for (double b : p2) CHECK(b >= 1.0);
}

TEST_CASE("clock gen aligns to the next 1/24 boundary on a mid-beat start", "[midi_clock]") {
    MidiClockGenerator g;
    g.start(1.5);                             // mid-beat
    auto p = drain(g, 1.6);
    REQUIRE_FALSE(p.empty());
    // first pulse is the next 1/24 boundary at/after 1.5 = 36/24
    CHECK_THAT(p.front(), WithinAbs(36.0 / 24.0, 1e-9));
}

TEST_CASE("clock gen emits each pulse exactly once across successive horizons", "[midi_clock]") {
    MidiClockGenerator g;
    g.start(0.0);
    std::vector<double> all;
    for (int k = 1; k <= 20; ++k) {           // horizons 0.1 .. 2.0
        g.collect(k * 0.1, [&](double b) { all.push_back(b); });
    }
    // Pulses 0/24 .. 48/24 (beat 2.0 inclusive) = 49, strictly increasing
    // (no duplicates across the successive-horizon boundary, no gaps).
    REQUIRE(all.size() == 49);
    for (size_t i = 1; i < all.size(); ++i) CHECK(all[i] > all[i - 1]);
    CHECK_THAT(all.front(), WithinAbs(0.0, 1e-9));
    CHECK_THAT(all.back(), WithinAbs(48.0 / 24.0, 1e-9));
}

TEST_CASE("clock gen yields nothing when stopped or before its first pulse", "[midi_clock]") {
    MidiClockGenerator g;
    g.start(5.0);
    CHECK(drain(g, 4.9).empty());             // horizon behind the start beat
    g.stop();
    CHECK_FALSE(g.running());
    CHECK(drain(g, 100.0).empty());           // stopped → nothing even far ahead
}

TEST_CASE("clock gen nextPulseBeat tracks the look-ahead frontier", "[midi_clock]") {
    MidiClockGenerator g;
    g.start(0.0);
    drain(g, 0.999);                          // emit 0/24 .. 23/24
    CHECK_THAT(g.nextPulseBeat(), WithinAbs(24.0 / 24.0, 1e-9));
}

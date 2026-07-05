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

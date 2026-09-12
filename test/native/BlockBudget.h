// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Sam Aaron
// The block-budget assertion the realtime tests share.
//
// A block's wall-clock time on a shared CI runner includes whatever slice the
// OS took away in the middle of it, and one stolen slice looks exactly like
// a carried read. A read the audio thread really carried happens on every
// run; a stolen slice does not. So a budget scenario gets up to three fresh
// runs and passes on the first that stays within the bar, and a failure
// reports the longest block of every run.
#pragma once

#include <catch2/catch_test_macros.hpp>
#include <string>

namespace block_budget {

// One 128-frame block at 48 kHz.
constexpr double kBlockMs = 128.0 / 48000.0 * 1000.0;

// `scenario()` builds a fresh engine, runs the case and returns the longest
// block it rendered, in milliseconds; every other assertion the case makes
// lives inside it and must hold on every run.
template <typename Scenario>
void requireWithin(double barMs, Scenario&& scenario) {
    std::string seen;
    for (int attempt = 1; attempt <= 3; ++attempt) {
        const double maxBlockMs = scenario();
        if (attempt > 1) seen += ", ";
        seen += std::to_string(maxBlockMs) + " ms";
        if (maxBlockMs < barMs) return;
    }
    INFO("longest block per run: " << seen << "; bar: " << barMs << " ms");
    CHECK(false);
}

} // namespace block_budget

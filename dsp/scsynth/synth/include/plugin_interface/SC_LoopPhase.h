// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Sam Aaron

#pragma once

#include "SC_Unit.h"

#include <cmath>

// A buffer phase wrapped into [0, hi). The phase comes from an input, so it
// can be any double: the remainder is fmod's, which is exact, where
// in - hi * floor(in / hi) loses it to rounding for a huge phase. NaN stays
// NaN, and reads as index 0.
inline double sc_gloop(double in, double hi) {
    // avoid the divide if possible
    if (in >= hi) {
        in -= hi;
        if (in < hi)
            return in;
    } else if (in < 0.) {
        in += hi;
        if (in >= 0.)
            return in;
    } else
        return in;

    double r = std::fmod(in, hi);
    if (r < 0.)
        r += hi;
    return r < hi ? r : 0.;
}

// The same, or with looping off, held at either end and the unit marked done.
inline double sc_loop(Unit* unit, double in, double hi, int loop) {
    if (!loop && (in >= hi || in < 0.)) {
        unit->mDone = true;
        return in >= hi ? hi : 0.;
    }
    return sc_gloop(in, hi);
}

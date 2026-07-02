/*
 * test_sc_calc.cpp — exercise the single-precision DSP path on the host.
 *
 * The host build is desktop (sc_calc_t == double), so on its own it never
 * compiles the float branch of sc_calc_t / sc_calc_guard. SC_CalcType.inc is
 * re-includable and pulls in no platform headers, so we re-include it here
 * forced to single precision, in its own namespace, and test the float-only
 * behaviour — the divergence guard the embedded (ESP32) build relies on.
 */
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <limits>

#include "SC_Platform.h" // host build: sc_calc_t = double, sc_calc_guard = identity

// The host (desktop) build must be full double precision with the guard a pure
// pass-through, i.e. hosted DSP behaviour unchanged by the sc_calc_t conversion.
static_assert(SC_HAS_HW_FLOAT64 == 1, "host build has a hardware double FPU");
static_assert(sizeof(sc_calc_t) == sizeof(double), "host sc_calc_t is double");

// Re-include the calc-type table forced to single precision, in its own namespace,
// so the float-only guard branch is compiled and testable on the host.
namespace flt {
#undef SC_HAS_HW_FLOAT64
#define SC_HAS_HW_FLOAT64 0
#include "SC_CalcType.inc"
#undef SC_HAS_HW_FLOAT64
} // namespace flt
static_assert(sizeof(flt::sc_calc_t) == sizeof(float), "forced-float sc_calc_t is float");

TEST_CASE("sc_calc_guard: host double path is a pass-through", "[sc_calc]") {
    // double has the headroom, so nothing is clamped on the host.
    CHECK(sc_calc_guard(0.5) == 0.5);
    CHECK(sc_calc_guard(1.0e30) == 1.0e30);
    CHECK(std::isinf(sc_calc_guard(std::numeric_limits<double>::infinity())));
}

TEST_CASE("sc_calc_guard: float path resets non-finite / runaway state", "[sc_calc]") {
    using F = flt::sc_calc_t;
    // normal values pass through unchanged
    CHECK(flt::sc_calc_guard(F(0.5f)) == 0.5f);
    CHECK(flt::sc_calc_guard(F(-12345.0f)) == -12345.0f);
    // non-finite and finite-runaway reset to 0 (recover the recursion)
    CHECK(flt::sc_calc_guard(std::numeric_limits<F>::infinity()) == 0.0f);
    CHECK(flt::sc_calc_guard(-std::numeric_limits<F>::infinity()) == 0.0f);
    CHECK(flt::sc_calc_guard(std::numeric_limits<F>::quiet_NaN()) == 0.0f);
    CHECK(flt::sc_calc_guard(F(1.0e30f)) == 0.0f);
}

// A 2-pole biquad recursion mirroring BPerformFilterLoop's inner step, in float,
// with intentionally unstable coefficients (a pole well outside the unit circle).
// Unguarded the float state diverges to non-finite; guarded it stays bounded.
TEST_CASE("float biquad: sc_calc_guard bounds a diverging recursion", "[sc_calc]") {
    using F = flt::sc_calc_t;
    const F b1 = F(2.5f), b2 = F(-1.0f); // |poles| > 1 -> unstable
    auto run = [&](bool guarded) {
        F y1 = 0, y2 = 0, peak = 0;
        for (int i = 0; i < 100000; ++i) {
            F in = (i == 0) ? F(1.0f) : F(0.0f); // unit impulse
            F y0 = in + b1 * y1 + b2 * y2;
            y2 = y1;
            y1 = y0;
            if (guarded) {
                y1 = flt::sc_calc_guard(y1);
                y2 = flt::sc_calc_guard(y2);
            }
            F a = std::fabs(y1);
            if (a > peak)
                peak = a;
        }
        return peak;
    };
    CHECK_FALSE(std::isfinite(run(false))); // unguarded: diverges
    F gpeak = run(true);
    CHECK(std::isfinite(gpeak)); // guarded: stays finite
    CHECK(gpeak < F(1.0e15f));   // and bounded below the guard threshold
}

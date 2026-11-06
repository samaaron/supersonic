// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Sam Aaron
/*
 * test_inline_binary_ops.cpp - the integer binary operators a SynthDef can
 * reach through BinaryOpUGen with any float: gcd and lcm. Their arguments
 * are floats held to the integer's range (sc_float_to), so the most negative
 * integer and the largest arrive as ordinary inputs, and the result must be
 * defined for every pair: a magnitude the integer cannot hold is held to its
 * range, as the float conversion holds its own.
 */
#include <catch2/catch_test_macros.hpp>

#include "SC_InlineBinaryOp.h"

#include <cstdint>
#include <limits>

namespace {
constexpr std::int64_t kMin = std::numeric_limits<std::int64_t>::min();
constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
}

TEST_CASE("gcd: the most negative integer is an ordinary argument", "[inline-ops]") {
    // |kMin| has no int64 of its own: the divisor is taken on magnitudes.
    CHECK(sc_gcd(kMin, std::int64_t(3)) == 1);   // |kMin| is a power of two
    CHECK(sc_gcd(std::int64_t(6), kMin) == 2);
    CHECK(sc_gcd(kMin, std::int64_t(0)) == kMin);
    // Both arguments at most negative: the divisor is their magnitude, and
    // the sign is negative as it is for any two non-positive arguments.
    CHECK(sc_gcd(kMin, kMin) == kMin);
    CHECK(sc_gcd(kMin, std::int64_t(-6)) == -2);
}

TEST_CASE("gcd: the signs upstream gives", "[inline-ops]") {
    CHECK(sc_gcd(std::int64_t(12), std::int64_t(18)) == 6);
    CHECK(sc_gcd(std::int64_t(-12), std::int64_t(18)) == 6);
    CHECK(sc_gcd(std::int64_t(-12), std::int64_t(-18)) == -6);
    CHECK(sc_gcd(std::int64_t(1), std::int64_t(-7)) == 1);
    CHECK(sc_gcd(std::int64_t(-1), std::int64_t(-7)) == -1);
    CHECK(sc_gcd(std::int64_t(0), std::int64_t(0)) == 0);
}

TEST_CASE("lcm: a multiple the integer cannot hold is held to its range", "[inline-ops]") {
    CHECK(sc_lcm(kMax, std::int64_t(2)) == kMax);
    CHECK(sc_lcm(std::int64_t(2), kMax) == kMax);
    CHECK(sc_lcm(kMin, std::int64_t(3)) == kMin);
    CHECK(sc_lcm(kMax, std::int64_t(-2)) == kMin);
    CHECK(sc_lcm(kMin, kMin) == kMin);
    // Large but exact: the product is formed on what is left after the
    // divisor is taken out, so a result that fits is exact.
    CHECK(sc_lcm(std::int64_t(1) << 40, std::int64_t(3) << 40) == (std::int64_t(3) << 40));
}

TEST_CASE("lcm: the signs upstream gives", "[inline-ops]") {
    CHECK(sc_lcm(std::int64_t(4), std::int64_t(6)) == 12);
    CHECK(sc_lcm(std::int64_t(-4), std::int64_t(6)) == -12);
    CHECK(sc_lcm(std::int64_t(-4), std::int64_t(-6)) == -12);
    CHECK(sc_lcm(std::int64_t(0), std::int64_t(6)) == 0);
}

TEST_CASE("gcd and lcm of floats: any float is an argument", "[inline-ops]") {
    CHECK(sc_gcd(-1e30f, 3.0f) == 1.0f);
    CHECK(sc_gcd(-1e30f, -1e30f) == static_cast<float>(kMin));
    CHECK(sc_lcm(1e30f, 2.0f) == static_cast<float>(kMax));
    CHECK(sc_gcd(std::numeric_limits<float>::quiet_NaN(), 5.0f) == 5.0f);   // NaN is 0 as an integer
}

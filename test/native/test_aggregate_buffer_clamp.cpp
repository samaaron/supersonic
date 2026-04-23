/*
 * test_aggregate_buffer_clamp.cpp — Policy unit tests for the aggregate
 * buffer-size floor.
 *
 * Aggregate devices that combine sub-devices with different clock
 * domains run kernel-level sample-rate conversion. That SRC starves at
 * buffer sizes < 256 samples and produces audible warble ("drift
 * storm"). clampBufferForDriftComp enforces the 256-sample floor
 * whenever an aggregate with drift comp is active; otherwise it's a
 * no-op so same-clock aggregates and single devices can still run at
 * 16 / 32 / 64.
 *
 * The pure static function makes this policy directly testable without
 * needing a real CoreAudio device — the wrapper that reads live
 * aggregate state is exercised indirectly via the integration tests
 * whenever switchDevice runs through the aggregate path.
 */
#include <catch2/catch_test_macros.hpp>
#include "DevicePolicy.h"

// Shorthand
static int clamp(int buf, bool active) {
    return sonicpi::device::clampBufferForDriftComp(buf, active);
}

TEST_CASE("AggregateClamp: no-op when drift-comp not active",
          "[AggregateClamp]") {
    REQUIRE(clamp(16, false)   == 16);
    REQUIRE(clamp(32, false)   == 32);
    REQUIRE(clamp(64, false)   == 64);
    REQUIRE(clamp(128, false)  == 128);
    REQUIRE(clamp(256, false)  == 256);
    REQUIRE(clamp(512, false)  == 512);
    REQUIRE(clamp(1024, false) == 1024);
}

TEST_CASE("AggregateClamp: clamps small buffers up to 256 when drift-comp active",
          "[AggregateClamp]") {
    REQUIRE(clamp(1, true)   == sonicpi::device::kMinAggregateBufferSize);
    REQUIRE(clamp(16, true)  == sonicpi::device::kMinAggregateBufferSize);
    REQUIRE(clamp(32, true)  == sonicpi::device::kMinAggregateBufferSize);
    REQUIRE(clamp(64, true)  == sonicpi::device::kMinAggregateBufferSize);
    REQUIRE(clamp(128, true) == sonicpi::device::kMinAggregateBufferSize);
    REQUIRE(clamp(255, true) == sonicpi::device::kMinAggregateBufferSize);
}

TEST_CASE("AggregateClamp: leaves buffers ≥256 alone when drift-comp active",
          "[AggregateClamp]") {
    REQUIRE(clamp(256, true)  == 256);
    REQUIRE(clamp(512, true)  == 512);
    REQUIRE(clamp(1024, true) == 1024);
    REQUIRE(clamp(2048, true) == 2048);
}

TEST_CASE("AggregateClamp: zero / negative bufferSize bypasses clamp",
          "[AggregateClamp]") {
    // 0 = auto (caller will pick the smallest multiple of 128). The clamp
    // must not re-interpret 0 as "too small" and bump it to 256 — that
    // would override the auto-selection machinery.
    REQUIRE(clamp(0, true)   == 0);
    REQUIRE(clamp(0, false)  == 0);
    REQUIRE(clamp(-1, true)  == -1);
    REQUIRE(clamp(-1, false) == -1);
}


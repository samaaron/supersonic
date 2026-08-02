/*
 * test_hardening_policy.cpp — Graph-robustness policy helpers.
 *
 * Guards two failure modes seen in the field (sonic-pi #3553, PipeWire):
 *
 *  1. A session manager scheduling our stream at a huge quantum (8192
 *     frames = 170ms at 48k) made the fixed 150ms gap logger fire on every
 *     healthy callback. The stall threshold must scale with the negotiated
 *     callback period so a big-but-regular quantum is not reported as a
 *     stall, while genuine stalls at ordinary quanta still are.
 *
 *  2. Stale session-manager state can leave the *configured* default
 *     device pointing at hardware that is no longer present (user swapped
 *     sound cards), silently changing how streams negotiate. The registry
 *     mirror detects the ghost so it can be surfaced as a diagnostic.
 */
#include <catch2/catch_test_macros.hpp>
#include "HardeningPolicy.h"

using namespace hardening;

// ── 1. Quantum-aware stall threshold ─────────────────────────────────────────

TEST_CASE("Stall threshold keeps the 150ms floor at ordinary quanta", "[Hardening]") {
    // 128..1024 frames at 48k are all well under the floor.
    REQUIRE(stallThresholdUs(128, 48000.0) == 150'000.0);
    REQUIRE(stallThresholdUs(1024, 48000.0) == 150'000.0);
    REQUIRE(stallThresholdUs(512, 44100.0) == 150'000.0);
}

TEST_CASE("Stall threshold scales with huge negotiated quanta", "[Hardening]") {
    // The #3553 case: quantum 8192 at 48k = 170.7ms per healthy callback.
    // One quantum period must sit BELOW the threshold (not a stall)...
    const double t = stallThresholdUs(8192, 48000.0);
    REQUIRE(170'700.0 < t);
    // ...while a genuinely missed cycle (several periods) sits above it.
    REQUIRE(3 * 170'667.0 > t);
}

TEST_CASE("Stall threshold tolerates degenerate inputs", "[Hardening]") {
    REQUIRE(stallThresholdUs(0, 48000.0) == 150'000.0);
    REQUIRE(stallThresholdUs(-64, 48000.0) == 150'000.0);
    REQUIRE(stallThresholdUs(1024, 0.0) == 150'000.0);
}

// ── 2. Ghost default-device detection ────────────────────────────────────────

TEST_CASE("No ghost when nothing is configured", "[Hardening]") {
    REQUIRE_FALSE(defaultNodeMissing("", { "alsa_output.ssl2" }));
}

TEST_CASE("No ghost when the configured default is present", "[Hardening]") {
    REQUIRE_FALSE(defaultNodeMissing("alsa_output.ssl2",
                                     { "alsa_output.hdmi", "alsa_output.ssl2" }));
}

TEST_CASE("Ghost detected when the configured default is absent", "[Hardening]") {
    // The #3553 case: default still points at an unplugged Scarlett while
    // other sinks are present.
    REQUIRE(defaultNodeMissing("alsa_output.usb-Focusrite_Scarlett_18i20",
                               { "alsa_output.ssl2" }));
}

TEST_CASE("No ghost verdict while the graph has no candidates yet", "[Hardening]") {
    // During boot the metadata can arrive before any nodes have been
    // registered; an empty graph must not read as a ghost.
    REQUIRE_FALSE(defaultNodeMissing("alsa_output.ssl2", {}));
}

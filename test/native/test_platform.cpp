/*
 * test_platform.cpp — contract for the capability platform layer (SC_Platform.h).
 *
 * SC_Platform.h folds the scattered raw-platform branches (ESP_PLATFORM,
 * __EMSCRIPTEN__, endianness, byte-atomics, BSS placement) behind a small set of
 * CAPABILITY macros. The rest of the engine keys off capabilities, never the raw
 * platform macro — so adding a target becomes "define a profile", not "hunt
 * #ifdefs".
 *
 * Capability values per target live in the re-includable SC_PlatformProfile.inc
 * (single source of truth). That lets this host build validate ALL THREE target
 * profiles at compile time — by re-including the table under each forced target —
 * without cross-compiling. The active build's derived macros (SC_LEAN_TARGET,
 * SC_COLD_BSS) are checked directly, since the host compiles the desktop branch.
 */
#include <catch2/catch_test_macros.hpp>

struct Caps {
    int hosted_os;
    int byte_atomics;
    int tiered_memory;
};

// ── Matrix: probe each target profile via the re-includable capability table ──
// Input to the table is exactly one SCP_TARGET_*; output is the SCP_* values.
// We capture each into a constexpr so the matrix is asserted at compile time.

#undef SCP_TARGET_DESKTOP
#undef SCP_TARGET_WASM
#undef SCP_TARGET_ESP32
#define SCP_TARGET_DESKTOP 1
#include "SC_PlatformProfile.inc"
constexpr Caps kDesktop{ SCP_HOSTED_OS, SCP_BYTE_ATOMICS, SCP_TIERED_MEMORY };
#undef SCP_TARGET_DESKTOP

#define SCP_TARGET_WASM 1
#include "SC_PlatformProfile.inc"
constexpr Caps kWasm{ SCP_HOSTED_OS, SCP_BYTE_ATOMICS, SCP_TIERED_MEMORY };
#undef SCP_TARGET_WASM

#define SCP_TARGET_ESP32 1
#include "SC_PlatformProfile.inc"
constexpr Caps kEsp32{ SCP_HOSTED_OS, SCP_BYTE_ATOMICS, SCP_TIERED_MEMORY };
#undef SCP_TARGET_ESP32

// Desktop: full OS, byte atomics, single-tier RAM.
static_assert(kDesktop.hosted_os == 1, "desktop has hosted OS");
static_assert(kDesktop.byte_atomics  == 1, "desktop has byte atomics");
static_assert(kDesktop.tiered_memory == 0, "desktop is single-tier");
// WASM: lean (no hosted OS), byte atomics OK, single-tier.
static_assert(kWasm.hosted_os == 0, "wasm is lean (no hosted OS)");
static_assert(kWasm.byte_atomics  == 1, "wasm has byte atomics");
static_assert(kWasm.tiered_memory == 0, "wasm is single-tier");
// ESP32: lean, NO byte atomics (Xtensa), two-tier SRAM/PSRAM.
static_assert(kEsp32.hosted_os == 0, "esp32 is lean");
static_assert(kEsp32.byte_atomics  == 0, "esp32 lacks byte atomics");
static_assert(kEsp32.tiered_memory == 1, "esp32 is two-tier");

// ── Active build: SC_Platform.h as compiled for the host (= desktop) ──────────
#include "SC_Platform.h"

static_assert(SC_HAS_HOSTED_OS == 1, "host build is desktop: has hosted OS");
static_assert(SC_HAS_BYTE_ATOMICS  == 1, "host build has byte atomics");
static_assert(SC_HAS_TIERED_MEMORY == 0, "host build is single-tier");

// Derived: a desktop host is NOT a lean target.
#ifdef SC_LEAN_TARGET
#    error "desktop host must not define SC_LEAN_TARGET"
#endif

// Invariant: SC_LEAN_TARGET is exactly !SC_HAS_HOSTED_OS.
#if SC_HAS_HOSTED_OS && defined(SC_LEAN_TARGET)
#    error "hosted OS present but SC_LEAN_TARGET defined"
#endif
#if !SC_HAS_HOSTED_OS && !defined(SC_LEAN_TARGET)
#    error "no hosted OS but SC_LEAN_TARGET not derived"
#endif

// SC_COLD_BSS must always be defined (an empty attribute off-embedded) so the
// engine's table declarations compile on every target.
#ifndef SC_COLD_BSS
#    error "SC_COLD_BSS must always be defined"
#endif
SC_COLD_BSS static int sc_cold_bss_probe = 0;  // empty expansion must compile on host

TEST_CASE("SC_Platform exposes a coherent capability matrix", "[platform]") {
    // Runtime mirror of the compile-time matrix (so the case is visible in the
    // suite and the values are double-checked at runtime too).
    CHECK(kDesktop.tiered_memory == 0);
    CHECK(kWasm.hosted_os == 0);
    CHECK(kEsp32.byte_atomics == 0);
    CHECK(kEsp32.tiered_memory == 1);
    (void)sc_cold_bss_probe;
}

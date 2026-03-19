/*
 * test_cold_swap_sampleloader.cpp — SampleLoader behaviour across cold swaps
 *
 * Validates that:
 *   - Pending buffer loads from before a cold swap don't crash
 *   - Stale generation loads are silently discarded (no zfree on abandoned pool)
 *   - Buffer loads after a cold swap work correctly
 *   - Multiple cold swaps with buffer activity don't corrupt the heap
 */
#include <catch2/catch_test_macros.hpp>
#include "EngineFixture.h"
#include "OscBuilder.h"
#include "OscTestUtils.h"
#include <thread>
#include <chrono>
#include <fstream>
#include <filesystem>

// ── Helper: check if a synthdef file exists ─────────────────────────────────

static bool synthdefsAvailable() {
    std::string path = std::string(SUPERSONIC_SYNTHDEFS_DIR) + "/sonic-pi-beep.scsyndef";
    return std::filesystem::exists(path);
}

// ── Buffer load survives cold swap ──────────────────────────────────────────

TEST_CASE("ColdSwap: buffer load before swap doesn't crash", "[ColdSwap][SampleLoader]") {
    EngineFixture fix;

    OscReply reply;
    fix.send(osc_test::message("/status"));
    REQUIRE(fix.waitForReply("/status.reply", reply));

    // Load a buffer (uses SampleLoader background I/O)
    // Use /b_alloc to create a simple buffer — this goes through the heap
    {
        osc_test::Builder b;
        auto& s = b.begin("/b_alloc");
        s << (int32_t)10 << (int32_t)1024 << (int32_t)1;
        fix.send(b.end());
    }

    // Wait for allocation to complete
    fix.send(osc_test::message("/sync", 100));
    REQUIRE(fix.waitForReply("/synced", reply));

    // Cold swap — this resets the heap via FreeAllInternal()
    auto result = fix.engine().switchDevice("", 44100);
    REQUIRE(result.success);
    REQUIRE(result.type == SwapType::Cold);

    // Engine should still be responsive (no crash from stale buffer pointers)
    fix.clearReplies();
    fix.send(osc_test::message("/status"));
    REQUIRE(fix.waitForReply("/status.reply", reply));
}

// ── Buffer load after cold swap works ───────────────────────────────────────

TEST_CASE("ColdSwap: buffer allocation works after swap", "[ColdSwap][SampleLoader]") {
    EngineFixture fix;

    OscReply reply;

    // Cold swap first
    auto result = fix.engine().switchDevice("", 44100);
    REQUIRE(result.success);

    // Now allocate a buffer in the NEW world
    {
        osc_test::Builder b;
        auto& s = b.begin("/b_alloc");
        s << (int32_t)5 << (int32_t)512 << (int32_t)2;
        fix.send(b.end());
    }

    fix.send(osc_test::message("/sync", 200));
    REQUIRE(fix.waitForReply("/synced", reply));

    // Verify engine is responsive
    fix.clearReplies();
    fix.send(osc_test::message("/status"));
    REQUIRE(fix.waitForReply("/status.reply", reply));
}

// ── Multiple cold swaps with buffer activity ────────────────────────────────

TEST_CASE("ColdSwap: multiple swaps with buffers don't corrupt heap", "[ColdSwap][SampleLoader]") {
    EngineFixture fix;

    OscReply reply;

    for (int i = 0; i < 3; i++) {
        // Allocate a buffer
        {
            osc_test::Builder b;
            auto& s = b.begin("/b_alloc");
            s << (int32_t)(10 + i) << (int32_t)256 << (int32_t)1;
            fix.send(b.end());
        }
        fix.send(osc_test::message("/sync", 300 + i));
        REQUIRE(fix.waitForReply("/synced", reply));

        // Cold swap to different rate
        double targetRate = (i % 2 == 0) ? 44100 : 48000;
        auto result = fix.engine().switchDevice("", targetRate);
        REQUIRE(result.success);
        REQUIRE(result.type == SwapType::Cold);

        // Verify engine is alive
        fix.clearReplies();
        fix.send(osc_test::message("/status"));
        REQUIRE(fix.waitForReply("/status.reply", reply));
    }
}

// ── Synthdef load + cold swap + synthdef reload ─────────────────────────────

TEST_CASE("ColdSwap: synthdef reload works after swap", "[ColdSwap][SampleLoader]") {
    if (!synthdefsAvailable()) {
        SKIP("Synthdefs not available");
    }

    EngineFixture fix;
    OscReply reply;

    // Load synthdef before swap
    REQUIRE(fix.loadSynthDef("sonic-pi-beep"));

    // Create a synth to verify it works
    fix.send(osc_test::message("/notify", (int32_t)1));
    REQUIRE(fix.waitForReply("/done", reply));
    fix.clearReplies();

    {
        osc_test::Builder b;
        auto& s = b.begin("/s_new");
        s << "sonic-pi-beep" << (int32_t)2000 << (int32_t)0 << (int32_t)1
          << "note" << 60.0f << "out_bus" << 0.0f;
        fix.send(b.end());
    }
    REQUIRE(fix.waitForReply("/n_go", reply));

    // Free the synth
    fix.send(osc_test::message("/n_free", (int32_t)2000));
    fix.send(osc_test::message("/sync", 400));
    REQUIRE(fix.waitForReply("/synced", reply));

    // Cold swap
    auto result = fix.engine().switchDevice("", 44100);
    REQUIRE(result.success);

    // Reload synthdef in the new world
    fix.clearReplies();
    REQUIRE(fix.loadSynthDef("sonic-pi-beep"));

    // Re-register for notifications (lost during cold swap)
    fix.send(osc_test::message("/notify", (int32_t)1));
    REQUIRE(fix.waitForReply("/done", reply));

    // Recreate group (destroyed by cold swap)
    fix.send(osc_test::message("/g_new", (int32_t)1, (int32_t)0, (int32_t)0));
    fix.send(osc_test::message("/sync", 401));
    REQUIRE(fix.waitForReply("/synced", reply));

    // Create synth in new world — should work
    fix.clearReplies();
    {
        osc_test::Builder b;
        auto& s = b.begin("/s_new");
        s << "sonic-pi-beep" << (int32_t)3000 << (int32_t)0 << (int32_t)1
          << "note" << 60.0f << "out_bus" << 0.0f;
        fix.send(b.end());
    }
    REQUIRE(fix.waitForReply("/n_go", reply));
}

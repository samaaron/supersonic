/*
 * test_world_options_layout.cpp — WorldOptions memory layout tests
 *
 * Validates that:
 *   - WORLD_OPTIONS_START is outside the IN ring buffer
 *   - WorldOptions survive OSC traffic (not corrupted by ring buffer writes)
 *   - After cold swap, worldOptions are still readable with correct values
 *   - After cold swap, engine reports correct sample rate
 */
#include <catch2/catch_test_macros.hpp>
#include "EngineFixture.h"
#include "OscBuilder.h"
#include "src/shared_memory.h"
#include "src/audio_processor.h"
#include <cstring>

// ── Layout validation ───────────────────────────────────────────────────────

TEST_CASE("Layout: WORLD_OPTIONS_START is outside IN ring buffer", "[Layout]") {
    // The IN ring buffer spans [IN_BUFFER_START, IN_BUFFER_START + IN_BUFFER_SIZE).
    // WorldOptions must not overlap with it.
    STATIC_REQUIRE(WORLD_OPTIONS_START >= IN_BUFFER_START + IN_BUFFER_SIZE);
}

TEST_CASE("Layout: WORLD_OPTIONS_START is outside OUT ring buffer", "[Layout]") {
    STATIC_REQUIRE(WORLD_OPTIONS_START >= OUT_BUFFER_START + OUT_BUFFER_SIZE);
}

TEST_CASE("Layout: WORLD_OPTIONS_START is outside DEBUG ring buffer", "[Layout]") {
    STATIC_REQUIRE(WORLD_OPTIONS_START >= DEBUG_BUFFER_START + DEBUG_BUFFER_SIZE);
}

TEST_CASE("Layout: WORLD_OPTIONS fits within TOTAL_BUFFER_SIZE", "[Layout]") {
    STATIC_REQUIRE(WORLD_OPTIONS_START + WORLD_OPTIONS_SIZE <= TOTAL_BUFFER_SIZE);
}

// ── WorldOptions survive OSC traffic ────────────────────────────────────────

TEST_CASE("WorldOptions: not corrupted by OSC message traffic", "[Layout][ColdSwap]") {
    EngineFixture fix;

    // Read the worldOptions as written by initialiseWorld
    uint32_t* opts = reinterpret_cast<uint32_t*>(ring_buffer_storage + WORLD_OPTIONS_START);
    uint32_t originalOpts[18];
    std::memcpy(originalOpts, opts, sizeof(originalOpts));

    // Sanity: values should be non-zero (numBuffers, maxNodes, etc.)
    REQUIRE(originalOpts[0] > 0);   // numBuffers
    REQUIRE(originalOpts[1] > 0);   // maxNodes
    REQUIRE(originalOpts[9] > 0);   // realTimeMemorySize
    REQUIRE(originalOpts[14] > 0);  // preferredSampleRate (48000)

    // Send a burst of OSC messages to fill the IN ring buffer
    for (int i = 0; i < 200; i++) {
        fix.send(osc_test::message("/status"));
    }

    // Wait for some to be processed
    OscReply reply;
    fix.send(osc_test::message("/sync", 99));
    fix.waitForReply("/synced", reply, 3000);

    // Verify worldOptions are unchanged
    uint32_t afterOpts[18];
    std::memcpy(afterOpts, opts, sizeof(afterOpts));
    for (int i = 0; i < 18; i++) {
        REQUIRE(afterOpts[i] == originalOpts[i]);
    }
}

// ── Cold swap preserves worldOptions ────────────────────────────────────────

TEST_CASE("ColdSwap: worldOptions preserved after rate change", "[ColdSwap][Layout]") {
    EngineFixture fix;

    // Read original worldOptions
    uint32_t* opts = reinterpret_cast<uint32_t*>(ring_buffer_storage + WORLD_OPTIONS_START);
    uint32_t originalOpts[18];
    std::memcpy(originalOpts, opts, sizeof(originalOpts));

    // Cold swap to 44100
    auto result = fix.engine().switchDevice("", 44100);
    REQUIRE(result.success);
    REQUIRE(result.type == SwapType::Cold);

    // WorldOptions should be preserved (only opts[14] = sampleRate changes)
    uint32_t afterOpts[18];
    std::memcpy(afterOpts, opts, sizeof(afterOpts));

    for (int i = 0; i < 18; i++) {
        if (i == 14) {
            // Sample rate should be updated to 44100
            REQUIRE(afterOpts[14] == 44100);
        } else {
            REQUIRE(afterOpts[i] == originalOpts[i]);
        }
    }
}

// ── Cold swap reports correct sample rate ───────────────────────────────────

TEST_CASE("ColdSwap: engine reports correct rate after swap", "[ColdSwap]") {
    EngineFixture fix;

    // Verify initial rate
    OscReply reply;
    fix.send(osc_test::message("/status"));
    REQUIRE(fix.waitForReply("/status.reply", reply));

    // Cold swap to 44100
    auto result = fix.engine().switchDevice("", 44100);
    REQUIRE(result.success);
    REQUIRE(result.sampleRate == 44100);

    // Engine should process OSC at new rate
    fix.clearReplies();
    fix.send(osc_test::message("/status"));
    REQUIRE(fix.waitForReply("/status.reply", reply));

    // Round-trip: swap back to 48000
    result = fix.engine().switchDevice("", 48000);
    REQUIRE(result.success);
    REQUIRE(result.sampleRate == 48000);

    // Verify worldOptions[14] matches
    uint32_t* opts = reinterpret_cast<uint32_t*>(ring_buffer_storage + WORLD_OPTIONS_START);
    REQUIRE(opts[14] == 48000);

    // Engine still responds
    fix.clearReplies();
    fix.send(osc_test::message("/status"));
    REQUIRE(fix.waitForReply("/status.reply", reply));
}

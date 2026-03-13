/*
 * test_error_handling.cpp — Error handling and malformed input tests.
 * Ported from osc_semantic.spec.mjs error-handling scenarios.
 *
 * Every test verifies the engine does not crash when given invalid,
 * malformed, or boundary-case input, and that it continues to operate
 * normally afterwards.
 */
#include "EngineFixture.h"
#include <cstdlib>
#include <vector>
#include <string>

// ---------------------------------------------------------------------------
// Helper: build an /s_new message
// ---------------------------------------------------------------------------
static osc_test::Packet sNew(const char* def, int32_t id, int32_t addAction, int32_t target) {
    osc_test::Builder b;
    auto& s = b.begin("/s_new");
    s << def << id << addAction << target;
    return b.end();
}

// =============================================================================
// SECTION: Invalid node operations
// =============================================================================

TEST_CASE("Invalid node ID returns error or is handled", "[error]") {
    EngineFixture fx;

    // /n_set on a node that was never created
    {
        osc_test::Builder b;
        auto& s = b.begin("/n_set");
        s << (int32_t)99999 << "freq" << 440.0f;
        fx.send(b.end());
    }

    // Engine must still be responsive
    fx.send(osc_test::message("/status"));
    OscReply r;
    REQUIRE(fx.waitForReply("/status.reply", r));
    SUCCEED();
}

TEST_CASE("Duplicate node ID fails gracefully", "[error]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    // Create synth 1000
    fx.send(sNew("sonic-pi-beep", 1000, 0, 1));

    // Attempt to create another synth with the same ID
    fx.send(sNew("sonic-pi-beep", 1000, 0, 1));

    // /status.reply args: unused(0), numUgens(1), numSynths(2), numGroups(3)
    fx.send(osc_test::message("/status"));
    OscReply r;
    REQUIRE(fx.waitForReply("/status.reply", r));
    CHECK(r.parsed().argInt(2) == 1);  // second creation should fail; only 1 synth

    // Cleanup
    fx.send(osc_test::message("/n_free", 1000));
}

TEST_CASE("Freeing root group is prevented", "[error]") {
    EngineFixture fx;

    // Attempt to free node 0 (root group)
    fx.send(osc_test::message("/n_free", 0));

    // Engine must still be alive and the root group must still exist
    fx.send(osc_test::message("/status"));
    OscReply r;
    REQUIRE(fx.waitForReply("/status.reply", r));
    CHECK(r.parsed().argInt(3) >= 1);  // at least 1 group still present
}

TEST_CASE("Setting controls on non-existent node", "[error]") {
    EngineFixture fx;

    {
        osc_test::Builder b;
        auto& s = b.begin("/n_set");
        s << (int32_t)99999 << "note" << 72.0f;
        fx.send(b.end());
    }

    // Engine must still respond
    fx.send(osc_test::message("/status"));
    OscReply r;
    REQUIRE(fx.waitForReply("/status.reply", r));
}

// =============================================================================
// SECTION: Invalid buffer operations
// =============================================================================

TEST_CASE("Buffer operation on unallocated buffer", "[error]") {
    EngineFixture fx;

    // Query a buffer that was never allocated
    fx.send(osc_test::message("/b_query", 999));

    // Engine must still respond
    fx.send(osc_test::message("/status"));
    OscReply r;
    REQUIRE(fx.waitForReply("/status.reply", r));
}

TEST_CASE("/b_alloc with extreme values", "[error]") {
    EngineFixture fx;

    // Attempt to allocate a buffer with a large (but not OOM-fatal) frame count.
    // 10M frames × 4 bytes = ~40MB — large enough to stress the allocator
    // without exhausting memory on CI runners.
    fx.send(osc_test::message("/b_alloc", 0, 10000000, 1));

    // Wait for the allocation to complete (or fail) before checking health
    OscReply done;
    fx.waitForReply("/done", done, 5000);

    // Engine must still respond
    fx.send(osc_test::message("/status"));
    OscReply r;
    REQUIRE(fx.waitForReply("/status.reply", r));

    // Cleanup
    fx.send(osc_test::message("/b_free", 0));
}

TEST_CASE("/b_set with out-of-bounds index", "[error]") {
    EngineFixture fx;

    // Allocate a small buffer
    fx.send(osc_test::message("/b_alloc", 0, 100, 1));
    OscReply alloc;
    REQUIRE(fx.waitForReply("/done", alloc));
    fx.clearReplies();

    // Set a sample at an out-of-bounds index
    {
        osc_test::Builder b;
        auto& s = b.begin("/b_set");
        s << (int32_t)0 << (int32_t)999 << 0.5f;
        fx.send(b.end());
    }

    // Engine must still respond
    fx.send(osc_test::message("/status"));
    OscReply r;
    REQUIRE(fx.waitForReply("/status.reply", r));

    // Cleanup
    fx.send(osc_test::message("/b_free", 0));
}

// =============================================================================
// SECTION: Malformed synthdef data
// =============================================================================

TEST_CASE("Truncated synthdef header", "[error]") {
    EngineFixture fx;

    // SCgf magic bytes, but truncated (only 4 bytes instead of a full synthdef)
    uint8_t truncated[] = { 0x53, 0x43, 0x67, 0x66 };
    fx.send(osc_test::messageWithBlob("/d_recv", truncated, sizeof(truncated)));

    // Engine must still respond
    fx.send(osc_test::message("/status"));
    OscReply r;
    REQUIRE(fx.waitForReply("/status.reply", r));
}

TEST_CASE("Empty synthdef data", "[error]") {
    EngineFixture fx;

    // Send /d_recv with an empty blob
    std::vector<uint8_t> empty;
    fx.send(osc_test::messageWithBlob("/d_recv", empty.data(), 0));

    // Engine must still respond
    fx.send(osc_test::message("/status"));
    OscReply r;
    REQUIRE(fx.waitForReply("/status.reply", r));
}

TEST_CASE("Random garbage as synthdef", "[error]") {
    EngineFixture fx;

    // 256 bytes of deterministic garbage (fixed pattern, not rand())
    std::vector<uint8_t> garbage(256, 0xAB);
    fx.send(osc_test::messageWithBlob("/d_recv", garbage.data(), garbage.size()));

    // Engine must still respond
    fx.send(osc_test::message("/status"));
    OscReply r;
    REQUIRE(fx.waitForReply("/status.reply", r));
}

TEST_CASE("Valid synthdef loads after malformed attempts", "[error]") {
    EngineFixture fx;

    // Send garbage synthdef first
    std::vector<uint8_t> garbage(256, 0xAB);
    fx.send(osc_test::messageWithBlob("/d_recv", garbage.data(), garbage.size()));

    // Now load a real synthdef — it must still succeed
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    // Create a synth to prove the valid def is usable
    fx.send(sNew("sonic-pi-beep", 1000, 0, 1));

    fx.send(osc_test::message("/status"));
    OscReply r;
    REQUIRE(fx.waitForReply("/status.reply", r));
    CHECK(r.parsed().argInt(2) >= 1);  // at least 1 synth running

    // Cleanup
    fx.send(osc_test::message("/n_free", 1000));
}

// =============================================================================
// SECTION: Invalid OSC commands
// =============================================================================

TEST_CASE("Unknown OSC command handled gracefully", "[error]") {
    EngineFixture fx;

    fx.send(osc_test::message("/nonexistent_command"));

    // Engine must still respond to /status
    fx.send(osc_test::message("/status"));
    OscReply r;
    REQUIRE(fx.waitForReply("/status.reply", r));
    SUCCEED();
}

TEST_CASE("/s_new with non-existent synthdef", "[error]") {
    EngineFixture fx;

    fx.send(sNew("totally_fake_synthdef", 1000, 0, 1));

    // No synth should have been created
    fx.send(osc_test::message("/status"));
    OscReply r;
    REQUIRE(fx.waitForReply("/status.reply", r));
    CHECK(r.parsed().argInt(2) == 0);  // numSynths == 0
}

TEST_CASE("/s_new with invalid add action", "[error]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    // Add action 99 is invalid (valid range: 0-4)
    {
        osc_test::Builder b;
        auto& s = b.begin("/s_new");
        s << "sonic-pi-beep" << (int32_t)1000 << (int32_t)99 << (int32_t)1;
        fx.send(b.end());
    }

    // Engine must still respond
    fx.send(osc_test::message("/status"));
    OscReply r;
    REQUIRE(fx.waitForReply("/status.reply", r));

    // Cleanup attempt — the synth may or may not have been created
    fx.send(osc_test::message("/n_free", 1000));
}

TEST_CASE("/g_new with non-existent target", "[error]") {
    EngineFixture fx;

    // Create group 100 with add-to-head of non-existent target 99999
    fx.send(osc_test::message("/g_new", 100, 0, 99999));

    // Engine must still respond
    fx.send(osc_test::message("/status"));
    OscReply r;
    REQUIRE(fx.waitForReply("/status.reply", r));
}

// =============================================================================
// SECTION: Robustness after errors
// =============================================================================

TEST_CASE("Engine recovers after rapid malformed commands", "[error]") {
    EngineFixture fx;

    // Send 50 unknown commands in rapid succession
    for (int i = 0; i < 50; ++i) {
        std::string cmd = "/badcmd" + std::to_string(i);
        fx.send(osc_test::message(cmd.c_str()));
    }

    // Engine must still respond to /status
    fx.send(osc_test::message("/status"));
    OscReply r;
    REQUIRE(fx.waitForReply("/status.reply", r));
    SUCCEED();
}

TEST_CASE("Engine works after buffer errors", "[error]") {
    EngineFixture fx;

    // Perform several invalid buffer operations
    fx.send(osc_test::message("/b_free", 999));     // never allocated
    fx.send(osc_test::message("/b_zero", 998));      // never allocated
    fx.send(osc_test::message("/b_query", 997));     // never allocated

    // Now do a legitimate buffer allocation
    fx.send(osc_test::message("/b_alloc", 0, 1024, 1));
    OscReply r;
    REQUIRE(fx.waitForReply("/done", r));

    // Cleanup
    fx.send(osc_test::message("/b_free", 0));
}

TEST_CASE("Engine works after node errors", "[error]") {
    EngineFixture fx;

    // Perform several invalid node operations
    fx.send(osc_test::message("/n_free", 99999));
    {
        osc_test::Builder b;
        auto& s = b.begin("/n_set");
        s << (int32_t)88888 << "x" << 1.0f;
        fx.send(b.end());
    }
    fx.send(osc_test::message("/n_run", 77777, 0));

    // Now do legitimate synth work
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));
    fx.send(sNew("sonic-pi-beep", 1000, 0, 1));

    fx.send(osc_test::message("/status"));
    OscReply r;
    REQUIRE(fx.waitForReply("/status.reply", r));
    CHECK(r.parsed().argInt(2) >= 1);  // at least 1 synth running

    // Cleanup
    fx.send(osc_test::message("/n_free", 1000));
}

// =============================================================================
// SECTION: Oversized / boundary messages
// =============================================================================

TEST_CASE("Very long synthdef name in /s_new", "[error]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    // Build a 200-character synthdef name (all 'x')
    std::string longName(200, 'x');
    fx.send(sNew(longName.c_str(), 1000, 0, 1));

    // Engine must still respond
    fx.send(osc_test::message("/status"));
    OscReply r;
    REQUIRE(fx.waitForReply("/status.reply", r));
}

TEST_CASE("Many arguments in single /n_set", "[error]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    // Create a synth to target
    fx.send(sNew("sonic-pi-beep", 1000, 0, 1));

    // Send /n_set with 50 control pairs
    {
        osc_test::Builder b;
        auto& s = b.begin("/n_set");
        s << (int32_t)1000;
        for (int i = 0; i < 50; ++i) {
            s << "note" << 60.0f;
        }
        fx.send(b.end());
    }

    // Engine must still respond and synth still exists
    fx.send(osc_test::message("/status"));
    OscReply r;
    REQUIRE(fx.waitForReply("/status.reply", r));
    CHECK(r.parsed().argInt(2) >= 1);  // synth still running

    // Cleanup
    fx.send(osc_test::message("/n_free", 1000));
}

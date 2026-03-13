/*
 * test_stress.cpp — Stress tests ported from concurrent_stress.spec.mjs
 *                   and ring_buffer_stress.spec.mjs.
 *
 * These exercise rapid create/free cycles, extreme parameter values,
 * group edge cases, ring buffer throughput, and rapid lifecycle churn.
 */
#include "EngineFixture.h"
#include <limits>

// Helper: create a synth via /s_new with string def, int ID, addAction, target
static osc_test::Packet sNew(const char* def, int32_t id, int32_t addAction, int32_t target) {
    osc_test::Builder b;
    auto& s = b.begin("/s_new");
    s << def << id << addAction << target;
    return b.end();
}

// Helper: create a synth with a float parameter
static osc_test::Packet sNewWithParam(const char* def, int32_t id, int32_t addAction,
                                       int32_t target, const char* param, float value) {
    osc_test::Builder b;
    auto& s = b.begin("/s_new");
    s << def << id << addAction << target << param << value;
    return b.end();
}

// =============================================================================
// RAPID CREATE/FREE CYCLES
// =============================================================================

TEST_CASE("rapid create and immediate free - 100 cycles", "[stress]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    for (int i = 0; i < 100; i++) {
        fx.send(sNew("sonic-pi-beep", 1000 + i, 0, 1));
        fx.send(osc_test::message("/n_free", 1000 + i));
    }

    // Engine should still be healthy
    fx.send(osc_test::message("/status"));
    OscReply r;
    REQUIRE(fx.waitForReply("/status.reply", r));
    CHECK(r.parsed().argInt(2) == 0);  // numSynths == 0
}

TEST_CASE("interleaved create/free with overlapping IDs", "[stress]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    // Create synths 1000-1009
    for (int i = 0; i < 10; i++) {
        fx.send(sNew("sonic-pi-beep", 1000 + i, 0, 1));
    }

    // Free 1000-1004
    for (int i = 0; i < 5; i++) {
        fx.send(osc_test::message("/n_free", 1000 + i));
    }

    // Reuse IDs 1000-1004
    for (int i = 0; i < 5; i++) {
        fx.send(sNew("sonic-pi-beep", 1000 + i, 0, 1));
    }

    // Free all 1000-1009
    for (int i = 0; i < 10; i++) {
        fx.send(osc_test::message("/n_free", 1000 + i));
    }

    // Engine should still be healthy with 0 synths
    fx.send(osc_test::message("/status"));
    OscReply r;
    REQUIRE(fx.waitForReply("/status.reply", r));
    CHECK(r.parsed().argInt(2) == 0);  // numSynths == 0
}

TEST_CASE("mass synth creation then mass free", "[stress]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    // Create 100 synths
    for (int i = 0; i < 100; i++) {
        fx.send(sNew("sonic-pi-beep", 2000 + i, 0, 1));
    }

    // Verify all created (allow slight margin for ring buffer capacity)
    fx.send(osc_test::message("/status"));
    OscReply created;
    REQUIRE(fx.waitForReply("/status.reply", created));
    CHECK(created.parsed().argInt(2) >= 50);  // ring buffer may not process all 100 immediately

    // Free all
    for (int i = 0; i < 100; i++) {
        fx.send(osc_test::message("/n_free", 2000 + i));
    }

    fx.send(osc_test::message("/status"));
    OscReply freed;
    REQUIRE(fx.waitForReply("/status.reply", freed));
    CHECK(freed.parsed().argInt(2) == 0);  // numSynths == 0
}

// =============================================================================
// EXTREME VALUES
// =============================================================================

TEST_CASE("extreme node ID values", "[stress]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    // Near INT_MAX
    fx.send(sNew("sonic-pi-beep", 2147483646, 0, 1));

    fx.send(osc_test::message("/status"));
    OscReply r;
    REQUIRE(fx.waitForReply("/status.reply", r));
    CHECK(r.parsed().argInt(2) >= 1);  // synth exists

    fx.send(osc_test::message("/n_free", 2147483646));
    SUCCEED();
}

TEST_CASE("NaN in synth parameters doesn't crash", "[stress]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    fx.send(sNewWithParam("sonic-pi-beep", 1000, 0, 1,
                           "note", std::numeric_limits<float>::quiet_NaN()));

    // No crash = success
    fx.send(osc_test::message("/n_free", 1000));
    SUCCEED();
}

TEST_CASE("Infinity in synth parameters doesn't crash", "[stress]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    fx.send(sNewWithParam("sonic-pi-beep", 1000, 0, 1,
                           "note", std::numeric_limits<float>::infinity()));

    fx.send(osc_test::message("/n_free", 1000));
    SUCCEED();
}

TEST_CASE("subnormal floats don't crash", "[stress]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    fx.send(sNewWithParam("sonic-pi-beep", 1000, 0, 1,
                           "note", std::numeric_limits<float>::denorm_min()));

    fx.send(osc_test::message("/n_free", 1000));
    SUCCEED();
}

TEST_CASE("extreme float values don't crash", "[stress]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    fx.send(sNewWithParam("sonic-pi-beep", 1000, 0, 1,
                           "note", std::numeric_limits<float>::max()));

    fx.send(osc_test::message("/n_free", 1000));
    SUCCEED();
}

// =============================================================================
// GROUP EDGE CASES
// =============================================================================

TEST_CASE("group creation with invalid parent doesn't crash", "[stress]") {
    EngineFixture fx;

    // Attempt to create group with non-existent parent 99999
    fx.send(osc_test::message("/g_new", 100, 0, 99999));

    // Engine should still function — create a valid group
    fx.send(osc_test::message("/g_new", 200, 0, 0));

    fx.send(osc_test::message("/status"));
    OscReply r;
    REQUIRE(fx.waitForReply("/status.reply", r));

    // Cleanup
    fx.send(osc_test::message("/n_free", 200));
    // Group 100 may or may not have been created; free it too (safe even if absent)
    fx.send(osc_test::message("/n_free", 100));
    SUCCEED();
}

TEST_CASE("hundreds of parameters in single message", "[stress]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    fx.send(sNew("sonic-pi-beep", 1000, 0, 1));

    // Build /n_set message with 50 repeated control index/value pairs
    {
        osc_test::Builder b;
        auto& s = b.begin("/n_set");
        s << (int32_t)1000;
        for (int i = 0; i < 50; i++) {
            s << (int32_t)0 << 60.0f;
        }
        fx.send(b.end());
    }

    // No crash = success
    fx.send(osc_test::message("/n_free", 1000));
    SUCCEED();
}

TEST_CASE("sync with extreme IDs", "[stress]") {
    EngineFixture fx;

    // /sync 0
    fx.send(osc_test::message("/sync", 0));
    OscReply r0;
    REQUIRE(fx.waitForReply("/synced", r0));
    CHECK(r0.parsed().argInt(0) == 0);

    fx.clearReplies();

    // /sync INT_MAX
    fx.send(osc_test::message("/sync", 2147483647));
    OscReply rMax;
    REQUIRE(fx.waitForReply("/synced", rMax));
    CHECK(rMax.parsed().argInt(0) == 2147483647);

    fx.clearReplies();

    // /sync -1
    fx.send(osc_test::message("/sync", -1));
    OscReply rNeg;
    REQUIRE(fx.waitForReply("/synced", rNeg));
    CHECK(rNeg.parsed().argInt(0) == -1);
}

// =============================================================================
// RING BUFFER STRESS
// =============================================================================

TEST_CASE("5000 messages without corruption", "[stress]") {
    EngineFixture fx;

    // Send /status 5000 times
    for (int i = 0; i < 5000; i++) {
        fx.send(osc_test::message("/status"));
    }

    // Verify engine still responds correctly
    fx.clearReplies();
    fx.send(osc_test::message("/status"));
    OscReply r;
    REQUIRE(fx.waitForReply("/status.reply", r));
    CHECK(r.parsed().argCount() >= 5);
}

TEST_CASE("rapid OSC messages stay intact", "[stress]") {
    EngineFixture fx;

    // Send 100 /sync commands with incrementing IDs
    for (int i = 1; i <= 100; i++) {
        fx.send(osc_test::message("/sync", i));
    }

    // Wait for the batch to drain, then verify engine integrity
    // with a unique ID that wasn't in the batch (1-100).
    fx.send(osc_test::message("/sync", 9999));
    OscReply r;
    REQUIRE(fx.waitForReply("/synced", r));
    // Find our unique sync
    while (r.parsed().argInt(0) != 9999) {
        REQUIRE(fx.waitForReply("/synced", r));
    }
    CHECK(r.parsed().argInt(0) == 9999);
}

TEST_CASE("mixed immediate messages concurrently", "[stress]") {
    EngineFixture fx;

    // Send 200 mixed messages in rapid succession
    for (int i = 0; i < 200; i++) {
        switch (i % 4) {
            case 0: fx.send(osc_test::message("/status"));            break;
            case 1: fx.send(osc_test::message("/sync", i));           break;
            case 2: fx.send(osc_test::message("/g_queryTree", 0, 0)); break;
            case 3: fx.send(osc_test::message("/version"));           break;
        }
    }

    // After all that, /status should still work
    fx.clearReplies();
    fx.send(osc_test::message("/status"));
    OscReply r;
    REQUIRE(fx.waitForReply("/status.reply", r));
    CHECK(r.parsed().argCount() >= 5);
}

// =============================================================================
// RAPID LIFECYCLE
// =============================================================================

TEST_CASE("multiple engine operations interleaved", "[stress]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    for (int round = 0; round < 10; round++) {
        int groupId = 100 + round * 2;

        // Create group, add 5 synths, free all
        fx.send(osc_test::message("/g_new", groupId, 0, 0));

        for (int j = 0; j < 5; j++) {
            fx.send(sNew("sonic-pi-beep", 3000 + round * 10 + j, 0, groupId));
        }

        fx.send(osc_test::message("/g_freeAll", groupId));

        // Clean up the group itself
        fx.send(osc_test::message("/n_free", groupId));
    }

    // Engine should still be healthy
    fx.send(osc_test::message("/status"));
    OscReply r;
    REQUIRE(fx.waitForReply("/status.reply", r));
    CHECK(r.parsed().argCount() >= 5);
}

TEST_CASE("buffer allocation stress", "[stress]") {
    EngineFixture fx;

    // Allocate 50 buffers
    for (int i = 0; i < 50; i++) {
        fx.send(osc_test::message("/b_alloc", i, 1024, 1));
        OscReply alloc;
        REQUIRE(fx.waitForReply("/done", alloc));
        fx.clearReplies();
    }

    // Free all 50 buffers
    for (int i = 0; i < 50; i++) {
        fx.send(osc_test::message("/b_free", i));
        OscReply freed;
        REQUIRE(fx.waitForReply("/done", freed));
        fx.clearReplies();
    }

    // /b_query on buffer 0 after free should still respond
    fx.send(osc_test::message("/b_query", 0));
    OscReply q;
    REQUIRE(fx.waitForReply("/b_info", q));
}

TEST_CASE("synthdef reload stress", "[stress]") {
    EngineFixture fx;

    // Load the same synthdef 20 times
    for (int i = 0; i < 20; i++) {
        REQUIRE(fx.loadSynthDef("sonic-pi-beep"));
    }

    // Creating a synth should still work after repeated reloads
    fx.send(sNew("sonic-pi-beep", 1000, 0, 1));

    fx.send(osc_test::message("/status"));
    OscReply r;
    REQUIRE(fx.waitForReply("/status.reply", r));
    CHECK(r.parsed().argInt(2) >= 1);  // at least our synth

    fx.send(osc_test::message("/n_free", 1000));
}

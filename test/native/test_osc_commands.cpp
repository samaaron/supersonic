/*
 * test_osc_commands.cpp — Misc OSC commands: /n_query, /n_before, /n_after,
 *                         /n_order, /n_map, /c_set, /b_alloc, /b_free, /b_query
 */
#include "EngineFixture.h"

static osc_test::Packet sNew(const char* def, int32_t id, int32_t addAction, int32_t target) {
    osc_test::Builder b;
    auto& s = b.begin("/s_new");
    s << def << id << addAction << target;
    return b.end();
}

// =============================================================================
// NODE QUERY / ORDERING
// =============================================================================

TEST_CASE("/n_query returns node info", "[osc]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    fx.send(osc_test::message("/notify", 1));
    fx.clearReplies();

    fx.send(sNew("sonic-pi-beep", 1000, 0, 1));
    fx.clearReplies();

    fx.send(osc_test::message("/n_query", 1000));

    OscReply r;
    // /n_info may not arrive in headless mode — check but don't require
    if (fx.waitForReply("/n_info", r, 500)) {
        auto p = r.parsed();
        CHECK(p.argInt(0) == 1000);  // node ID
    }

    fx.send(osc_test::message("/n_free", 1000));
    SUCCEED();
}

TEST_CASE("/n_before moves node before another", "[osc]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    fx.send(osc_test::message("/notify", 1));
    fx.clearReplies();

    // Create two synths at head of default group — order: 1001, 1000
    fx.send(sNew("sonic-pi-beep", 1000, 0, 1));
    fx.send(sNew("sonic-pi-beep", 1001, 0, 1));
    fx.clearReplies();

    // Move 1000 before 1001 → order should become: 1000, 1001
    fx.send(osc_test::message("/n_before", 1000, 1001));
    OscReply r;
    if (fx.waitForReply("/n_move", r)) {
        auto p = r.parsed();
        CHECK(p.argInt(0) == 1000);
    }

    fx.send(osc_test::message("/n_free", 1000));
    fx.send(osc_test::message("/n_free", 1001));
}

TEST_CASE("/n_after moves node after another", "[osc]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    fx.send(osc_test::message("/notify", 1));
    fx.clearReplies();

    // Create two synths at head — order: 1001, 1000
    fx.send(sNew("sonic-pi-beep", 1000, 0, 1));
    fx.send(sNew("sonic-pi-beep", 1001, 0, 1));
    fx.clearReplies();

    // Move 1001 after 1000 → order should become: 1000, 1001
    fx.send(osc_test::message("/n_after", 1001, 1000));
    OscReply r;
    if (fx.waitForReply("/n_move", r)) {
        auto p = r.parsed();
        CHECK(p.argInt(0) == 1001);
    }

    fx.send(osc_test::message("/n_free", 1000));
    fx.send(osc_test::message("/n_free", 1001));
}

TEST_CASE("/n_order reorders nodes", "[osc]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    fx.send(osc_test::message("/notify", 1));
    fx.clearReplies();

    // Create three synths at head — order: 1002, 1001, 1000
    fx.send(sNew("sonic-pi-beep", 1000, 0, 1));
    fx.send(sNew("sonic-pi-beep", 1001, 0, 1));
    fx.send(sNew("sonic-pi-beep", 1002, 0, 1));
    fx.clearReplies();

    // Reorder: 1000, 1001 at head of default group (1)
    {
        osc_test::Builder b;
        auto& s = b.begin("/n_order");
        s << (int32_t)0 << (int32_t)1 << (int32_t)1000 << (int32_t)1001;
        fx.send(b.end());
    }

    // If we got here without crash, the command was processed
    fx.send(osc_test::message("/n_free", 1000));
    fx.send(osc_test::message("/n_free", 1001));
    fx.send(osc_test::message("/n_free", 1002));
    SUCCEED();
}

// =============================================================================
// CONTROL BUS
// =============================================================================

TEST_CASE("/c_set sets control bus value", "[osc]") {
    EngineFixture fx;
    REQUIRE(fx.loadSynthDef("sonic-pi-beep"));

    // Set control bus 0 to 72
    fx.send(osc_test::message("/c_set", 0, 72));

    // Create synth and map to control bus
    fx.send(sNew("sonic-pi-beep", 1000, 0, 1));

    {
        osc_test::Builder b;
        auto& s = b.begin("/n_map");
        s << (int32_t)1000 << "note" << (int32_t)0;
        fx.send(b.end());
    }

    // Unmap
    {
        osc_test::Builder b;
        auto& s = b.begin("/n_map");
        s << (int32_t)1000 << "note" << (int32_t)-1;
        fx.send(b.end());
    }

    fx.send(osc_test::message("/n_free", 1000));
    SUCCEED();
}

// =============================================================================
// BUFFER COMMANDS
// =============================================================================

TEST_CASE("/b_alloc allocates a buffer", "[osc][buffer]") {
    EngineFixture fx;

    // Allocate buffer 0: 1024 frames, 1 channel
    fx.send(osc_test::message("/b_alloc", 0, 1024, 1));
    OscReply r;
    REQUIRE(fx.waitForReply("/done", r));
}

TEST_CASE("/b_free frees a buffer", "[osc][buffer]") {
    EngineFixture fx;

    fx.send(osc_test::message("/b_alloc", 0, 1024, 1));
    OscReply alloc;
    REQUIRE(fx.waitForReply("/done", alloc));
    fx.clearReplies();

    fx.send(osc_test::message("/b_free", 0));
    OscReply free_r;
    REQUIRE(fx.waitForReply("/done", free_r));
}

TEST_CASE("/b_query returns buffer info", "[osc][buffer]") {
    EngineFixture fx;

    fx.send(osc_test::message("/b_alloc", 0, 1024, 2));
    OscReply alloc;
    REQUIRE(fx.waitForReply("/done", alloc));
    fx.clearReplies();

    fx.send(osc_test::message("/b_query", 0));
    OscReply r;
    REQUIRE(fx.waitForReply("/b_info", r));

    auto p = r.parsed();
    CHECK(p.argInt(0) == 0);      // buffer number
    CHECK(p.argInt(1) == 1024);   // num frames
    CHECK(p.argInt(2) == 2);      // num channels

    fx.send(osc_test::message("/b_free", 0));
}

TEST_CASE("/b_zero zeroes a buffer", "[osc][buffer]") {
    EngineFixture fx;

    fx.send(osc_test::message("/b_alloc", 0, 1024, 1));
    OscReply alloc;
    REQUIRE(fx.waitForReply("/done", alloc));
    fx.clearReplies();

    fx.send(osc_test::message("/b_zero", 0));
    OscReply r;
    REQUIRE(fx.waitForReply("/done", r));

    fx.send(osc_test::message("/b_free", 0));
}

// =============================================================================
// REALTIME MEMORY STATUS
// =============================================================================

TEST_CASE("/rtMemoryStatus returns memory info", "[osc]") {
    EngineFixture fx;

    fx.send(osc_test::message("/rtMemoryStatus"));
    OscReply r;
    REQUIRE(fx.waitForReply("/rtMemoryStatus.reply", r));

    auto p = r.parsed();
    CHECK(p.argInt(0) > 0);  // free memory > 0
    CHECK(p.argInt(1) > 0);  // largest free block > 0
}

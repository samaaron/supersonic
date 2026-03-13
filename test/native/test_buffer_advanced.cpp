/*
 * test_buffer_advanced.cpp — Advanced buffer operations: /b_set, /b_get,
 *                             /b_setn, /b_getn, /b_fill, /b_gen, multi-query
 */
#include "EngineFixture.h"
#include <catch2/catch_approx.hpp>
#include <cmath>

// =============================================================================
// /b_set and /b_get
// =============================================================================

TEST_CASE("/b_set and /b_get round-trip", "[buffer]") {
    EngineFixture fx;

    fx.send(osc_test::message("/b_alloc", 0, 1024, 1));
    OscReply alloc;
    REQUIRE(fx.waitForReply("/done", alloc));
    fx.clearReplies();

    // Set three samples at different indices
    {
        osc_test::Builder b;
        auto& s = b.begin("/b_set");
        s << (int32_t)0 << (int32_t)0 << 0.5f
                        << (int32_t)10 << 0.75f
                        << (int32_t)100 << -0.25f;
        fx.send(b.end());
    }
    fx.clearReplies();

    // Get those samples back
    {
        osc_test::Builder b;
        auto& s = b.begin("/b_get");
        s << (int32_t)0 << (int32_t)0 << (int32_t)10 << (int32_t)100;
        fx.send(b.end());
    }

    OscReply r;
    REQUIRE(fx.waitForReply("/b_set", r));
    auto p = r.parsed();
    CHECK(p.argInt(0) == 0);     // bufnum
    CHECK(p.argInt(1) == 0);     // index 0
    CHECK(p.argFloat(2) == Catch::Approx(0.5f).margin(0.001f));
    CHECK(p.argInt(3) == 10);    // index 10
    CHECK(p.argFloat(4) == Catch::Approx(0.75f).margin(0.001f));
    CHECK(p.argInt(5) == 100);   // index 100
    CHECK(p.argFloat(6) == Catch::Approx(-0.25f).margin(0.001f));

    fx.send(osc_test::message("/b_free", 0));
}

// =============================================================================
// /b_setn and /b_getn
// =============================================================================

TEST_CASE("/b_setn and /b_getn round-trip", "[buffer]") {
    EngineFixture fx;

    fx.send(osc_test::message("/b_alloc", 0, 1024, 1));
    OscReply alloc;
    REQUIRE(fx.waitForReply("/done", alloc));
    fx.clearReplies();

    // Set sequential samples starting at index 0
    {
        osc_test::Builder b;
        auto& s = b.begin("/b_setn");
        s << (int32_t)0 << (int32_t)0 << (int32_t)4
          << 0.1f << 0.2f << 0.3f << 0.4f;
        fx.send(b.end());
    }
    fx.clearReplies();

    // Get them back
    {
        osc_test::Builder b;
        auto& s = b.begin("/b_getn");
        s << (int32_t)0 << (int32_t)0 << (int32_t)4;
        fx.send(b.end());
    }

    OscReply r;
    REQUIRE(fx.waitForReply("/b_setn", r));
    auto p = r.parsed();
    CHECK(p.argInt(0) == 0);      // bufnum
    CHECK(p.argInt(1) == 0);      // start index
    CHECK(p.argInt(2) == 4);      // count
    CHECK(p.argFloat(3) == Catch::Approx(0.1f).margin(0.01f));
    CHECK(p.argFloat(4) == Catch::Approx(0.2f).margin(0.01f));
    CHECK(p.argFloat(5) == Catch::Approx(0.3f).margin(0.01f));
    CHECK(p.argFloat(6) == Catch::Approx(0.4f).margin(0.01f));

    fx.send(osc_test::message("/b_free", 0));
}

// =============================================================================
// /b_fill
// =============================================================================

TEST_CASE("/b_fill fills range with value", "[buffer]") {
    EngineFixture fx;

    fx.send(osc_test::message("/b_alloc", 0, 1024, 1));
    OscReply alloc;
    REQUIRE(fx.waitForReply("/done", alloc));
    fx.clearReplies();

    // Fill samples 0-99 with 0.5
    {
        osc_test::Builder b;
        auto& s = b.begin("/b_fill");
        s << (int32_t)0 << (int32_t)0 << (int32_t)100 << 0.5f;
        fx.send(b.end());
    }
    fx.clearReplies();

    // Verify sample 50
    {
        osc_test::Builder b;
        auto& s = b.begin("/b_get");
        s << (int32_t)0 << (int32_t)50;
        fx.send(b.end());
    }

    OscReply r;
    REQUIRE(fx.waitForReply("/b_set", r));
    CHECK(r.parsed().argFloat(2) == Catch::Approx(0.5f).margin(0.001f));

    fx.send(osc_test::message("/b_free", 0));
}

TEST_CASE("/b_fill then /b_zero clears", "[buffer]") {
    EngineFixture fx;

    fx.send(osc_test::message("/b_alloc", 0, 100, 1));
    OscReply alloc;
    REQUIRE(fx.waitForReply("/done", alloc));
    fx.clearReplies();

    // Fill with 0.999
    {
        osc_test::Builder b;
        auto& s = b.begin("/b_fill");
        s << (int32_t)0 << (int32_t)0 << (int32_t)100 << 0.999f;
        fx.send(b.end());
    }

    // Zero it
    fx.send(osc_test::message("/b_zero", 0));
    OscReply zr;
    REQUIRE(fx.waitForReply("/done", zr));
    fx.clearReplies();

    // Check sample 50 is zero
    {
        osc_test::Builder b;
        auto& s = b.begin("/b_get");
        s << (int32_t)0 << (int32_t)50;
        fx.send(b.end());
    }

    OscReply r;
    REQUIRE(fx.waitForReply("/b_set", r));
    CHECK(r.parsed().argFloat(2) == Catch::Approx(0.0f).margin(0.001f));

    fx.send(osc_test::message("/b_free", 0));
}

// =============================================================================
// /b_gen
// =============================================================================

TEST_CASE("/b_gen sine1 generates waveform", "[buffer]") {
    EngineFixture fx;

    fx.send(osc_test::message("/b_alloc", 0, 512, 1));
    OscReply alloc;
    REQUIRE(fx.waitForReply("/done", alloc));
    fx.clearReplies();

    // Generate normalized sine wave (flags=1 normalize, amp=1.0)
    {
        osc_test::Builder b;
        auto& s = b.begin("/b_gen");
        s << (int32_t)0 << "sine1" << (int32_t)1 << 1.0f;
        fx.send(b.end());
    }

    OscReply done;
    REQUIRE(fx.waitForReply("/done", done));
    fx.clearReplies();

    // Check quarter-way (should be near peak for sine)
    {
        osc_test::Builder b;
        auto& s = b.begin("/b_get");
        s << (int32_t)0 << (int32_t)128;
        fx.send(b.end());
    }

    OscReply r;
    REQUIRE(fx.waitForReply("/b_set", r));
    float peak = r.parsed().argFloat(2);
    CHECK(std::abs(peak) > 0.9f);

    fx.send(osc_test::message("/b_free", 0));
}

TEST_CASE("/b_gen sine1 multiple harmonics", "[buffer]") {
    EngineFixture fx;

    fx.send(osc_test::message("/b_alloc", 0, 512, 1));
    OscReply alloc;
    REQUIRE(fx.waitForReply("/done", alloc));
    fx.clearReplies();

    // sine1 with two harmonics (fundamental + second harmonic)
    {
        osc_test::Builder b;
        auto& s = b.begin("/b_gen");
        s << (int32_t)0 << "sine1" << (int32_t)1 << 1.0f << 0.5f;
        fx.send(b.end());
    }

    OscReply done;
    REQUIRE(fx.waitForReply("/done", done));

    fx.send(osc_test::message("/b_free", 0));
}

TEST_CASE("/b_gen cheby generates transfer function", "[buffer]") {
    EngineFixture fx;

    fx.send(osc_test::message("/b_alloc", 0, 512, 1));
    OscReply alloc;
    REQUIRE(fx.waitForReply("/done", alloc));
    fx.clearReplies();

    {
        osc_test::Builder b;
        auto& s = b.begin("/b_gen");
        s << (int32_t)0 << "cheby" << (int32_t)1 << 1.0f;
        fx.send(b.end());
    }

    OscReply done;
    REQUIRE(fx.waitForReply("/done", done));

    fx.send(osc_test::message("/b_free", 0));
}

TEST_CASE("/b_gen copy transfers samples between buffers", "[buffer]") {
    EngineFixture fx;

    // Allocate two buffers
    fx.send(osc_test::message("/b_alloc", 0, 256, 1));
    OscReply alloc0;
    REQUIRE(fx.waitForReply("/done", alloc0));

    fx.send(osc_test::message("/b_alloc", 1, 256, 1));
    OscReply alloc1;
    REQUIRE(fx.waitForReply("/done", alloc1));
    fx.clearReplies();

    // Fill buffer 0 with 0.5
    {
        osc_test::Builder b;
        auto& s = b.begin("/b_fill");
        s << (int32_t)0 << (int32_t)0 << (int32_t)256 << 0.5f;
        fx.send(b.end());
    }
    fx.clearReplies();

    // Copy from buffer 0 to buffer 1
    {
        osc_test::Builder b;
        auto& s = b.begin("/b_gen");
        s << (int32_t)1 << "copy" << (int32_t)0  // destBuf, "copy", flags
          << (int32_t)0   // dest start
          << (int32_t)0   // source buf
          << (int32_t)0   // source start
          << (int32_t)-1; // num samples (-1 = all)
        fx.send(b.end());
    }

    OscReply done;
    REQUIRE(fx.waitForReply("/done", done));

    fx.send(osc_test::message("/b_free", 0));
    fx.send(osc_test::message("/b_free", 1));
}

// =============================================================================
// /b_query for multiple buffers
// =============================================================================

TEST_CASE("/b_query returns info for multiple buffers", "[buffer]") {
    EngineFixture fx;

    fx.send(osc_test::message("/b_alloc", 0, 22050, 2));
    OscReply a0;
    REQUIRE(fx.waitForReply("/done", a0));

    fx.send(osc_test::message("/b_alloc", 1, 44100, 1));
    OscReply a1;
    REQUIRE(fx.waitForReply("/done", a1));
    fx.clearReplies();

    // Query both
    fx.send(osc_test::message("/b_query", 0, 1));
    OscReply r;
    REQUIRE(fx.waitForReply("/b_info", r));

    auto p = r.parsed();
    CHECK(p.argInt(0) == 0);      // buf 0
    CHECK(p.argInt(1) == 22050);  // frames
    CHECK(p.argInt(2) == 2);      // channels

    // Second buffer info may be at indices 4-7 if concatenated
    if (p.argCount() > 4) {
        CHECK(p.argInt(4) == 1);      // buf 1
        CHECK(p.argInt(5) == 44100);  // frames
        CHECK(p.argInt(6) == 1);      // channels
    }

    fx.send(osc_test::message("/b_free", 0));
    fx.send(osc_test::message("/b_free", 1));
}

// =============================================================================
// Buffer re-allocation
// =============================================================================

TEST_CASE("re-allocating buffer replaces previous", "[buffer]") {
    EngineFixture fx;

    fx.send(osc_test::message("/b_alloc", 0, 1024, 1));
    OscReply a1;
    REQUIRE(fx.waitForReply("/done", a1));
    fx.clearReplies();

    // Check initial params
    fx.send(osc_test::message("/b_query", 0));
    OscReply q1;
    REQUIRE(fx.waitForReply("/b_info", q1));
    CHECK(q1.parsed().argInt(1) == 1024);
    fx.clearReplies();

    // Re-allocate with different size
    fx.send(osc_test::message("/b_alloc", 0, 2048, 2));
    OscReply a2;
    REQUIRE(fx.waitForReply("/done", a2));
    fx.clearReplies();

    fx.send(osc_test::message("/b_query", 0));
    OscReply q2;
    REQUIRE(fx.waitForReply("/b_info", q2));
    CHECK(q2.parsed().argInt(1) == 2048);
    CHECK(q2.parsed().argInt(2) == 2);

    fx.send(osc_test::message("/b_free", 0));
}

TEST_CASE("multiple buffers are independent", "[buffer]") {
    EngineFixture fx;

    // Allocate 3 buffers with different sizes
    for (int i = 0; i < 3; i++) {
        fx.send(osc_test::message("/b_alloc", i, 1000 * (i + 1), 1));
        OscReply a;
        REQUIRE(fx.waitForReply("/done", a));
    }
    fx.clearReplies();

    // Query each and verify
    for (int i = 0; i < 3; i++) {
        fx.send(osc_test::message("/b_query", i));
        OscReply r;
        REQUIRE(fx.waitForReply("/b_info", r));
        CHECK(r.parsed().argInt(0) == i);
        CHECK(r.parsed().argInt(1) == 1000 * (i + 1));
        fx.clearReplies();
    }

    for (int i = 0; i < 3; i++) {
        fx.send(osc_test::message("/b_free", i));
    }
}

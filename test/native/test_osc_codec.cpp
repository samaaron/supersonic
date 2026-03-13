/*  test_osc_codec.cpp — Catch2 round-trip tests for OscTestUtils encode/decode
    MIT License
*/
#include "OscTestUtils.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

// ---------------------------------------------------------------------------
// 1. message() with no args produces valid OSC with correct address
// ---------------------------------------------------------------------------
TEST_CASE("message with no args produces valid OSC", "[osc_codec]") {
    auto pkt = osc_test::message("/ping");
    REQUIRE(pkt.size() > 0);

    auto addr = osc_test::parseAddress(pkt.ptr(), pkt.size());
    CHECK(addr == "/ping");

    auto parsed = osc_test::parseReply(pkt.ptr(), pkt.size());
    CHECK(parsed.address == "/ping");
    CHECK(parsed.argCount() == 0);
}

// ---------------------------------------------------------------------------
// 2. message() with one int arg round-trips correctly
// ---------------------------------------------------------------------------
TEST_CASE("message with int arg round-trips", "[osc_codec]") {
    auto pkt = osc_test::message("/test", 42);
    auto parsed = osc_test::parseReply(pkt.ptr(), pkt.size());
    CHECK(parsed.address == "/test");
    CHECK(parsed.argCount() == 1);
    CHECK(parsed.argInt(0) == 42);
}

// ---------------------------------------------------------------------------
// 3. message() with two int args round-trips
// ---------------------------------------------------------------------------
TEST_CASE("message with two int args round-trips", "[osc_codec]") {
    auto pkt = osc_test::message("/pair", (int32_t)100, (int32_t)-7);
    auto parsed = osc_test::parseReply(pkt.ptr(), pkt.size());
    CHECK(parsed.address == "/pair");
    CHECK(parsed.argCount() == 2);
    CHECK(parsed.argInt(0) == 100);
    CHECK(parsed.argInt(1) == -7);
}

// ---------------------------------------------------------------------------
// 4. message() with three int args round-trips
// ---------------------------------------------------------------------------
TEST_CASE("message with three int args round-trips", "[osc_codec]") {
    auto pkt = osc_test::message("/triple", (int32_t)1, (int32_t)2, (int32_t)3);
    auto parsed = osc_test::parseReply(pkt.ptr(), pkt.size());
    CHECK(parsed.address == "/triple");
    CHECK(parsed.argCount() == 3);
    CHECK(parsed.argInt(0) == 1);
    CHECK(parsed.argInt(1) == 2);
    CHECK(parsed.argInt(2) == 3);
}

// ---------------------------------------------------------------------------
// 5. message() with string arg round-trips
// ---------------------------------------------------------------------------
TEST_CASE("message with string arg round-trips", "[osc_codec]") {
    auto pkt = osc_test::message("/greet", "hello world");
    auto parsed = osc_test::parseReply(pkt.ptr(), pkt.size());
    CHECK(parsed.address == "/greet");
    CHECK(parsed.argCount() == 1);
    CHECK(parsed.argString(0) == "hello world");
}

// ---------------------------------------------------------------------------
// 6. messageWithBlob() produces valid packet
// ---------------------------------------------------------------------------
TEST_CASE("messageWithBlob produces valid packet", "[osc_codec]") {
    const uint8_t blob[] = {0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02};
    auto pkt = osc_test::messageWithBlob("/blob", blob, sizeof(blob));
    REQUIRE(pkt.size() > 0);

    auto addr = osc_test::parseAddress(pkt.ptr(), pkt.size());
    CHECK(addr == "/blob");
}

// ---------------------------------------------------------------------------
// 7. Builder with mixed types (string + int + float) round-trips
// ---------------------------------------------------------------------------
TEST_CASE("Builder with mixed types round-trips", "[osc_codec]") {
    osc_test::Builder b;
    auto& s = b.begin("/mixed");
    s << "hello" << (int32_t)99 << 3.14f;
    auto pkt = b.end();

    auto parsed = osc_test::parseReply(pkt.ptr(), pkt.size());
    CHECK(parsed.address == "/mixed");
    CHECK(parsed.argCount() == 3);
    CHECK(parsed.argString(0) == "hello");
    CHECK(parsed.argInt(1) == 99);
    CHECK(parsed.argFloat(2) == Catch::Approx(3.14f).margin(1e-6f));
}

// ---------------------------------------------------------------------------
// 8. parseAddress extracts correct address pattern
// ---------------------------------------------------------------------------
TEST_CASE("parseAddress extracts correct address pattern", "[osc_codec]") {
    auto pkt = osc_test::message("/s_new", (int32_t)1000);
    auto addr = osc_test::parseAddress(pkt.ptr(), pkt.size());
    CHECK(addr == "/s_new");
}

// ---------------------------------------------------------------------------
// 9. ParsedReply.argCount() returns correct count
// ---------------------------------------------------------------------------
TEST_CASE("ParsedReply argCount returns correct count", "[osc_codec]") {
    SECTION("zero args") {
        auto pkt = osc_test::message("/none");
        auto parsed = osc_test::parseReply(pkt.ptr(), pkt.size());
        CHECK(parsed.argCount() == 0);
    }

    SECTION("one arg") {
        auto pkt = osc_test::message("/one", (int32_t)5);
        auto parsed = osc_test::parseReply(pkt.ptr(), pkt.size());
        CHECK(parsed.argCount() == 1);
    }

    SECTION("three args") {
        auto pkt = osc_test::message("/three", (int32_t)1, (int32_t)2, (int32_t)3);
        auto parsed = osc_test::parseReply(pkt.ptr(), pkt.size());
        CHECK(parsed.argCount() == 3);
    }
}

// ---------------------------------------------------------------------------
// 10. Out-of-range argInt / argFloat / argString return defaults
// ---------------------------------------------------------------------------
TEST_CASE("out-of-range arg accessors return defaults", "[osc_codec]") {
    auto pkt = osc_test::message("/single", (int32_t)7);
    auto parsed = osc_test::parseReply(pkt.ptr(), pkt.size());

    // Index 0 is valid — index 1 and beyond are out of range
    CHECK(parsed.argInt(1) == 0);
    CHECK(parsed.argFloat(1) == 0.0f);
    CHECK(parsed.argString(1) == "");

    // Large index
    CHECK(parsed.argInt(999) == 0);
    CHECK(parsed.argFloat(999) == 0.0f);
    CHECK(parsed.argString(999) == "");
}

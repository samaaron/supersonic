// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Sam Aaron
/*
 * test_osc_bundle_elements.cpp - the elements of a bundle as the engine reads
 * them. OSC gives every element a size that is a multiple of 4, so each element
 * starts on a 4-byte boundary and the command handlers read its words in place.
 * An element of any other size ends the bundle there.
 */
#include "EngineFixture.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

void appendWord(std::vector<uint8_t>& bytes, uint32_t word) {
    for (int k = 3; k >= 0; --k) bytes.push_back(uint8_t(word >> (8 * k)));
}

// /c_set bus value: 20 bytes.
std::vector<uint8_t> controlSet(int32_t bus, float value) {
    std::vector<uint8_t> m;
    for (const char c : std::string("/c_set\0\0,if\0", 12)) m.push_back(uint8_t(c));
    appendWord(m, uint32_t(bus));
    uint32_t bits;
    std::memcpy(&bits, &value, 4);
    appendWord(m, bits);
    return m;
}

// An immediate bundle of `elements`, each led by its size.
std::vector<uint8_t> bundle(const std::vector<std::vector<uint8_t>>& elements) {
    std::vector<uint8_t> b;
    for (const char c : std::string("#bundle\0", 8)) b.push_back(uint8_t(c));
    appendWord(b, 0);
    appendWord(b, 1);
    for (const auto& e : elements) {
        appendWord(b, uint32_t(e.size()));
        b.insert(b.end(), e.begin(), e.end());
    }
    return b;
}

} // namespace

TEST_CASE("OSC bundles: an element whose size is not a multiple of 4 ends the bundle", "[osc_bundle]") {
    EngineFixture fx;

    // The middle element carries one byte past its message, so the element
    // after it would start one byte past a 4-byte boundary.
    auto padded = controlSet(1, 2.0f);
    padded.push_back(0);
    const auto b = bundle({controlSet(0, 1.0f), padded, controlSet(2, 3.0f)});
    fx.send(b.data(), uint32_t(b.size()));

    fx.clearReplies();
    fx.send(osc_test::message("/c_getn", 0, 3));
    OscReply r;
    REQUIRE(fx.waitForReply("/c_setn", r));
    const auto parsed = r.parsed();
    REQUIRE(parsed.argCount() == 5);
    CHECK(parsed.argFloat(2) == 1.0f);
    CHECK(parsed.argFloat(3) == 0.0f);
    CHECK(parsed.argFloat(4) == 0.0f);
}

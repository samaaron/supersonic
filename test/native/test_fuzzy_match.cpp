/*
 * test_fuzzy_match.cpp — Tests for -H multi-token fuzzy device matching
 */
#include <catch2/catch_test_macros.hpp>
#include "FuzzyMatch.h"

// Realistic device list matching a Windows machine with multiple drivers
static const std::vector<std::string> kDevices = {
    "Windows Audio : Speakers (Qualcomm Aqstic Audio Adapter Device)",
    "Windows Audio (Exclusive Mode) : Speakers (Qualcomm Aqstic Audio Adapter Device)",
    "Windows Audio (Low Latency Mode) : Speakers (Qualcomm Aqstic Audio Adapter Device)",
    "DirectSound : Primary Sound Driver",
    "DirectSound : Speakers (Qualcomm Aqstic Audio Adapter Device)",
    "DirectSound : Headphones (USB Audio Device)",
    "ASIO : Focusrite USB ASIO",
};

TEST_CASE("fuzzyMatch: empty pattern returns empty", "[fuzzy]") {
    CHECK(fuzzyMatch("", kDevices).empty());
}

TEST_CASE("fuzzyMatch: whitespace-only pattern returns empty", "[fuzzy]") {
    CHECK(fuzzyMatch("   ", kDevices).empty());
}

TEST_CASE("fuzzyMatch: empty candidates returns empty", "[fuzzy]") {
    CHECK(fuzzyMatch("speakers", {}).empty());
}

TEST_CASE("fuzzyMatch: no match returns empty", "[fuzzy]") {
    CHECK(fuzzyMatch("bluetooth", kDevices).empty());
}

TEST_CASE("fuzzyMatch: single token matches driver name", "[fuzzy]") {
    // "asio" should match the ASIO entry (shortest containing "asio")
    CHECK(fuzzyMatch("asio", kDevices) == "ASIO : Focusrite USB ASIO");
}

TEST_CASE("fuzzyMatch: single token matches device name", "[fuzzy]") {
    // "headphones" only appears in one entry
    CHECK(fuzzyMatch("headphones", kDevices) == "DirectSound : Headphones (USB Audio Device)");
}

TEST_CASE("fuzzyMatch: single token matching multiple prefers shortest", "[fuzzy]") {
    // "primary" only matches one entry
    CHECK(fuzzyMatch("primary", kDevices) == "DirectSound : Primary Sound Driver");
}

TEST_CASE("fuzzyMatch: two tokens narrow driver + device", "[fuzzy]") {
    // "direct headphones" should match DirectSound + Headphones
    CHECK(fuzzyMatch("direct headphones", kDevices) ==
          "DirectSound : Headphones (USB Audio Device)");
}

TEST_CASE("fuzzyMatch: two tokens in reverse order", "[fuzzy]") {
    // Order shouldn't matter
    CHECK(fuzzyMatch("headphones direct", kDevices) ==
          "DirectSound : Headphones (USB Audio Device)");
}

TEST_CASE("fuzzyMatch: case insensitive", "[fuzzy]") {
    CHECK(fuzzyMatch("DIRECTSOUND", kDevices) == "DirectSound : Primary Sound Driver");
    CHECK(fuzzyMatch("Speakers DIRECT", kDevices) ==
          "DirectSound : Speakers (Qualcomm Aqstic Audio Adapter Device)");
}

TEST_CASE("fuzzyMatch: 'exclusive' selects WASAPI exclusive mode", "[fuzzy]") {
    CHECK(fuzzyMatch("exclusive", kDevices) ==
          "Windows Audio (Exclusive Mode) : Speakers (Qualcomm Aqstic Audio Adapter Device)");
}

TEST_CASE("fuzzyMatch: 'low latency' selects low latency mode", "[fuzzy]") {
    CHECK(fuzzyMatch("low latency", kDevices) ==
          "Windows Audio (Low Latency Mode) : Speakers (Qualcomm Aqstic Audio Adapter Device)");
}

TEST_CASE("fuzzyMatch: 'focusrite' matches ASIO device", "[fuzzy]") {
    CHECK(fuzzyMatch("focusrite", kDevices) == "ASIO : Focusrite USB ASIO");
}

TEST_CASE("fuzzyMatch: 'usb' matches shortest USB entry", "[fuzzy]") {
    // Both ASIO and DirectSound Headphones contain "usb"
    // "ASIO : Focusrite USB ASIO" (25 chars) is shorter than
    // "DirectSound : Headphones (USB Audio Device)" (44 chars)
    CHECK(fuzzyMatch("usb", kDevices) == "ASIO : Focusrite USB ASIO");
}

TEST_CASE("fuzzyMatch: 'usb direct' narrows to DirectSound USB device", "[fuzzy]") {
    CHECK(fuzzyMatch("usb direct", kDevices) ==
          "DirectSound : Headphones (USB Audio Device)");
}

TEST_CASE("fuzzyMatch: three tokens work", "[fuzzy]") {
    CHECK(fuzzyMatch("windows exclusive qualcomm", kDevices) ==
          "Windows Audio (Exclusive Mode) : Speakers (Qualcomm Aqstic Audio Adapter Device)");
}

TEST_CASE("fuzzyMatch: partial token match", "[fuzzy]") {
    // "qual" is a substring of "Qualcomm"
    // Multiple entries contain it — shortest wins
    auto result = fuzzyMatch("qual", kDevices);
    CHECK_FALSE(result.empty());
    CHECK(result.find("Qualcomm") != std::string::npos);
}

TEST_CASE("fuzzyMatch: all tokens must match", "[fuzzy]") {
    // "asio headphones" — no single entry has both
    CHECK(fuzzyMatch("asio headphones", kDevices).empty());
}

TEST_CASE("fuzzyMatch: extra whitespace is ignored", "[fuzzy]") {
    CHECK(fuzzyMatch("  direct   headphones  ", kDevices) ==
          "DirectSound : Headphones (USB Audio Device)");
}

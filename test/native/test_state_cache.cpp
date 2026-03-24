/*
 * test_state_cache.cpp — StateCache unit tests
 */
#include <catch2/catch_test_macros.hpp>
#include "StateCache.h"
#include <cstring>
#include <thread>

// Build a minimal v1 SCgf binary with a given name
static std::vector<uint8_t> makeSCgfV1(const std::string& name) {
    std::vector<uint8_t> data;
    // Magic "SCgf"
    data.push_back('S'); data.push_back('C');
    data.push_back('g'); data.push_back('f');
    // Version = 1 (big-endian int32)
    data.push_back(0); data.push_back(0);
    data.push_back(0); data.push_back(1);
    // numDefs = 1 (big-endian int16)
    data.push_back(0); data.push_back(1);
    // Name length + name
    data.push_back(static_cast<uint8_t>(name.size()));
    for (char c : name) data.push_back(static_cast<uint8_t>(c));
    return data;
}

// Build a minimal v2 SCgf binary with a given name
static std::vector<uint8_t> makeSCgfV2(const std::string& name) {
    std::vector<uint8_t> data;
    data.push_back('S'); data.push_back('C');
    data.push_back('g'); data.push_back('f');
    // Version = 2
    data.push_back(0); data.push_back(0);
    data.push_back(0); data.push_back(2);
    // numDefs = 1
    data.push_back(0); data.push_back(1);
    // Name length + name
    data.push_back(static_cast<uint8_t>(name.size()));
    for (char c : name) data.push_back(static_cast<uint8_t>(c));
    return data;
}

// Build a minimal v3 SCgf binary with a given name (has defSize before name)
static std::vector<uint8_t> makeSCgfV3(const std::string& name) {
    std::vector<uint8_t> data;
    data.push_back('S'); data.push_back('C');
    data.push_back('g'); data.push_back('f');
    // Version = 3
    data.push_back(0); data.push_back(0);
    data.push_back(0); data.push_back(3);
    // numDefs = 1
    data.push_back(0); data.push_back(1);
    // defSize = 100 (arbitrary, big-endian int32)
    data.push_back(0); data.push_back(0);
    data.push_back(0); data.push_back(100);
    // Name length + name
    data.push_back(static_cast<uint8_t>(name.size()));
    for (char c : name) data.push_back(static_cast<uint8_t>(c));
    return data;
}

// ── SynthDef cache tests ─────────────────────────────────────────────────────

TEST_CASE("StateCache: synthdef round-trip", "[StateCache]") {
    StateCache cache;
    auto data = makeSCgfV1("beep");

    cache.cacheSynthDef("beep", data);
    REQUIRE(cache.synthDefs().size() == 1);
    REQUIRE(cache.synthDefs().count("beep") == 1);
    REQUIRE(cache.synthDefs().at("beep") == data);

    cache.uncacheSynthDef("beep");
    REQUIRE(cache.synthDefs().empty());
}

TEST_CASE("StateCache: synthdef overwrite", "[StateCache]") {
    StateCache cache;
    auto data1 = makeSCgfV1("beep");
    auto data2 = makeSCgfV2("beep");

    cache.cacheSynthDef("beep", data1);
    cache.cacheSynthDef("beep", data2);
    REQUIRE(cache.synthDefs().size() == 1);
    REQUIRE(cache.synthDefs().at("beep") == data2);
}

TEST_CASE("StateCache: clearSynthDefs", "[StateCache]") {
    StateCache cache;
    cache.cacheSynthDef("beep", makeSCgfV1("beep"));
    cache.cacheSynthDef("saw", makeSCgfV1("saw"));
    REQUIRE(cache.synthDefs().size() == 2);

    cache.clearSynthDefs();
    REQUIRE(cache.synthDefs().empty());
}

// ── Buffer cache tests ──────────────────────────────────────────────────────

TEST_CASE("StateCache: buffer round-trip", "[StateCache]") {
    StateCache cache;

    cache.cacheBuffer({0, "/path/to/sample.wav", 0, 44100, 1, 48000});
    REQUIRE(cache.buffers().size() == 1);
    REQUIRE(cache.buffers()[0].bufnum == 0);
    REQUIRE(cache.buffers()[0].path == "/path/to/sample.wav");

    cache.uncacheBuffer(0);
    REQUIRE(cache.buffers().empty());
}

TEST_CASE("StateCache: buffer overwrite by bufnum", "[StateCache]") {
    StateCache cache;
    cache.cacheBuffer({5, "/old.wav", 0, 0, 0, 0});
    cache.cacheBuffer({5, "/new.wav", 0, 0, 0, 0});
    REQUIRE(cache.buffers().size() == 1);
    REQUIRE(cache.buffers()[0].path == "/new.wav");
}

// ── Module registration tests ────────────────────────────────────────────────

TEST_CASE("StateCache: module captureAll", "[StateCache]") {
    StateCache cache;
    int captureCount = 0;

    cache.registerModule({"test-module",
        [&]() { captureCount++; },
        nullptr
    });

    cache.captureAll();
    REQUIRE(captureCount == 1);
}

TEST_CASE("StateCache: multiple modules captureAll", "[StateCache]") {
    StateCache cache;
    int count = 0;

    cache.registerModule({"mod-a", [&]() { count++; }, nullptr});
    cache.registerModule({"mod-b", [&]() { count++; }, nullptr});

    cache.captureAll();
    REQUIRE(count == 2);
}

// ── SynthDef name extraction tests ───────────────────────────────────────────

TEST_CASE("StateCache: extractSynthDefName v1", "[StateCache]") {
    auto data = makeSCgfV1("sonic-pi-beep");
    auto name = StateCache::extractSynthDefName(data.data(), data.size());
    REQUIRE(name == "sonic-pi-beep");
}

TEST_CASE("StateCache: extractSynthDefName v2", "[StateCache]") {
    auto data = makeSCgfV2("my-synth");
    auto name = StateCache::extractSynthDefName(data.data(), data.size());
    REQUIRE(name == "my-synth");
}

TEST_CASE("StateCache: extractSynthDefName v3", "[StateCache]") {
    auto data = makeSCgfV3("v3-synth");
    auto name = StateCache::extractSynthDefName(data.data(), data.size());
    REQUIRE(name == "v3-synth");
}

TEST_CASE("StateCache: extractSynthDefName invalid data", "[StateCache]") {
    // Too short
    REQUIRE(StateCache::extractSynthDefName(nullptr, 0).empty());

    // Wrong magic
    uint8_t bad[] = {'N','O','P','E', 0,0,0,1, 0,1, 4, 'a','b','c','d'};
    REQUIRE(StateCache::extractSynthDefName(bad, sizeof(bad)).empty());

    // Valid magic but truncated before name
    uint8_t trunc[] = {'S','C','g','f', 0,0,0,1, 0,1};
    REQUIRE(StateCache::extractSynthDefName(trunc, sizeof(trunc)).empty());
}

// ── Concurrent access test ──────────────────────────────────────────────────

TEST_CASE("StateCache: concurrent cacheSynthDef and synthDefs", "[StateCache]") {
    StateCache cache;
    constexpr int kIterations = 1000;

    // Writer thread: cacheSynthDef in a tight loop
    std::thread writer([&]() {
        for (int i = 0; i < kIterations; i++) {
            auto name = "synth-" + std::to_string(i % 10);
            cache.cacheSynthDef(name, makeSCgfV1(name));
        }
    });

    // Reader thread: synthDefs() returns by value — should never see torn data
    std::thread reader([&]() {
        for (int i = 0; i < kIterations; i++) {
            auto defs = cache.synthDefs();
            // Just iterate — under TSan this would catch races
            for (auto& [name, data] : defs) {
                (void)name;
                (void)data.size();
            }
        }
    });

    writer.join();
    reader.join();

    // After both threads finish, cache should have at most 10 entries
    REQUIRE(cache.synthDefs().size() <= 10);
}

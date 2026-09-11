/*
 * test_guest_config_layout.cpp — guest config block memory layout tests
 *
 * Validates that:
 *   - the block size the engine chooses follows the driver's buffer
 *   - after a cold swap, the engine reports the correct sample rate
 *
 * The block's slots are named by src/native/GuestConfigBlock.h — the layout
 * THIS host and its guest agree on.
 *
 * Where the region SITS, and that it survives traffic and a rebuild, is
 * clockwork's guarantee about its own arena and is asserted in clockwork's
 * suite (test_dsp_regions.cpp).
 */
#include <catch2/catch_test_macros.hpp>
#include "EngineFixture.h"
#include "OscBuilder.h"
#include "shared_memory.h"
#include "audio_processor.h"
#include "audio_config.h"
#include "native/GuestConfigBlock.h"
#include <cstring>

// ── Layout validation ───────────────────────────────────────────────────────

TEST_CASE("BlockSize: world block matches a smaller driver buffer",
          "[Layout][ChooseBlockSize]") {
    auto cfg = EngineFixture::defaultConfig();
    cfg.bufferSize = 32;
    cfg.blockSize  = 0;   // no explicit -z: policy decides
    EngineFixture fix(cfg);
    REQUIRE(get_audio_buffer_samples() == 32);
}

TEST_CASE("BlockSize: larger driver buffer keeps the default block",
          "[Layout][ChooseBlockSize]") {
    auto cfg = EngineFixture::defaultConfig();
    cfg.bufferSize = 512;
    cfg.blockSize  = 0;
    EngineFixture fix(cfg);
    REQUIRE(get_audio_buffer_samples() == clockwork::kDefaultBlockSize);
}

TEST_CASE("BlockSize: explicit -z overrides the driver buffer",
          "[Layout][ChooseBlockSize]") {
    auto cfg = EngineFixture::defaultConfig();
    cfg.bufferSize = 512;
    cfg.blockSize  = 64;
    EngineFixture fix(cfg);
    REQUIRE(get_audio_buffer_samples() == 64);
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

    // Engine still responds
    fix.clearReplies();
    fix.send(osc_test::message("/status"));
    REQUIRE(fix.waitForReply("/status.reply", reply));
}

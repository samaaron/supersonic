/*
 * test_cold_swap_scope.cpp — Cold swap with active scope buffers
 *
 * Reproduces crash: SIGSEGV in getScopeBuffer during ScopeOut2_Ctor
 * after a cold swap (rate change) while scope synth is running.
 */
#include <catch2/catch_test_macros.hpp>
#include "EngineFixture.h"
#include "OscBuilder.h"
#include "OscTestUtils.h"
#include <filesystem>

static SupersonicEngine::Config scopeConfig() {
    SupersonicEngine::Config cfg;
    cfg.sampleRate    = 48000;
    cfg.bufferSize    = 128;
    cfg.udpPort       = 57199;  // Non-zero enables shared memory (needed for scope buffers)
    cfg.numBuffers    = 1024;
    cfg.maxNodes      = 1024;
    cfg.maxGraphDefs  = 512;
    cfg.maxWireBufs   = 64;
    cfg.headless      = true;
    return cfg;
}

TEST_CASE("ColdSwap: rate change with active scope synth", "[ColdSwap][Scope]") {
    EngineFixture fix(scopeConfig());

    OscReply reply;
    fix.send(osc_test::message("/status"));
    REQUIRE(fix.waitForReply("/status.reply", reply));

    // Register for node notifications (required for /n_go)
    fix.send(osc_test::message("/notify", 1));
    REQUIRE(fix.waitForReply("/done", reply));
    fix.clearReplies();

    std::string defPath = std::string(SUPERSONIC_SYNTHDEFS_DIR) + "/sonic-pi-scope.scsyndef";
    if (!std::filesystem::exists(defPath)) {
        SKIP("sonic-pi-scope synthdef not available");
    }
    REQUIRE(fix.loadSynthDef("sonic-pi-scope"));

    // Trigger scope synth
    {
        osc_test::Builder b;
        auto& s = b.begin("/s_new");
        s << "sonic-pi-scope" << (int32_t)1000 << (int32_t)0 << (int32_t)0
          << "max_frames" << 1024.0f;
        fix.send(b.end());
    }

    REQUIRE(fix.waitForReply("/n_go", reply, 3000));

    // Cold swap — this previously caused SIGSEGV in getScopeBuffer
    auto result = fix.engine().switchDevice("", 44100);
    REQUIRE(result.success);

    fix.clearReplies();
    fix.send(osc_test::message("/status"));
    REQUIRE(fix.waitForReply("/status.reply", reply));
}

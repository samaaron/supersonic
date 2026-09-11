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
#include "shm_segment.hpp"
#include <thread>
#include <chrono>
#include <filesystem>

static ClockworkEngine::Config scopeConfig() {
    ClockworkEngine::Config cfg;
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

using detail_shm_segment::shm_segment_client;

// A scope reader must see data again after a cold swap. The old crash test
// (below) proved the swap does not SIGSEGV; this proves the panel is not left
// dead. The World rebuild kills every node, so the scope synth is re-created
// afterwards — the spider's cold_swap_reinit Phase 5 (start_scope) — and the
// slot the reader watches must go live again.
TEST_CASE("ColdSwap: scope data resumes after a cold swap and re-spawn",
          "[ColdSwap][Scope]") {
    std::string defPath = std::string(CLOCKWORK_SAMPLES_DIR).empty() ? "" : "";
    std::string synthPath = std::string(CLOCKWORK_SYNTHDEFS_DIR) + "/sonic-pi-scope.scsyndef";
    if (!std::filesystem::exists(synthPath)) SKIP("sonic-pi-scope synthdef not available");

    EngineFixture fix(scopeConfig());
    OscReply reply;
    fix.send(osc_test::message("/notify", 1));
    REQUIRE(fix.waitForReply("/done", reply));
    REQUIRE(fix.loadSynthDef("sonic-pi-scope"));

    constexpr float kSlot = 0.0f;   // Sonic Pi's scope watches slot 0
    auto spawnScope = [&](int32_t nodeId) {
        osc_test::Builder b;
        auto& s = b.begin("/s_new");
        s << "sonic-pi-scope" << nodeId << (int32_t)0 << (int32_t)0
          << "scope_num" << kSlot << "max_frames" << 1024.0f;
        fix.send(b.end());
    };
    auto advances = [](shm_scope_stream_reader& r) {
        const uint64_t before = r.write_position();
        for (int i = 0; i < 60; ++i) {
            if (r.write_position() > before) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return false;
    };

    // Before the swap: the slot is live and its cursor advances.
    fix.clearReplies();
    spawnScope(1000);
    REQUIRE(fix.waitForReply("/n_go", reply, 3000));
    {
        shm_segment_client client(detail_shm_segment::shm_dup_handle(fix.engine().shmNativeHandle()));
        auto reader = client.get_scope_stream_reader((unsigned)kSlot);
        REQUIRE(fix.waitForBlocks(20, 3000));
        REQUIRE(reader.valid());
        CHECK(advances(reader));
    }

    // Cold swap: the World is rebuilt, every node (the scope synth included)
    // is gone.
    auto result = fix.engine().switchDevice("", 44100);
    REQUIRE(result.success);

    // The rebuild drops the loaded synthdefs; the spider reloads them in
    // Phase 2 of its cold-swap reinit before re-creating anything.
    REQUIRE(fix.loadSynthDef("sonic-pi-scope"));

    // Re-create the scope synth, as the spider does in Phase 5.
    fix.clearReplies();
    spawnScope(1001);
    REQUIRE(fix.waitForReply("/n_go", reply, 3000));

    // Re-open the segment (as the GUI's ResetConnection does: a fresh
    // attach, here a fresh duplicate of the engine's handle) and require the
    // slot to be live and advancing again. THIS is the scopes-work-after-swap
    // property; if it fails, the scope panel stays dead after a device change.
    {
        shm_segment_client client(detail_shm_segment::shm_dup_handle(fix.engine().shmNativeHandle()));
        auto reader = client.get_scope_stream_reader((unsigned)kSlot);
        REQUIRE(fix.waitForBlocks(20, 3000));
        CHECK(reader.valid());
        CHECK(advances(reader));
    }
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

    std::string defPath = std::string(CLOCKWORK_SYNTHDEFS_DIR) + "/sonic-pi-scope.scsyndef";
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

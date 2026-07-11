/*
 * test_scope_slot_ownership.cpp — a late ScopeOut2 dtor must not stomp a
 * slot that a newer unit has since re-claimed.
 *
 * Field failure (Sonic Pi, 2026-07-11): re-running a buffer re-registers a
 * live loop's scope tap on the same slot number. The old run's scope_out
 * node tears down *late* when it sits under FX with kill-delay tails, so
 * the ordering on the server is:
 *
 *   Ctor(new unit, slot N)   — new run claims the slot, state=1
 *   Dtor(old unit, slot N)   — late teardown, releaseScopeBuffer → state=0
 *
 * leaving the new writer alive but the slot marked free: the GUI reader's
 * valid() gate fails forever (scope greys out) while audio plays on.
 * Release must be ownership-checked: only the unit that most recently
 * initialised the slot may free it.
 */
#include <catch2/catch_test_macros.hpp>
#include "EngineFixture.h"
#include "OscBuilder.h"
#include "OscTestUtils.h"

#include "src/synth/common/server_shm.hpp"

#include <chrono>
#include <filesystem>
#include <thread>

using detail_server_shm::server_shared_memory_client;

namespace {

SupersonicEngine::Config scopeOwnershipConfig() {
    SupersonicEngine::Config cfg;
    cfg.sampleRate        = 48000;
    cfg.bufferSize        = 128;
    cfg.udpPort           = 57210;  // non-zero enables shared memory
    cfg.numBuffers        = 64;
    cfg.maxNodes          = 256;
    cfg.maxGraphDefs      = 64;
    cfg.maxWireBufs       = 64;
    cfg.headless          = true;
    cfg.numOutputChannels = 2;
    cfg.numInputChannels  = 0;
    return cfg;
}

void spawnScopeSynth(EngineFixture& fix, int32_t nodeId, float scopeNum) {
    osc_test::Builder b;
    auto& s = b.begin("/s_new");
    s << "sonic-pi-scope" << nodeId << (int32_t)0 << (int32_t)0
      << "scope_num" << scopeNum << "max_frames" << 1024.0f;
    fix.send(b.end());
}

}  // namespace

TEST_CASE("scope-ownership: late dtor of a superseded unit leaves the slot active",
          "[scope][shm][security]") {
    std::string defPath = std::string(SUPERSONIC_SYNTHDEFS_DIR) + "/sonic-pi-scope.scsyndef";
    if (!std::filesystem::exists(defPath)) {
        SKIP("sonic-pi-scope synthdef not available");
    }

    EngineFixture fix(scopeOwnershipConfig());
    OscReply reply;
    fix.send(osc_test::message("/notify", 1));
    REQUIRE(fix.waitForReply("/done", reply));
    REQUIRE(fix.loadSynthDef("sonic-pi-scope"));

    constexpr float kSlot = 2.0f;

    // Old run's writer claims slot 2.
    fix.clearReplies();
    spawnScopeSynth(fix, 1000, kSlot);
    REQUIRE(fix.waitForReply("/n_go", reply, 3000));

    // Re-run: a new writer re-claims the same slot while the old one lives.
    fix.clearReplies();
    spawnScopeSynth(fix, 1001, kSlot);
    REQUIRE(fix.waitForReply("/n_go", reply, 3000));

    // Late teardown of the old run's node.
    fix.clearReplies();
    fix.send(osc_test::message("/n_free", (int32_t)1000));
    REQUIRE(fix.waitForReply("/n_end", reply, 3000));

    // The surviving writer's slot must still be live for readers: state == 1
    // and the stage still advancing as blocks render.
    server_shared_memory_client client(scopeOwnershipConfig().udpPort);
    auto reader = client.get_scope_buffer_reader((unsigned)kSlot);
    REQUIRE(fix.waitForBlocks(40, 3000));
    CHECK(reader.valid());
    unsigned frames = 0;
    bool advanced = false;
    for (int i = 0; i < 50 && !advanced; ++i) {
        advanced = reader.pull(frames);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(advanced);

    // Owner release still frees the slot.
    fix.clearReplies();
    fix.send(osc_test::message("/n_free", (int32_t)1001));
    REQUIRE(fix.waitForReply("/n_end", reply, 3000));
    REQUIRE(fix.waitForBlocks(10, 2000));
    CHECK(!reader.valid());
}

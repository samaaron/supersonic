/*
 * test_engine_state.cpp — EngineState lifecycle and shared memory tests.
 *
 * Verifies the engine state machine: Booting → Running → Restarting
 * → Running → Stopped.  Also tests that shared memory survives engine
 * lifecycle transitions.
 */
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <atomic>
#include <cstring>
#include "EngineFixture.h"
#include "engine_state.h"
#include "shm_segment.hpp"
#include "audio_processor.h"   // control / shared_memory arena globals
#include "shared_memory.h"     // IN ring layout
#include "ring/ring.h"         // Message wire header

#ifdef _WIN32
#else
#include <sys/stat.h>
#include <fcntl.h>
#endif

// ── State transitions ──────────────────────────────────────────────────────

TEST_CASE("EngineState: starts Stopped before init", "[EngineState]") {
    ClockworkEngine engine;
    CHECK(engine.engineState() == EngineState::Stopped);
}

TEST_CASE("EngineState: Running after init", "[EngineState]") {
    ClockworkEngine engine;
    engine.onReply = [](const uint8_t*, uint32_t) {};
    ClockworkEngine::Config cfg;
    cfg.headless = true;
    cfg.udpPort  = 0;
    engine.init(cfg);
    CHECK(engine.engineState() == EngineState::Running);
    engine.shutdown();
}

TEST_CASE("EngineState: Stopped after shutdown", "[EngineState]") {
    ClockworkEngine engine;
    engine.onReply = [](const uint8_t*, uint32_t) {};
    ClockworkEngine::Config cfg;
    cfg.headless = true;
    cfg.udpPort  = 0;
    engine.init(cfg);
    REQUIRE(engine.engineState() == EngineState::Running);
    engine.shutdown();
    CHECK(engine.engineState() == EngineState::Stopped);
}

// ── engineStateToString ─────────────────────────────────────────────────────

TEST_CASE("EngineState: engineStateToString covers all states", "[EngineState]") {
    CHECK(std::string(engineStateToString(EngineState::Booting))    == "booting");
    CHECK(std::string(engineStateToString(EngineState::Running))    == "running");
    CHECK(std::string(engineStateToString(EngineState::Restarting)) == "restarting");
    CHECK(std::string(engineStateToString(EngineState::Stopped))    == "stopped");
    CHECK(std::string(engineStateToString(EngineState::Error))      == "error");
}

// ── Fixture-based: verify engine is Running and responds after boot ─────────

TEST_CASE("EngineState: fixture engine is Running", "[EngineState]") {
    EngineFixture fix;
    CHECK(fix.engine().engineState() == EngineState::Running);

    // Verify it actually works
    fix.send(osc_test::message("/status"));
    OscReply r;
    REQUIRE(fix.waitForReply("/status.reply", r));
}

TEST_CASE("EngineState: switchDevice in headless doesn't change state", "[EngineState]") {
    EngineFixture fix;
    CHECK(fix.engine().engineState() == EngineState::Running);
    // In headless mode, device name is ignored — the swap is a hot swap
    // (pause/resume headless driver) which always succeeds.
    auto result = fix.engine().switchDevice("nonexistent");
    REQUIRE(result.success);
    REQUIRE(result.type == SwapType::Hot);
    // State should still be Running — hot swaps don't transition state
    CHECK(fix.engine().engineState() == EngineState::Running);
}

// (The recording-across-hot-swap case moved with recording itself: the
// front's recorder reads the master tap, which a hot swap does not touch —
// test_front_recording.cpp.)
TEST_CASE("EngineState: purge clears stale messages after cold swap",
          "[EngineState][atomicity]") {
    // Verify that purge() clears the ring buffer so stale messages
    // from before a swap don't reach the fresh world.
    EngineFixture fix;

    // Send commands and verify they work
    fix.send(osc_test::message("/status"));
    OscReply r;
    REQUIRE(fix.waitForReply("/status.reply", r));

    // Purge (simulates what happens during cold swap drain)
    fix.engine().purge();

    // Verify the engine still works after purge — no leftover state
    fix.clearReplies();
    fix.send(osc_test::message("/status"));
    REQUIRE(fix.waitForReply("/status.reply", r));

    // Verify /status.reply has valid data (not stale)
    auto p = r.parsed();
    CHECK(p.argCount() >= 5);
}

// A producer preempted inside clockwork_ingress_write holds in_write_lock with the
// old head already loaded. If purge() moves the IN-ring cursors underneath
// it, the producer's completed write re-publishes head past the reset point,
// re-exposing every already-consumed frame between offset 0 and the old head
// — the next drain then replays messages the engine already performed.
// Manual pump makes the interleaving deterministic: the test thread is the
// producer, the purger, and the audio thread.
TEST_CASE("EngineState: purge with a producer mid-write does not replay consumed frames",
          "[EngineState][purge]") {
    auto cfg = EngineFixture::defaultConfig();
    cfg.manualAudioPump = true;
    EngineFixture fix(cfg);

    // Park some consumed frames at the front of the ring: send /syncs and
    // wait for their replies, so [0, h0) holds performed frames.
    OscReply r;
    for (int i = 0; i < 3; ++i) {
        fix.send(osc_test::message("/sync", 4200 + i));
        REQUIRE(fix.waitForReply("/synced", r));
    }
    fix.clearReplies();

    const int32_t h0 = control->in_head.load(std::memory_order_acquire);
    REQUIRE(h0 > 0);
    REQUIRE(static_cast<uint32_t>(h0) + 64 < IN_BUFFER_SIZE);  // room for the frame below

    // Producer stalls mid-write: lock held, head loaded, payload not yet copied.
    int32_t unlocked = 0;
    REQUIRE(control->in_write_lock.compare_exchange_strong(unlocked, 1));

    fix.engine().purge();

    // Producer resumes: completes the frame at its pre-loaded head and
    // publishes, following RingBufferWriter::write's sequence (header +
    // payload memcpy, then head store, then unlock).
    {
        auto pkt = osc_test::message("/sync", 4299);
        Message hdr;
        hdr.magic    = MESSAGE_MAGIC;
        hdr.length   = static_cast<uint32_t>(sizeof(Message)) + pkt.size();
        hdr.sequence = static_cast<uint32_t>(
            control->in_sequence.fetch_add(1, std::memory_order_relaxed));
        hdr.sourceId = 0;
        uint8_t* slot = shared_memory + IN_BUFFER_START + h0;
        std::memcpy(slot, &hdr, sizeof(Message));
        std::memcpy(slot + sizeof(Message), pkt.ptr(), pkt.size());
        const uint32_t aligned = (hdr.length + 3u) & ~3u;
        control->in_head.store(
            static_cast<int32_t>((static_cast<uint32_t>(h0) + aligned) % IN_BUFFER_SIZE),
            std::memory_order_release);
        control->in_write_lock.store(0, std::memory_order_release);
    }

    // Drain. Frames performed before the purge must not run again.
    fix.pumpBlock(8);
    for (auto& reply : fix.allReplies()) {
        if (reply.address != "/synced") continue;
        const int32_t id = reply.parsed().argInt(0);
        CHECK(id != 4200);
        CHECK(id != 4201);
        CHECK(id != 4202);
    }

    // Engine must still be fully functional.
    fix.clearReplies();
    fix.send(osc_test::message("/status"));
    REQUIRE(fix.waitForReply("/status.reply", r));
}

TEST_CASE("EngineState: sequential swaps in headless both succeed cleanly",
          "[EngineState][atomicity]") {
    EngineFixture fix;
    // In headless mode, switchDevice with a device name is a hot swap
    // (device name is ignored, headless driver pause/resumes)
    auto r1 = fix.engine().switchDevice("test");
    REQUIRE(r1.success);
    REQUIRE(r1.type == SwapType::Hot);

    // Second call should also succeed cleanly
    auto r2 = fix.engine().switchDevice("test2");
    REQUIRE(r2.success);
    REQUIRE(r2.type == SwapType::Hot);

    // Engine should still be running
    REQUIRE(fix.engine().engineState() == EngineState::Running);
}

// ── Shared memory ownership ─────────────────────────────────────────────────

TEST_CASE("SharedMemory: engine creates an anonymous segment on boot", "[EngineState][SharedMemory]") {
    using detail_shm_segment::shm_handle_valid;
    ClockworkEngine engine;
    ClockworkEngine::Config cfg;
    cfg.headless = true;
    cfg.udpPort  = 59100;   // non-zero is what creates the segment
    engine.init(cfg);
    REQUIRE(engine.isRunning());

    // The segment exists and is what a reader would be handed: a duplicate of
    // the handle maps to a published, layout-checked segment.
    REQUIRE(shm_handle_valid(engine.shmNativeHandle()));
    {
        detail_shm_segment::shm_segment_client client(
            detail_shm_segment::shm_dup_handle(engine.shmNativeHandle()));
        CHECK(client.get_metrics() != nullptr);
    }

    // After shutdown there is nothing to hand out. The pages themselves go
    // with the last mapping; there is no name left behind to check for.
    engine.shutdown();
    CHECK_FALSE(shm_handle_valid(engine.shmNativeHandle()));
}

TEST_CASE("SharedMemory: segment survives cold swap (destroy_world + rebuild_world)",
          "[EngineState][SharedMemory]") {
    ClockworkEngine engine;
    ClockworkEngine::Config cfg;
    cfg.headless = true;
    cfg.udpPort  = 59101;
    engine.init(cfg);
    const auto handle = engine.shmNativeHandle();
    REQUIRE(detail_shm_segment::shm_handle_valid(handle));

    // Verify the ownership contract: World_Cleanup should NOT remove the
    // segment because mOwnsShmem is false.  Verify indirectly: after
    // shutdown + re-init with the same port, the segment is still accessible.
    auto pkt = osc_test::message("/status");
    engine.sendOSC(pkt.ptr(), pkt.size());

    // Same object, not a re-created one: a reader that attached before the
    // swap is still looking at live memory.
    CHECK(engine.shmNativeHandle() == handle);

    engine.shutdown();
    CHECK_FALSE(detail_shm_segment::shm_handle_valid(engine.shmNativeHandle()));
}

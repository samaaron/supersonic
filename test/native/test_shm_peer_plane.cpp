/*
 * test_shm_peer_plane.cpp — the SHM command plane (shm_peer_plane.h), in
 * process: segment geometry, the attach protocol, and the full command →
 * engine → reply-ring round trip with ShmTransport bound the way Main.cpp
 * binds it (--shm-commands).
 *
 * The cross-process crash properties (SIGKILL mid-write, lock-holding corpse,
 * reattach) are proven in test_shm_peer_crash.cpp.
 */
#include <catch2/catch_test_macros.hpp>

#include "EngineFixture.h"
#include "OscTestUtils.h"
#include "ShmTransport.h"
#include "src/lanes/ring_drain.h"
#include "src/shm_peer_plane.h"
#include "src/synth/common/server_shm.hpp"
#include "src/workers/RingBufferWriter.h"

#include <chrono>
#include <string>
#include <thread>
#include <vector>

namespace {

SupersonicEngine::Config planeConfig(unsigned port) {
    SupersonicEngine::Config cfg;
    cfg.sampleRate   = 48000;
    cfg.bufferSize   = 128;
    cfg.udpPort      = port;   // non-zero enables the public shm segment
    cfg.numBuffers   = 256;
    cfg.maxNodes     = 256;
    cfg.maxGraphDefs = 64;
    cfg.maxWireBufs  = 32;
    cfg.headless     = true;
    cfg.shmCommands  = true;
    return cfg;
}

// Write one OSC packet into the command ring exactly as a peer would.
bool peerWrite(ShmPeerPlaneHeader* plane, const osc_test::Packet& pkt) {
    return RingBufferWriter::write(
        shm_peer_cmd_ring(plane), SHM_PEER_CMD_RING_SIZE,
        &plane->cmd_head, &plane->cmd_tail,
        &plane->cmd_sequence, &plane->cmd_write_lock,
        pkt.ptr(), pkt.size(), 0);
}

// Drain every complete frame currently in the reply ring into `out`.
void drainReplies(ShmPeerPlaneHeader* plane, SsDrainState& st,
                  std::vector<std::vector<uint8_t>>& out) {
    ss_drain_ring(
        shm_peer_rep_ring(plane), SHM_PEER_REP_RING_SIZE,
        &plane->rep_head, &plane->rep_tail, st, SsDrainMetrics{}, 0,
        [&](uint32_t /*src*/, const uint8_t* d, uint32_t n, uint32_t) {
            out.emplace_back(d, d + n);
            return SsDrainVerdict::Consume;
        });
}

bool waitUntil(const std::function<bool()>& f, int ms = 3000) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (f()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

} // namespace

TEST_CASE("shm-peer: segment publishes plane geometry and attach claims it",
          "[shm][peer]") {
    constexpr unsigned kPort = 57221;
    EngineFixture fx(planeConfig(kPort));

    server_shared_memory_client client(kPort);
    ShmPeerPlaneHeader* plane = client.get_peer_plane();
    REQUIRE(plane != nullptr);

    // Geometry the peer reads (it has no compile-time view of our build).
    CHECK(plane->cmd_ring_size == SHM_PEER_CMD_RING_SIZE);
    CHECK(plane->rep_ring_size == SHM_PEER_REP_RING_SIZE);

    // Attach: generation bumps, a stale writer lock is reset, stale replies
    // are skipped (tail jumps to head).
    plane->cmd_write_lock.store(1, std::memory_order_relaxed);  // corpse held it
    uint32_t g0 = plane->generation.load(std::memory_order_relaxed);
    uint32_t g1 = shm_peer_attach(plane, 12345);
    CHECK(g1 == g0 + 1);
    CHECK(plane->owner_pid.load(std::memory_order_relaxed) == 12345);
    CHECK(plane->cmd_write_lock.load(std::memory_order_relaxed) == 0);
    CHECK(plane->rep_tail.load(std::memory_order_relaxed)
          == plane->rep_head.load(std::memory_order_relaxed));

    uint32_t g2 = shm_peer_attach(plane, 12346);  // last attach wins
    CHECK(g2 == g1 + 1);
    CHECK(plane->owner_pid.load(std::memory_order_relaxed) == 12346);
}

TEST_CASE("shm-peer: command ring commands reach the engine and reply",
          "[shm][peer]") {
    constexpr unsigned kPort = 57222;
    EngineFixture fx(planeConfig(kPort));

    server_shared_memory_client client(kPort);
    ShmPeerPlaneHeader* plane = client.get_peer_plane();
    REQUIRE(plane != nullptr);
    shm_peer_attach(plane, 1);

    // The fixture's CallbackTransport surfaces replies for every token via
    // onReply, so the peer command's /synced lands in the fixture collector.
    REQUIRE(peerWrite(plane, osc_test::message("/sync", 4242)));

    OscReply reply;
    REQUIRE(fx.waitForReply("/synced", reply));
    CHECK(reply.parsed().argInt(0) == 4242);
}

TEST_CASE("shm-peer: ShmTransport routes replies into the reply ring",
          "[shm][peer]") {
    constexpr unsigned kPort = 57223;

    // Engine wired the way Main.cpp wires --shm-commands: ShmTransport bound
    // to the engine's plane slot, set before init.
    ShmTransport     transport;
    SupersonicEngine engine;
    engine.onDebug = [](const std::string&) {};
    engine.onReply = [](const uint8_t*, uint32_t) {};
    engine.setTransport(&transport);
    engine.init(planeConfig(kPort));
    transport.bindPlaneSlot(engine.peerPlaneSlot());
    REQUIRE(transport.ready());

    server_shared_memory_client client(kPort);
    ShmPeerPlaneHeader* plane = client.get_peer_plane();
    REQUIRE(plane != nullptr);
    shm_peer_attach(plane, 1);

    SsDrainState repState;
    std::vector<std::vector<uint8_t>> replies;

    // /sync round trip through shared memory in both directions.
    REQUIRE(peerWrite(plane, osc_test::message("/sync", 7)));
    bool got = waitUntil([&] {
        drainReplies(plane, repState, replies);
        return !replies.empty();
    });
    REQUIRE(got);
    CHECK(osc_test::parseAddress(replies[0].data(),
                                 static_cast<uint32_t>(replies[0].size())) == "/synced");

    // Notify subscription: /supersonic/notify from the peer flips the
    // transport's audience flag and its .reply arrives on the reply ring.
    replies.clear();
    REQUIRE_FALSE(transport.hasNotifySubscribers());
    REQUIRE(peerWrite(plane, osc_test::message("/supersonic/notify")));
    got = waitUntil([&] {
        drainReplies(plane, repState, replies);
        return !replies.empty() && transport.hasNotifySubscribers();
    });
    REQUIRE(got);

    // Reply-ring frames carry the peer's origin token in the Message header —
    // verified indirectly: send() to any other token must be undeliverable.
    CHECK_FALSE(transport.send(0, replies[0].data(),
                               static_cast<uint32_t>(replies[0].size()), false));
    CHECK(transport.send(SHM_PEER_ORIGIN_TOKEN, replies[0].data(),
                         static_cast<uint32_t>(replies[0].size()), false));

    engine.shutdown();
    CHECK_FALSE(transport.ready());  // shutdown nulls the published plane
}

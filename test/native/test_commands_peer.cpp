// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Sam Aaron
/*
 * test_commands_peer.cpp — supersonic::Commands in a peer's hands
 * (docs/COMMANDS.md): over a handle onto the engine's command plane, with no
 * transport to answer through, the front answers the file verbs itself and
 * hands its answers out of poll(). The engine never sees a file verb, from
 * the plane or from anywhere.
 */
#include <catch2/catch_test_macros.hpp>

#include "EngineFixture.h"
#include "OscTestUtils.h"
#include "ShmTransport.h"
#include "clockwork_client.h"
#include "shm_segment.hpp"
#include "supersonic_commands.h"
#include "supersonic_commands_api.h"

#include <chrono>
#include <filesystem>
#include <string>
#include <thread>

namespace {

ClockworkEngine::Config peerConfig() {
    ClockworkEngine::Config cfg = EngineFixture::defaultConfig();
    cfg.shmSegment  = true;
    cfg.shmCommands = true;   // the command segment: the plane and the lanes
    return cfg;
}

// Both segments, as a peer in another process would hold them.
ClockworkClient* openPeer(ClockworkEngine& engine) {
    ClockworkStatus st = CLOCKWORK_E_NOT_FOUND;
    ClockworkClient* peer = clockwork_client_open_shm_peer_handles(
        detail_shm_segment::shm_handle_to_intptr(detail_shm_segment::shm_dup_handle(engine.shmReaderHandle())),
        detail_shm_segment::shm_handle_to_intptr(detail_shm_segment::shm_dup_handle(engine.shmCommandHandle())),
        0 /* no wake event: the case pumps the blocks that drain the plane */, &st);
    REQUIRE(peer != nullptr);
    REQUIRE(st == CLOCKWORK_OK);
    return peer;
}

} // namespace

TEST_CASE("commands peer: a peer's front answers /b_allocRead through the plane, and the engine answers /b_query",
          "[commands][peer][shm]") {
    const std::string sample = std::string(CLOCKWORK_SAMPLES_DIR) + "/bd_haus.flac";
    if (!std::filesystem::exists(sample)) SKIP("no shipped samples at " << CLOCKWORK_SAMPLES_DIR);

    // The engine's replies to the peer leave through the plane's reply ring.
    ShmTransport shm;
    EngineFixture fx(peerConfig(), [&](ClockworkEngine& engine) { engine.setTransport(&shm); });
    shm.bindPlaneSlot(fx.engine().peerPlaneSlot());
    REQUIRE(shm.ready());

    // The front, in the peer's hands: no transport to answer through, so its
    // answers come out of poll(). The front owns the handle.
    using Work = supersonic::Commands::Work;
    const Work work = fx.testIsTheHost() ? Work::Host : Work::Own;
    supersonic::Commands front(openPeer(fx.engine()), nullptr, work);
    if (work == Work::Host) fx.setHostWork([&] { front.runPending(); });

    // A turn: a block (its control pass drains the plane), and a look at the
    // plane's replies through the front.
    ClockworkClientMessage msgs[16];
    const auto turnUntil = [&](const char* addr, const char* cmd, osc_test::ParsedReply* out, int blocks) {
        for (int i = 0; i < blocks; ++i) {
            fx.pumpBlock();
            if (work == Work::Own) std::this_thread::sleep_for(std::chrono::milliseconds(2));
            const uint32_t n = front.poll(msgs, 16);
            for (uint32_t k = 0; k < n; ++k) {
                if (osc_test::parseAddress(msgs[k].bytes, msgs[k].length) != addr) continue;
                auto r = osc_test::parseReply(msgs[k].bytes, msgs[k].length);
                if (cmd && (r.argCount() < 1 || r.argString(0) != cmd)) continue;
                if (out) *out = r;
                return true;
            }
        }
        return false;
    };

    // /b_allocRead is the front's: taken, worked here, answered in scsynth's
    // words to the token the asker used.
    osc_test::Builder b;
    b.begin("/b_allocRead") << int32_t{3} << sample.c_str() << int32_t{0} << int32_t{0};
    const auto alloc = b.end();
    REQUIRE(front.ingress(alloc.ptr(), alloc.size(), 77));
    osc_test::ParsedReply done;
    REQUIRE(turnUntil("/done", "/b_allocRead", &done, 4000));
    CHECK(done.argInt(1) == 3);

    // /b_query is the engine's: not the front's to take, sent on the plane by
    // the peer, answered back through it with the frames the load put there.
    const auto query = osc_test::message("/b_query", int32_t{3});
    CHECK_FALSE(front.send(query.ptr(), query.size(), 77));   // not the front's: sent on
    osc_test::ParsedReply info;
    REQUIRE(turnUntil("/b_info", nullptr, &info, 2000));
    CHECK(info.argInt(0) == 3);
    CHECK(info.argInt(1) > 0);

    fx.setHostWork(nullptr);
}

TEST_CASE("commands peer: the C surface is the same front, for a peer in another language",
          "[commands][peer][shm][c]") {
    const std::string sample = std::string(CLOCKWORK_SAMPLES_DIR) + "/bd_haus.flac";
    if (!std::filesystem::exists(sample)) SKIP("no shipped samples at " << CLOCKWORK_SAMPLES_DIR);

    ShmTransport shm;
    EngineFixture fx(peerConfig(), [&](ClockworkEngine& engine) { engine.setTransport(&shm); });
    shm.bindPlaneSlot(fx.engine().peerPlaneSlot());
    REQUIRE(shm.ready());

    // No threads of its own: the caller runs the file work, as the host work.
    SupersonicCommands* f = supersonic_commands_open(openPeer(fx.engine()), 0);
    REQUIRE(f != nullptr);
    CHECK(supersonic_commands_open(nullptr, 0) == nullptr);
    fx.setHostWork([&] { supersonic_commands_run_pending(f); });

    osc_test::Builder b;
    b.begin("/b_allocRead") << int32_t{5} << sample.c_str() << int32_t{0} << int32_t{0};
    const auto alloc = b.end();
    CHECK(supersonic_commands_send(f, alloc.ptr(), alloc.size(), 9) == 1);   // the front's
    const auto query = osc_test::message("/b_query", int32_t{5});

    ClockworkClientMessage msgs[16];
    bool done = false, info = false;
    for (int i = 0; i < 4000 && !(done && info); ++i) {
        fx.pumpBlock();
        if (!fx.testIsTheHost()) {
            supersonic_commands_run_pending(f);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        const uint32_t n = supersonic_commands_poll(f, msgs, 16);
        for (uint32_t k = 0; k < n; ++k) {
            const auto addr = osc_test::parseAddress(msgs[k].bytes, msgs[k].length);
            auto r = osc_test::parseReply(msgs[k].bytes, msgs[k].length);
            if (addr == "/done" && r.argCount() > 1 && r.argString(0) == "/b_allocRead") {
                CHECK(r.argInt(1) == 5);
                CHECK(msgs[k].origin == 9);   // answered to the asker's token
                done = true;
                CHECK(supersonic_commands_send(f, query.ptr(), query.size(), 9) == 0);   // the engine's
            }
            if (addr == "/b_info" && r.argInt(0) == 5) {
                CHECK(r.argInt(1) > 0);
                info = true;
            }
        }
    }
    CHECK(done);
    CHECK(info);
    fx.setHostWork(nullptr);
    supersonic_commands_close(f);
}

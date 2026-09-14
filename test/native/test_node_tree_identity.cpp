// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Sam Aaron
/*
 * test_node_tree_identity.cpp — the arena table says whose the window is.
 *
 * The node tree mirror lives in DspConfig::shm_window, a region clockwork
 * lays out but never interprets, so a client reading it by hand has no way
 * to know which guest wrote it, or which layout, before it casts a pointer.
 * dsp_describe() declares NODE_TREE_WINDOW_MAGIC / _VERSION and clockwork
 * copies them into the table's window entry when the guest binds. A client
 * that opens the public segment finds them there.
 */
#include <catch2/catch_test_macros.hpp>
#include "EngineFixture.h"
#include "OscTestUtils.h"
#include "shm_segment.hpp"
#include "shared_memory.h"
#include "node_tree.h"
#include "dsp_api.h"

namespace {
ClockworkEngine::Config segmentConfig(unsigned port) {
    ClockworkEngine::Config cfg;
    cfg.sampleRate    = 48000;
    cfg.bufferSize    = 128;
    cfg.udpPort       = port;   // non-zero: the public shm segment exists
    cfg.numBuffers    = 256;
    cfg.maxNodes      = 256;
    cfg.maxGraphDefs  = 64;
    cfg.maxWireBufs   = 32;
    cfg.headless      = true;
    return cfg;
}
}  // namespace

TEST_CASE("node tree: dsp_describe declares the window as the scsynth node tree",
          "[node_tree][arena][audience]") {
    const DspInfo* info = dsp_describe();
    REQUIRE(info != nullptr);
    CHECK(info->window_magic   == NODE_TREE_WINDOW_MAGIC);
    CHECK(info->window_version == NODE_TREE_WINDOW_VERSION);
    CHECK(NODE_TREE_WINDOW_MAGIC == 0x53434E54u);   // 'SCNT'
}

TEST_CASE("node tree: a client finds the window's identity in the arena table",
          "[node_tree][arena][audience][shm]") {
    constexpr unsigned kPort = 57221;
    EngineFixture fx(segmentConfig(kPort));
    fx.send(osc_test::message("/sync", 7));
    OscReply r;
    REQUIRE(fx.waitForReply("/synced", r));

    // Open the public segment as a separate client, the way a GUI does.
    shm_segment_client client(detail_shm_segment::shm_dup_handle(fx.engine().shmNativeHandle()));
    const ClockworkArenaHeader* h = arenaHeader(client.get_base());
    REQUIRE(h != nullptr);
    const ClockworkArenaEntry* w = clockwork_arena_find(h, CLOCKWORK_ARENA_GUEST_WINDOW);
    REQUIRE(w != nullptr);
    CHECK(w->geom[CLOCKWORK_GEOM_WINDOW_MAGIC]   == NODE_TREE_WINDOW_MAGIC);
    CHECK(w->geom[CLOCKWORK_GEOM_WINDOW_VERSION] == NODE_TREE_WINDOW_VERSION);
    // The window is a published contract, in the guest's published run.
    CHECK(clockwork_arena_published(h, CLOCKWORK_ARENA_GUEST_WINDOW));
    CHECK(w->offset + w->bytes <= h->guest_published_end);
    // And the record behind it is the node tree: its version word, the one
    // the host watches, is where the layout says it is.
    const auto* tree = reinterpret_cast<const NodeTreeHeader*>(client.get_base() + w->offset);
    CHECK(tree->node_count.load() <= NODE_TREE_MIRROR_MAX_NODES);
}

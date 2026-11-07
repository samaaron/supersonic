// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Sam Aaron
/*
 * test_sequenced_refusals.cpp - sequenced commands whose Init returns early.
 *
 * CallSequencedCommand builds a command in memory from the real-time pool,
 * calls Init, and calls Delete if Init returns an error. That memory still holds
 * whatever was there before. The destructor runs on a command Init may not have
 * finished, so every pointer it frees needs a value before Init runs.
 */
#include "SynthDefBytes.h"
#include "GuestWorld.h"   // the engine's World, whose pool holds the commands

#include "SC_Prototypes.h"   // World_Alloc
#include "SC_SequencedCommand.h"

#include <catch2/catch_template_test_macros.hpp>

#include <cstring>
#include <new>

using namespace synthdef_bytes;

// AudioQuitCmd has no arguments to refuse and nothing to free. LoadSynthDefCmd
// and LoadSynthDefDirCmd are not built into the engine: it does not read files.
TEMPLATE_TEST_CASE("Sequenced commands: Delete after an Init with no arguments frees only what Init allocated",
                   "[osc][security]", BufAllocCmd, BufAllocReadCmd, BufAllocReadChannelCmd, BufCloseCmd, BufFreeCmd,
                   BufGenCmd, BufReadCmd, BufReadChannelCmd, BufWriteCmd, BufZeroCmd, NotifyCmd, RecvSynthDefCmd,
                   SendReplyCmd, SyncCmd) {
    EngineFixture fx;
    World* world = guestWorld(fx);
    void* space = World_Alloc(world, sizeof(TestType));
    REQUIRE(space);
    std::memset(space, 0xBE, sizeof(TestType));
    auto* cmd = new (space) TestType(world, nullptr);
    const char noArguments[4] = {};
    (void)cmd->Init(noArguments, 0);
    cmd->Delete();
    stillRunning(fx);
}

TEST_CASE("Sequenced commands: /b_write refused before it reads a filename leaves the engine running",
          "[osc][security]") {
    EngineFixture fx;
    // The first command leaves its filename pointer in freed memory, where the
    // next command of the same size lands.
    osc_test::Builder named;
    named.begin("/b_write") << 0 << "gone.wav";
    fx.send(named.end());
    fx.pumpBlock(2);
    fx.send(osc_test::message("/b_write"));
    fx.send(osc_test::message("/b_write", 0));
    fx.send(osc_test::message("/b_write", 99999));
    fx.pumpBlock(2);
    stillRunning(fx);
}

// The engine's real-time pool: the bytes free, from /rtMemoryStatus.
static int32_t poolBytesFree(EngineFixture& fx) {
    fx.clearReplies();
    fx.send(osc_test::message("/rtMemoryStatus"));
    OscReply r;
    REQUIRE(fx.waitForReply("/rtMemoryStatus.reply", r));
    return r.parsed().argInt(0);
}

TEST_CASE("Sequenced commands: /d_recv with no data gives its command back to the pool", "[osc][security]") {
    EngineFixture fx;
    const auto bareRecv = [&](int times) {
        for (int i = 0; i < times; ++i) {
            fx.send(osc_test::message("/d_recv"));
            fx.pumpBlock(1);
        }
    };
    bareRecv(1);   // anything the first refusal sets up stays set up
    const int32_t before = poolBytesFree(fx);
    bareRecv(200);
    CHECK(poolBytesFree(fx) == before);
    stillRunning(fx);
}

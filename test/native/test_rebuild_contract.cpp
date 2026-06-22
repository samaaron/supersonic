/*
 * test_rebuild_contract.cpp — Audio-rebuild (cold-swap) contract.
 *
 * On a device/driver rebuild the engine should:
 *   S1 quiesce the audio thread at a tick boundary (confirmed-stopped),
 *   S2 drain the OUT-rt + NRT egress rings TO THE CLIENT (deliver, not drop),
 *   S3 discard the IN ring (its node/group ids are now invalid),
 *   S4 reset the node tree,
 *   S5 PRESERVE /notify registrations — a client registered before the rebuild
 *      must keep receiving /n_go//n_end afterwards WITHOUT re-registering,
 *   S6 resume.
 *
 * These tests pin the observable invariants. Some are expected to FAIL against
 * the current engine (they encode the target contract, not today's behaviour) —
 * that gap is the point: they become the spec the engine fix must satisfy.
 */
#include <catch2/catch_test_macros.hpp>
#include "EngineFixture.h"
#include "OscBuilder.h"

// ── Control: re-registering /notify after a cold swap yields /n_go ────────────
// Mirrors the engine's CURRENT expected flow (Spider re-registers post-swap).
// Proves the harness/setup is sound, so a failure of the S5 test below is about
// preservation specifically, not test plumbing.
TEST_CASE("RebuildContract: re-register /notify after cold swap yields /n_go",
          "[Rebuild]") {
    EngineFixture fix;

    auto result = fix.engine().switchDevice("", 44100);
    REQUIRE(result.success);
    REQUIRE(result.type == SwapType::Cold);
    fix.clearReplies();

    fix.send(osc_test::message("/notify", 1));
    OscReply reply;
    REQUIRE(fix.waitForReply("/done", reply, 5000));
    fix.clearReplies();

    fix.send(osc_test::message("/g_new", 2, 0, 0));
    REQUIRE(fix.waitForReply("/n_go", reply, 5000));
}

// ── S5: /notify is preserved across a cold swap (no re-registration) ──────────
// The decisive test. Register BEFORE the swap; after the swap create a group
// WITHOUT re-registering. If notify is preserved, /n_go must arrive. If it
// times out, the rebuild dropped the registration — the lost-/n_go root cause.
TEST_CASE("RebuildContract: /notify preserved across cold swap (no re-register)",
          "[Rebuild]") {
    EngineFixture fix;

    // Register for notifications BEFORE the swap.
    fix.send(osc_test::message("/notify", 1));
    OscReply reply;
    REQUIRE(fix.waitForReply("/done", reply));
    fix.clearReplies();

    auto result = fix.engine().switchDevice("", 44100);
    REQUIRE(result.success);
    REQUIRE(result.type == SwapType::Cold);
    fix.clearReplies();

    // NO /notify here. Contract S5: a node created after the rebuild must still
    // notify the pre-registered client.
    fix.send(osc_test::message("/g_new", 2, 0, 0));
    bool gotNgo = fix.waitForReply("/n_go", reply, 2000);
    INFO("If this fails, the cold-swap world rebuild dropped /notify (S5 violated)");
    CHECK(gotNgo);
}

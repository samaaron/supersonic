/*
 * test_stream_client_registration.cpp — engine behaviour that stream-transport
 * clients (TCP/UDS/pipe) depend on, added with the Sonic Pi TCP command-plane
 * migration (2026-08-05):
 *
 *  1. /clock/notify/subscribe must be acked EVERY time a tokened subscribe
 *     arrives, not only on first registration. Clients confirm subscription
 *     by resending with a correlation token until acked (subscribe is
 *     idempotent); gating the ack on "newly registered" made every confirm
 *     after an initial untokened subscribe time out silently.
 *
 *  2. /supersonic/notify must replay the current lifecycle STATE
 *     (/supersonic/statechange) to the new registrant: stream clients can
 *     only connect after engine init, so they miss the boot broadcast (the
 *     "late joiner" gap; UDP clients pre-registered via the boot queue).
 *
 *  3. The replay must be state-only: /supersonic/setup is a rebuild EVENT
 *     and replaying it forced clients into spurious cold-swap reinits.
 */
#include <catch2/catch_test_macros.hpp>

#include "EngineFixture.h"
#include "OscTestUtils.h"

TEST_CASE("Clock notify subscribe is acked on every tokened request",
          "[notify][clock]") {
    EngineFixture fx;

    // Untokened subscribe first — mirrors SupersonicComms, which fires a
    // plain subscribe before its tokened confirm loop. This registers the
    // caller, making the next subscribe a re-registration.
    fx.send(osc_test::message("/clock/notify/subscribe"));

    // First tokened confirm: must be acked with the token echoed.
    fx.send(osc_test::message("/clock/notify/subscribe", 111));
    OscReply r1;
    REQUIRE(fx.waitForReply("/clock/notify/subscribe.reply", r1));
    auto p1 = r1.parsed();
    REQUIRE(p1.argCount() >= 1);
    CHECK(p1.argInt(p1.argCount() - 1) == 111);

    // Second tokened confirm (already registered): must STILL be acked.
    fx.send(osc_test::message("/clock/notify/subscribe", 222));
    // Wait for the reply carrying THIS token (the first reply may still be
    // in the collection buffer).
    const bool acked = fx.pollUntil([&] {
        for (auto& r : fx.allReplies()) {
            if (r.address != "/clock/notify/subscribe.reply") continue;
            auto p = r.parsed();
            if (p.argCount() >= 1 && p.argInt(p.argCount() - 1) == 222) return true;
        }
        return false;
    });
    CHECK(acked);
}

TEST_CASE("Notify registration replays lifecycle state, never the setup event",
          "[notify][statechange]") {
    EngineFixture fx;

    fx.send(osc_test::message("/supersonic/notify"));
    OscReply ack;
    REQUIRE(fx.waitForReply("/supersonic/notify.reply", ack));

    // The registrant is told the CURRENT state (running, post-init).
    OscReply state;
    REQUIRE(fx.waitForReply("/supersonic/statechange", state));
    auto ps = state.parsed();
    REQUIRE(ps.argCount() >= 1);
    CHECK(ps.argString(0) == "running");

    // …but never a replayed /supersonic/setup: that is a World-rebuild
    // EVENT, and replaying it triggers spurious client reinits. Give any
    // stray setup a few blocks to surface before asserting its absence.
    fx.waitForBlocks(8);
    for (auto& r : fx.allReplies())
        CHECK(r.address != "/supersonic/setup");
}

TEST_CASE("devices/report with no port registers the caller connection",
          "[notify][devices]") {
    EngineFixture fx;

    // Portless form (stream transports have no addressable reply port):
    // must subscribe the caller and trigger a device report broadcast that
    // reaches it. Headless engines still produce a (possibly empty) report.
    fx.send(osc_test::message("/supersonic/devices/report"));
    OscReply devices;
    CHECK(fx.waitForReply("/supersonic/devices", devices, 10000));
}

/*
 * test_control_plane_liveness.cpp — registering for notifications must not
 * touch the audio device (sonic-pi#3551).
 *
 * The reported failure: SuperSonic boots on Windows, opens a DirectSound
 * device, prints its banner — and then answers nothing. The daemon's
 * /supersonic/notify is acked, spider's never is, so spider gives up after
 * 30 s and the user is told "GUI was unable to connect to the Ruby server".
 * The reporter could reproduce it at will by launching Voicemeeter first.
 *
 * Registration used to trigger a device report for each newly-seen client. On
 * Windows that report enumerates every device through COM, which is where
 * Voicemeeter's virtual drivers turn a fast probe into a pathological one —
 * putting it squarely in the path of spider's boot handshake. Clients that
 * want the device list ask for it (/supersonic/devices/report), so the
 * unsolicited probe bought nothing and cost the boot.
 */
#include <catch2/catch_test_macros.hpp>
#include "EngineFixture.h"
#include "OscBuilder.h"

TEST_CASE("Notify registration is device-free", "[control][3551]") {
    EngineFixture fix;

    OscReply r;
    fix.send(osc_test::message("/supersonic/notify"));
    REQUIRE(fix.waitForReply("/supersonic/notify.reply", r));

    // No device traffic may follow a registration — the list only arrives when
    // a client asks for it.
    REQUIRE_FALSE(fix.waitForReply("/supersonic/devices", r, 250));
}

TEST_CASE("Notify registration stays device-free for every new client",
          "[control][3551]") {
    EngineFixture fix;

    // Spider registers after the daemon, from a different origin. It was that
    // second registration — a fresh client, so a fresh probe — that landed in
    // the boot handshake.
    for (int i = 0; i < 3; ++i) {
        OscReply r;
        fix.send(osc_test::message("/supersonic/notify"));
        REQUIRE(fix.waitForReply("/supersonic/notify.reply", r));
        REQUIRE_FALSE(fix.waitForReply("/supersonic/devices", r, 100));
    }
}

// The device list must still be available on request — the fix removes the
// unsolicited probe, not the feature.
TEST_CASE("Device report is still delivered when asked for", "[control][3551]") {
    EngineFixture fix;

    OscReply r;
    fix.send(osc_test::message("/supersonic/notify"));
    REQUIRE(fix.waitForReply("/supersonic/notify.reply", r));

    fix.send(osc_test::message("/supersonic/devices/report"));
    REQUIRE(fix.waitForReply("/supersonic/devices", r, 2000));
}

// Device commands now run on the device worker rather than the gateway, so the
// thing to pin is that their replies still arrive — an offloaded command that
// silently stops answering is a worse bug than the one being fixed.
TEST_CASE("Offloaded device commands still reply", "[control][3551]") {
    EngineFixture fix;
    OscReply r;

    // The report only goes to notify subscribers, so register first — same as
    // a real client does before asking for device state.
    fix.send(osc_test::message("/supersonic/notify"));
    REQUIRE(fix.waitForReply("/supersonic/notify.reply", r));

    fix.send(osc_test::message("/supersonic/devices/mode", "system"));
    REQUIRE(fix.waitForReply("/supersonic/devices/mode.reply", r, 2000));

    fix.send(osc_test::message("/supersonic/devices/report"));
    REQUIRE(fix.waitForReply("/supersonic/devices", r, 2000));
}

// The control thread is the sole non-RT consumer: whatever it does while
// handling one client's registration is time every other client spends
// unanswered. Assert on the engine's own measurement rather than a wall-clock
// deadline here, so a loaded CI runner can't turn preemption into a failure.
TEST_CASE("Registering clients does not park the control thread",
          "[control][blocking][3551]") {
    EngineFixture fix;
    fix.engine().resetNrtMaxPass();

    // Five origins register during a real boot (daemon boot socket, daemon
    // api_server, spider boot socket, spider main, GUI) — the second of them
    // was what parked the thread behind the first one's device probe.
    for (int i = 0; i < 5; ++i) {
        OscReply r;
        fix.send(osc_test::message("/supersonic/notify"));
        REQUIRE(fix.waitForReply("/supersonic/notify.reply", r));
    }

    CHECK(fix.engine().nrtInFlightMs() == 0);
    CHECK(fix.engine().nrtMaxPassMs() < 250);
}

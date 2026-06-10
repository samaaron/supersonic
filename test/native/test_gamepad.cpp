/*
 * test_gamepad.cpp — the /gamepad/ subsystem routed through the engine ingress
 * (sendOSC -> ingest -> GamepadControl -> Rust ss_gamepad subsystem -> egress).
 *
 * No controller hardware required: these pin that /gamepad commands reach the
 * subsystem and that its replies/pushes come back through the egress, that
 * subscription pushes a devices snapshot, and that the rumble/enable paths are
 * robust with no pads connected (the device list is typically empty on a
 * headless CI box — and stays empty if the platform backend can't start at
 * all, e.g. without HID permissions, which must not break the OSC surface).
 */
#include <catch2/catch_test_macros.hpp>

#include "EngineFixture.h"
#include "OscTestUtils.h"

#ifdef SUPERSONIC_GAMEPAD

TEST_CASE("/gamepad/devices/list replies through the engine", "[gamepad]") {
    EngineFixture fx;
    fx.clearReplies();
    fx.send(osc_test::message("/gamepad/devices/list"));

    OscReply r;
    REQUIRE(fx.waitForReply("/gamepad/devices.reply", r));
    // First arg is the pad count: ≥ 0 even with no controllers attached.
    CHECK(r.parsed().argInt(0) >= 0);
}

TEST_CASE("/gamepad/notify/subscribe pushes a devices snapshot", "[gamepad]") {
    EngineFixture fx;
    fx.clearReplies();
    fx.send(osc_test::message("/gamepad/notify/subscribe"));

    OscReply r;
    CHECK(fx.waitForReply("/gamepad/devices.reply", r));
}

TEST_CASE("/gamepad/refresh broadcasts a devices update", "[gamepad]") {
    EngineFixture fx;
    // A subscriber is needed for the broadcast to have an in-process audience.
    fx.send(osc_test::message("/gamepad/notify/subscribe"));
    fx.clearReplies();
    fx.send(osc_test::message("/gamepad/refresh"));

    OscReply r;
    CHECK(fx.waitForReply("/gamepad/devices", r));
}

TEST_CASE("/gamepad rumble + enable dispatch is robust with no pads", "[gamepad]") {
    EngineFixture fx;

    // None of these have a connected target, so they are no-ops — but must not
    // crash the subsystem or the engine.
    osc_test::Builder rumble;
    rumble.begin("/gamepad/out/rumble")
        << "*" << 1.0f << 0.5f << static_cast<osc::int32>(100);
    fx.send(rumble.end());

    fx.send(osc_test::message("/gamepad/out/rumble_stop", "*"));

    osc_test::Builder disable;
    disable.begin("/gamepad/enable") << "*" << static_cast<osc::int32>(0);
    fx.send(disable.end());

    osc_test::Builder enable;
    enable.begin("/gamepad/enable") << "*" << static_cast<osc::int32>(1);
    fx.send(enable.end());

    // The engine is still alive and serving /gamepad afterwards.
    fx.clearReplies();
    fx.send(osc_test::message("/gamepad/devices/list"));
    OscReply r;
    CHECK(fx.waitForReply("/gamepad/devices.reply", r));
}

#endif // SUPERSONIC_GAMEPAD

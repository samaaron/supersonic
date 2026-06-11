/*
 * GamepadControl.cpp — see GamepadControl.h. The Rust subsystem runs its
 * device IO on its own poll thread; its callback may fire off the audio
 * thread, so everything it touches (the egress ring) is already thread-safe.
 */
#include "GamepadControl.h"

#include "OscEgress.h"
#include "ss_gamepad.h"
#include "osc/OscReceivedElements.h"

#include <cstdio>
#include <cstring>
#include <string>

void GamepadControl::init(OscEgress* egress) {
    mEgress = egress;
    if (!mGamepad) {
        mGamepad = ss_gamepad_create(this, &GamepadControl::emitCb);
    }
}

void GamepadControl::shutdown() {
    if (mGamepad) {
        ss_gamepad_destroy(mGamepad);
        mGamepad = nullptr;
    }
}

bool GamepadControl::handleGamepadCommand(const uint8_t* data, uint32_t size) {
    if (size < 12 || std::memcmp(data, "/gamepad/", 9) != 0) return false;

    // Subscription drives the egress audience (owned by the transport). The
    // address is the leading, NUL-terminated OSC string.
    const char* addr = reinterpret_cast<const char*>(data);
    if (std::strcmp(addr, "/gamepad/notify/subscribe") == 0) {
        if (mEgress && mEgress->subscribeCallerToGamepadNotify() && mGamepad)
            ss_gamepad_emit_devices(mGamepad);  // devices snapshot to the new subscriber
        return true;
    }
    if (std::strcmp(addr, "/gamepad/notify/unsubscribe") == 0) {
        if (mEgress) mEgress->unsubscribeCallerFromGamepadNotify();
        return true;
    }

    if (mGamepad) ss_gamepad_handle_osc(mGamepad, data, size);
    return true;
}

// Hotplug logging. /gamepad/devices broadcasts fire only on connect/
// disconnect/enable changes (plus one snapshot per new subscriber), so this
// is flood-safe. Per-event traffic (/gamepad/in/axis, /gamepad/in/button) is
// deliberately not logged. Payload: <n:i> [name:s enabled:i]*
static void logGamepadDevicesChange(const uint8_t* data, uint32_t len) {
    if (std::strcmp(reinterpret_cast<const char*>(data), "/gamepad/devices") != 0) return;
    std::string names;
    try {
        osc::ReceivedMessage msg(osc::ReceivedPacket(
            reinterpret_cast<const char*>(data),
            static_cast<osc::osc_bundle_element_size_t>(len)));
        auto it = msg.ArgumentsBegin();
        if (it == msg.ArgumentsEnd() || !it->IsInt32()) return;
        const int n = it->AsInt32Unchecked(); ++it;
        for (int i = 0; i < n && it != msg.ArgumentsEnd(); ++i) {
            if (!it->IsString()) return;
            if (!names.empty()) names += ", ";
            names += it->AsStringUnchecked(); ++it;   // name
            if (it != msg.ArgumentsEnd()) ++it;       // enabled flag
        }
    } catch (...) { return; }
    fprintf(stderr, "[gamepad] devices: [%s]\n", names.c_str());
    fflush(stderr);
}

void GamepadControl::emitCb(void* ctx, int32_t kind, const uint8_t* osc, uint32_t len) {
    auto* self = static_cast<GamepadControl*>(ctx);
    if (!self->mEgress) return;
    if (kind == SS_GAMEPAD_EMIT_REPLY) {
        self->mEgress->reply(osc, len);
    } else {
        logGamepadDevicesChange(osc, len);
        self->mEgress->broadcastGamepadNotify(osc, len);
    }
}

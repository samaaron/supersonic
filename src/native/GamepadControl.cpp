/*
 * GamepadControl.cpp — see GamepadControl.h. The Rust subsystem runs its
 * device IO on its own poll thread; its callback may fire off the audio
 * thread, so everything it touches (the egress ring) is already thread-safe.
 */
#include "GamepadControl.h"

#include "OscEgress.h"
#include "ss_gamepad.h"

#include <cstring>

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

void GamepadControl::emitCb(void* ctx, int32_t kind, const uint8_t* osc, uint32_t len) {
    auto* self = static_cast<GamepadControl*>(ctx);
    if (!self->mEgress) return;
    if (kind == SS_GAMEPAD_EMIT_REPLY)
        self->mEgress->reply(osc, len);
    else
        self->mEgress->broadcastGamepadNotify(osc, len);
}

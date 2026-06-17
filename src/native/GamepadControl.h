/*
 * GamepadControl.h — the "/gamepad/" engine seam.
 *
 * Owns the Rust gamepad subsystem (ss_gamepad_*, dual MIT/GPL crate —
 * gilrs, or GameController on macOS — linked via the supersonic-native
 * umbrella staticlib) and bridges it
 * to the engine: forwards "/gamepad/" control OSC into it, and routes its
 * callback back out — "/gamepad/in/" events + "/gamepad/devices" to the egress
 * hub. Subscription manages the egress audience. The MIDI seam's sibling
 * (MidiControl), minus the clock/transport feeds: controllers carry no tempo.
 */
#pragma once

#include <cstdint>

struct SsGamepad;    // rust/supersonic-gamepad/cpp/ss_gamepad.h
class OscEgress;
struct DrainCallCtx;

class GamepadControl {
public:
    void init(OscEgress* egress);
    void shutdown();

    // Handle one "/gamepad/" command off the audio thread (NRT gateway).
    // Returns true if it belongs to this subsystem (always, for a "/gamepad/"
    // prefix).
    bool handleGamepadCommand(const DrainCallCtx& meta, const uint8_t* data, uint32_t size);

private:
    // ss_gamepad_* host callback (ctx = this). May fire on the subsystem's
    // poll thread; the egress producer side is thread-safe.
    static void emitCb(void* ctx, int32_t kind, const uint8_t* osc, uint32_t len);

    SsGamepad* mGamepad = nullptr;
    OscEgress* mEgress  = nullptr;
    // Origin token held for synchronous emitCb REPLYs during handleGamepadCommand
    // (the Rust callback carries no call ctx). Async device emits broadcast.
    uint32_t   mReplyToken = 0;
};

/*
 * ss_gamepad.h — C ABI for the SuperSonic gamepad subsystem (Rust: gilrs,
 * or Apple's GameController framework on macOS).
 *
 * The C++ engine seam (src/native/GamepadControl) creates one instance,
 * supplying one thread-safe host callback, and forwards decoded "/gamepad/"
 * OSC into it via ss_gamepad_handle_osc(). The subsystem owns its device
 * IO on a dedicated poll thread and never touches the audio thread; results
 * return through the callback, which may fire on the poll thread. Replies
 * ("/gamepad/devices.reply") are emitted synchronously on the caller's thread.
 *
 * Must match rust/supersonic-gamepad/src/ffi.rs. Dual-licensed
 * MIT OR GPL-3.0-or-later (see repo LICENSE).
 */
#ifndef SUPERSONIC_SS_GAMEPAD_H
#define SUPERSONIC_SS_GAMEPAD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque handle. */
typedef struct SsGamepad SsGamepad;

/* `kind` codes for ss_gamepad_emit_fn. Must match supersonic_osc::ffi's
 * EMIT_* values (shared with ss_midi.h). */
#define SS_GAMEPAD_EMIT_BROADCAST 0 /* fan out to the /gamepad/notify audience */
#define SS_GAMEPAD_EMIT_REPLY 1     /* reply to the current caller */

/* Emit an OSC packet to the engine: BROADCAST for "/gamepad/in/" events + the
 * "/gamepad/devices" push, REPLY for "/gamepad/devices.reply". `osc`/`len` are
 * only valid for the duration of the call. May fire on the poll thread. */
typedef void (*ss_gamepad_emit_fn)(void* ctx, int32_t kind, const uint8_t* osc, uint32_t len);

/* Create the subsystem (spawns the poll thread). `ctx` and the callback must
 * outlive it. Returns NULL on failure. */
SsGamepad* ss_gamepad_create(void* ctx, ss_gamepad_emit_fn emit);

/* Stop the poll thread, drop all rumble effects, free the instance. */
void ss_gamepad_destroy(SsGamepad* handle);

/* Feed one decoded "/gamepad/" OSC packet (off the audio thread). */
void ss_gamepad_handle_osc(SsGamepad* handle, const uint8_t* data, uint32_t len);

/* Emit a /gamepad/devices.reply snapshot to the caller (e.g. on new
 * subscription). */
void ss_gamepad_emit_devices(SsGamepad* handle);

#ifdef __cplusplus
}
#endif

#endif /* SUPERSONIC_SS_GAMEPAD_H */

/*
 * ss_midi.h — C ABI for the SuperSonic MIDI subsystem (Rust / midir).
 *
 * The C++ engine seam (src/native/MidiControl) creates one instance, supplying
 * three thread-safe host callbacks, and forwards decoded "/midi/" OSC into it via
 * ss_midi_handle_osc(). The subsystem owns its midir device IO (and midir's input
 * thread) and never touches the audio thread; results return through the
 * callbacks, which may fire on the midir input thread.
 *
 * Must match rust/supersonic-midi/src/ffi.rs. Dual-licensed
 * MIT OR GPL-3.0-or-later (see repo LICENSE).
 */
#ifndef SUPERSONIC_SS_MIDI_H
#define SUPERSONIC_SS_MIDI_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque handle. */
typedef struct SsMidi SsMidi;

/* Transport `kind` codes for ss_midi_transport_fn. */
#define SS_MIDI_TRANSPORT_START 0
#define SS_MIDI_TRANSPORT_CONTINUE 1
#define SS_MIDI_TRANSPORT_STOP 2
#define SS_MIDI_TRANSPORT_POSITION 3

/* `kind` codes for ss_midi_emit_fn. */
#define SS_MIDI_EMIT_BROADCAST 0 /* fan out to the /midi/notify audience */
#define SS_MIDI_EMIT_REPLY 1     /* reply to the current caller */

/* Emit an OSC packet to the engine: BROADCAST for "/midi/in/" events + the
 * "/midi/ports" push, REPLY for "/midi/ports.reply". `osc`/`len` are only valid
 * for the duration of the call. */
typedef void (*ss_midi_emit_fn)(void* ctx, int32_t kind, const uint8_t* osc, uint32_t len);

/* Push a distilled clock-in BPM to SuperClock. */
typedef void (*ss_midi_tempo_fn)(void* ctx, double bpm);

/* Transport intent for SuperClock. `beat` is the target beat for START/POSITION,
 * -1 for CONTINUE/STOP. */
typedef void (*ss_midi_transport_fn)(void* ctx, int32_t kind, double beat);

/* Create the subsystem. `ctx` and the callbacks must outlive it. */
SsMidi* ss_midi_create(void* ctx,
                       ss_midi_emit_fn emit,
                       ss_midi_tempo_fn set_tempo,
                       ss_midi_transport_fn transport);

/* Stop the clock thread, close all ports, free the instance. */
void ss_midi_destroy(SsMidi* handle);

/* Feed one decoded "/midi/" OSC packet (off the audio thread). */
void ss_midi_handle_osc(SsMidi* handle, const uint8_t* data, uint32_t len);

/* Emit a /midi/ports.reply snapshot to the caller (e.g. on new subscription). */
void ss_midi_emit_ports(SsMidi* handle);

/* Re-enumerate devices and broadcast the updated /midi/ports to subscribers.
 * Called from the engine's hotplug listener on a MIDI device add/remove. */
void ss_midi_refresh(SsMidi* handle);

#ifdef __cplusplus
}
#endif

#endif /* SUPERSONIC_SS_MIDI_H */
